"""Run scenarios through civhost: one process per run, content-addressed cache, process pool, crash classification.

Cache entry: headless/out/runs/cache/<k0k1>/<sim_key>/ with scenario.json, result.json, digest.bin, run.json,
host.log.gz, stdout.log.gz, state-T/, crash dumps, and a .complete marker (written in tmp/ first, then renamed).
"""
from __future__ import annotations

import concurrent.futures as cf
import gzip
import json
import os
import re
import shutil
import subprocess
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable

from .. import common as C
from .. import digestbin
from . import scenario as S

CACHEABLE = ("ok", "crash-repro")
EXIT_NAMES = {1: "setup-error", 2: "check-failed", 4: "unimplemented", 5: "unexpected-call", 6: "lua-error", 7: "stuck"}


class ValidationError(Exception):
    def __init__(self, errors: list[str]):
        super().__init__("; ".join(errors))
        self.errors = errors


class Ctx:
    """Run environment: host binary, data profile, game dir, where runs/batches live."""

    def __init__(self, host: str | None = None, data: str | None = None, game: str | None = None, runs_root: Path | None = None,
                 batches_root: Path | None = None, timeout_s: int | None = None, log: Callable[[str], None] | None = None):
        self.host = C.resolve_host(host)
        self.data = C.resolve_data(data)
        self.game = game or os.environ.get("CIV5_GAME_DIR")
        self.root = Path(runs_root) if runs_root else C.OUT / "runs"
        self.batches_root = Path(batches_root) if batches_root else C.OUT / "batches"
        self.timeout_s = timeout_s
        self.log = log or (lambda m: None)
        self.flags = C.host_flags(self.host)

    def cache_dir(self, key: str) -> Path:
        return self.root / "cache" / key[:2] / key

    def tmp_dir(self, tag: str) -> Path:
        d = self.root / "tmp" / f"{tag}.{os.getpid()}.{threading.get_ident()}.{time.monotonic_ns()}"
        d.parent.mkdir(parents=True, exist_ok=True)
        return d

    def sweep_tmp(self, max_age_s: int = 12 * 3600) -> None:
        t = self.root / "tmp"
        if t.is_dir():
            for d in t.iterdir():
                try:
                    if time.time() - d.stat().st_mtime > max_age_s:
                        shutil.rmtree(d, ignore_errors=True)
                except OSError:
                    pass


@dataclass
class Prepared:
    name: str
    scenario: dict
    canon: dict
    turns: int
    key: str
    dll: C.Dll


def prepare(ctx: Ctx, item: S.Item, dll: C.Dll, turns: int | None = None, check: bool = True) -> Prepared:
    errs, _warns = S.validate(item.scenario, ctx.data.path) if check else ([], [])
    t = S.effective_turns(item.scenario, turns) if isinstance(item.scenario, dict) else None
    if not t:
        errs.append("$.turns: no turn count (set 'turns' in the scenario or pass --turns)")
    if errs:
        raise ValidationError([f"{item.name}: {e}" for e in errs])
    canon = S.canonical(item.scenario, t)
    key = S.sim_key(canon, dll.sha256, ctx.data.hash, ctx.host.build_sha256)
    return Prepared(name=item.name, scenario=item.scenario, canon=canon, turns=t, key=key, dll=dll)


def safe_name(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9._-]+", "_", name).strip("_")[:80] or "run"


# ---------------------------------------------------------------- one process
def _tail(path: Path, n: int = 40) -> list[str]:
    try:
        return path.read_text(encoding="utf-8", errors="replace").splitlines()[-n:]
    except OSError:
        return []


def _gzip_logs(d: Path) -> None:
    for f in list(d.glob("*.log")):
        target = d / ("host.log.gz" if f.name == "civhost.log" else f.name + ".gz")
        try:
            with open(f, "rb") as src, gzip.open(target, "wb", compresslevel=6) as dst:
                shutil.copyfileobj(src, dst)
            f.unlink()
        except OSError:
            pass


def _prune(d: Path) -> None:
    """Drop what the host copies in per run and that is not an artifact: data profile DB copies, SQLite journals."""
    for pat in ("*.db", "*.db-journal", "*.db-wal", "*.db-shm", "map-write.bin"):
        for f in d.glob(pat):
            try:
                f.unlink()
            except OSError:
                pass


def host_argv(ctx: Ctx, prep: Prepared, out_dir: Path, turns: int, scn_path: Path, dump_state_at: int | None = None,
              load: Path | None = None, checkpoint_every: int | None = None, extra: list[str] | None = None) -> tuple[list[str], list[str]]:
    """(argv, unsupported). Env axes become `--axis-name value` flags when the host advertises them."""
    argv = [*ctx.host.cmd, "--dll", str(prep.dll.path), "--data", str(ctx.data.path)]
    if ctx.game:
        argv += ["--game", ctx.game]
    argv += ["--scenario", str(scn_path), "--turns", str(turns), "--out", str(out_dir)]
    unsupported: list[str] = []

    def opt(flag: str, val: Any) -> None:
        if flag in ctx.flags or not ctx.flags:      # empty flag set = host gave no usage text: try anyway
            argv.extend([flag] if val is None else [flag, str(val)])
        else:
            unsupported.append(flag)

    if dump_state_at is not None:
        opt("--dump-state-at", dump_state_at)
    if checkpoint_every:
        opt("--checkpoint-every", checkpoint_every)
    if load is not None:
        opt("--load", load)
    for axis, val in sorted((prep.canon.get("env") or {}).items()):
        opt("--" + axis.replace("_", "-"), val)
    argv += list(extra or [])
    return argv, unsupported


def execute(ctx: Ctx, prep: Prepared, out_dir: Path, *, turns: int | None = None, dump_state_at: int | None = None,
            load: Path | None = None, checkpoint_every: int | None = None, extra: list[str] | None = None,
            affinity_mask: int | None = None, timeout_s: int | None = None) -> dict:
    """Run civhost once into out_dir (created). Always returns a result dict (also written to out_dir/result.json)."""
    out_dir.mkdir(parents=True, exist_ok=True)
    turns = turns or prep.turns
    scn_path = out_dir / "scenario.json"
    C.write_json(scn_path, {**prep.canon, "turns": turns, "name": prep.name})
    argv, unsupported = host_argv(ctx, prep, out_dir, turns, scn_path, dump_state_at, load, checkpoint_every, extra)
    res: dict[str, Any] = {
        "schema": 1, "name": prep.name, "key": prep.key, "status": "error", "failure": None, "turns_requested": turns,
        "turns_done": 0, "last_turn": None, "exit_code": None, "wall_ms": 0.0, "sim_ms": 0.0, "launch_ms": None,
        "dll": {"label": prep.dll.label, "path": str(prep.dll.path), "sha256": prep.dll.sha256},
        "host": {"path": str(ctx.host.path), "build_sha256": ctx.host.build_sha256}, "data_hash": ctx.data.hash,
        "argv": [x.replace(str(out_dir), "<run>") for x in argv], "created": time.strftime("%Y-%m-%dT%H:%M:%S"), "digest_sha256": None, "final_digest": None, "stdout_tail": [],
    }
    if unsupported:
        res.update(status="unsupported", failure=f"host does not support {', '.join(unsupported)}")
        C.write_json(out_dir / "result.json", res)
        return res

    timeout = timeout_s or ctx.timeout_s or (900 + 30 * turns)
    t0 = time.perf_counter()
    timed_out = False
    with open(out_dir / "stdout.log", "wb") as log:
        proc = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
        C.attach_process(proc, affinity_mask)
        try:
            rc = proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            proc.kill()
            rc = proc.wait()
    res["wall_ms"] = round((time.perf_counter() - t0) * 1000, 1)
    res["exit_code"] = rc

    digest_path = out_dir / "digest.bin"
    if digest_path.is_file():
        try:
            d = digestbin.parse(digest_path)
            res["turns_done"] = len(d.turns)
            res["last_turn"] = d.last_turn
            res["digest_sha256"] = C.sha256_bytes(digest_path.read_bytes())
        except ValueError as e:
            res["failure"] = f"bad digest.bin: {e}"
    run_json = out_dir / "run.json"
    if run_json.is_file():
        try:
            rj = C.read_json(run_json)
            res["sim_ms"] = round(sum(t.get("ms", 0.0) for t in rj.get("turns", [])), 2)
            res["launch_ms"] = rj.get("launch_ms")
            res["final_digest"] = rj.get("final_digest")
            res["host_status"] = rj.get("status")
        except (ValueError, OSError):
            pass

    if timed_out:
        res.update(status="hang", failure=f"no exit within {timeout}s")
    elif rc == 0:
        ok = res["turns_done"] >= turns and res.get("host_status", "ok") == "ok"
        res["status"] = "ok" if ok else "error"
        if not ok and not res["failure"]:
            res["failure"] = f"host exit 0 but {res['turns_done']}/{turns} turns in digest.bin"
    elif rc in EXIT_NAMES:
        res.update(status=EXIT_NAMES[rc], failure=f"host exit {rc} ({EXIT_NAMES[rc]})")
    elif rc == 3 or rc < 0 or rc >= 0x80000000:
        res.update(status="crash", failure=f"host exit {rc & 0xFFFFFFFF:#x}")
    else:
        res.update(status="error", failure=f"host exit {rc}")
    if res["status"] != "ok":
        res["stdout_tail"] = _tail(out_dir / "civhost.log") or _tail(out_dir / "stdout.log")
    _prune(out_dir)
    _gzip_logs(out_dir)
    C.write_json(out_dir / "result.json", res)
    return res


# ---------------------------------------------------------------- cache
def cache_lookup(ctx: Ctx, key: str) -> tuple[dict, Path] | None:
    d = ctx.cache_dir(key)
    if (d / ".complete").is_file():
        try:
            r = C.read_json(d / "result.json")
        except (OSError, ValueError):
            return None
        if r.get("status") in CACHEABLE:
            return r, d
    return None


def _publish(ctx: Ctx, key: str, tmp: Path) -> Path:
    final = ctx.cache_dir(key)
    final.parent.mkdir(parents=True, exist_ok=True)
    (tmp / ".complete").write_text(time.strftime("%Y-%m-%dT%H:%M:%S"), encoding="ascii")
    for _ in range(2):
        if final.exists():
            aside = ctx.tmp_dir(key[:12] + ".old")
            try:
                os.replace(final, aside)
            except OSError:
                break
            shutil.rmtree(aside, ignore_errors=True)
        try:
            os.replace(tmp, final)
            return final
        except OSError:
            continue
    shutil.rmtree(tmp, ignore_errors=True)      # lost a race against another publisher of the same key
    return final


def _attempt_summary(r: dict) -> dict:
    return {k: r.get(k) for k in ("status", "failure", "last_turn", "turns_done", "exit_code")}


def run_scenario(ctx: Ctx, item: S.Item, dll: C.Dll, *, turns: int | None = None, use_cache: bool = True,
                 prep: Prepared | None = None) -> dict:
    """Cached run with the one-retry crash policy. Result has `cached` and `run_dir`."""
    prep = prep or prepare(ctx, item, dll, turns)
    if use_cache:
        hit = cache_lookup(ctx, prep.key)
        if hit:
            r, d = hit
            return {**r, "name": prep.name, "cached": True, "run_dir": str(d)}
    tmp = ctx.tmp_dir(prep.key[:12])
    r = execute(ctx, prep, tmp)
    attempts = [_attempt_summary(r)]
    if r["status"] in ("crash", "hang"):
        tmp2 = ctx.tmp_dir(prep.key[:12])
        r2 = execute(ctx, prep, tmp2)
        attempts.append(_attempt_summary(r2))
        if r2["status"] == r["status"] and r2["last_turn"] == r["last_turn"]:
            r2.update(failure_kind=r2["status"], status="crash-repro")
        else:
            r2.update(failure_kind=r["status"], status="crash-flaky" if r2["status"] == "ok" else r2["status"], flaky=True)
        shutil.move(str(tmp), str(tmp2 / "attempt1"))
        tmp, r = tmp2, r2
    r["attempts"] = attempts
    r["name"] = prep.name
    C.write_json(tmp / "result.json", r)
    if r["status"] in CACHEABLE:
        d = _publish(ctx, prep.key, tmp)
    else:
        d = ctx.root / "failed" / f"{C.utc_stamp()}-{prep.key[:8]}-{safe_name(prep.name)}"
        d.parent.mkdir(parents=True, exist_ok=True)
        if d.exists():
            shutil.rmtree(d, ignore_errors=True)
        shutil.move(str(tmp), str(d))
    return {**r, "cached": False, "run_dir": str(d)}


def load_run(ctx: Ctx, key: str) -> dict | None:
    hit = cache_lookup(ctx, key)
    return {**hit[0], "run_dir": str(hit[1])} if hit else None


# ---------------------------------------------------------------- batch
def cost(prep: Prepared) -> int:
    return prep.turns * (len(prep.scenario.get("civs", [])) + len(prep.scenario.get("minors", [])) + 4)


def run_batch(ctx: Ctx, items: list[S.Item], dll: C.Dll, *, jobs: int = 1, repeat: int = 1, turns: int | None = None,
              name: str = "batch", use_cache: bool = True, args: dict | None = None) -> dict:
    """Validate everything first, then run unique keys in a thread pool (each thread owns one civhost process at a time)."""
    t_start = time.perf_counter()
    errs: list[str] = []
    preps: list[Prepared] = []
    for it in items:
        try:
            preps.append(prepare(ctx, it, dll, turns))
        except ValidationError as e:
            errs += e.errors
    if errs:
        raise ValidationError(errs)
    ctx.sweep_tmp()
    bdir = ctx.batches_root / f"{C.utc_stamp()}-{safe_name(name)}"
    bdir.mkdir(parents=True, exist_ok=True)
    lock = threading.Lock()
    logf = open(bdir / "runner.log", "a", encoding="utf-8")

    def log(msg: str) -> None:
        with lock:
            line = f"[{time.strftime('%H:%M:%S')}] {msg}"
            logf.write(line + "\n")
            logf.flush()
            ctx.log(line)

    unique: dict[str, Prepared] = {}
    for p in preps:
        unique.setdefault(p.key, p)
    order = sorted(unique.values(), key=cost, reverse=True)
    t_prep = time.perf_counter() - t_start
    log(f"batch {bdir.name}: {len(preps)} scenario(s), {len(order)} unique, jobs={jobs}, repeat={repeat}, dll={dll.label}")
    results: dict[str, dict] = {}

    def work(p: Prepared) -> None:
        r = run_scenario(ctx, S.Item(p.name, p.scenario, ""), dll, prep=p, use_cache=use_cache)
        reps = []
        if r["status"] == "ok":
            for k in range(2, repeat + 1):
                rd = bdir / "repeats" / f"{safe_name(p.name)}-r{k}"
                rr = execute(ctx, p, rd)
                reps.append({"status": rr["status"], "digest_sha256": rr["digest_sha256"], "wall_ms": rr["wall_ms"]})
                for f in rd.iterdir():          # keep only the small stuff of a repeat
                    if f.name not in ("digest.bin", "run.json", "result.json", "host.log.gz"):
                        shutil.rmtree(f, ignore_errors=True) if f.is_dir() else f.unlink()
        r["repeats"] = reps
        r["repeat_mismatch"] = any(x["digest_sha256"] != r["digest_sha256"] for x in reps)
        with lock:
            results[p.key] = r
        log(f"{p.name}: {r['status']}{' (cached)' if r['cached'] else ''} turns={r['turns_done']}/{r['turns_requested']} "
            f"wall={r['wall_ms'] / 1000:.1f}s{' REPEAT-MISMATCH' if r['repeat_mismatch'] else ''}")

    interrupted = False
    ex = cf.ThreadPoolExecutor(max_workers=max(1, jobs))
    try:
        futs = [ex.submit(work, p) for p in order]
        for f in futs:
            f.result()
    except KeyboardInterrupt:
        interrupted = True
        ex.shutdown(wait=False, cancel_futures=True)
        log("interrupted")
    finally:
        ex.shutdown(wait=not interrupted)
    rows = []
    for p in preps:
        r = results.get(p.key)
        if r is None:
            r = {"status": "interrupted", "key": p.key, "cached": False, "turns_done": 0, "turns_requested": p.turns, "wall_ms": 0, "sim_ms": 0,
                 "repeat_mismatch": False, "repeats": [], "run_dir": None}
        rows.append({**{k: r.get(k) for k in ("status", "failure", "key", "cached", "turns_requested", "turns_done", "last_turn", "wall_ms", "sim_ms",
                                              "launch_ms", "final_digest", "digest_sha256", "run_dir", "repeat_mismatch", "repeats", "stdout_tail",
                                              "attempts", "flaky")},
                     "name": p.name})
    batch = {"schema": 1, "name": bdir.name, "dir": str(bdir), "created": time.strftime("%Y-%m-%dT%H:%M:%S"), "args": args or {},
             "dll": {"label": dll.label, "sha256": dll.sha256}, "host_build_sha256": ctx.host.build_sha256, "data_hash": ctx.data.hash,
             "jobs": jobs, "repeat": repeat, "prep_s": round(t_prep, 2), "wall_s": round(time.perf_counter() - t_start, 2),
             "interrupted": interrupted, "scenarios": rows}
    C.write_json(bdir / "batch.json", batch)
    from . import report
    report.write(batch, bdir)
    logf.close()
    return batch


def batch_exit_code(batch: dict) -> int:
    bad = [r for r in batch["scenarios"] if r["status"] not in ("ok",) or r.get("repeat_mismatch")]
    return 1 if bad else 0
