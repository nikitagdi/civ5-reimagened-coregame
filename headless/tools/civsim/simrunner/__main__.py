"""python -m civsim.simrunner <command> (run from the repo root with PYTHONPATH=headless\\tools).

  validate <scn|corpus>...                       names against the data profile's gameplay DB, seeds mandatory, no floats   (exit 2 on errors)
  expand <scn|corpus>... [--json] [--dll D]      list scenarios and their sim keys, no run
  run <scn> [--dll golden] [--turns N] [--no-cache]
  batch <corpus|scn>... [--dll golden] [--jobs N|auto] [--repeat K] [--turns N] [--name X] [--no-cache]
  report <batch-dir>                             rewrite report.md/report.json
  show <sim-key|batch-dir>                       result summary
  cache stats
  selftest                                       fake host end to end

common: --host <civhost.exe|fake .py>  --data <profile dir>  --game <Civ5 dir>  --timeout <s per run>
exit: 0 ok, 1 crash/hang/error/repeat mismatch, 2 validation/config
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import tempfile
import time
from pathlib import Path

from .. import common as C
from . import report as R
from . import runner as RN
from . import scenario as S


def _common(p: argparse.ArgumentParser) -> None:
    p.add_argument("--host")
    p.add_argument("--data")
    p.add_argument("--game")
    p.add_argument("--timeout", type=int)
    p.add_argument("--dll", default="golden")


def _ctx(a) -> RN.Ctx:
    return RN.Ctx(host=a.host, data=a.data, game=a.game, timeout_s=a.timeout, log=lambda m: print(m, file=sys.stderr))


def cmd_validate(a) -> int:
    data = C.resolve_data(a.data)
    items = S.resolve_targets(a.targets)
    bad = 0
    for it in items:
        errs, warns = S.validate(it.scenario, data.path)
        if it.scenario.get("turns") is None and not a.turns:
            warns.append("$.turns: not set (pass --turns when running)")
        for w in warns:
            print(f"warn  {it.name}: {w}")
        for e in errs:
            print(f"ERROR {it.name}: {e}")
        bad += bool(errs)
    print(f"{len(items)} scenario(s), {bad} with errors")
    return 2 if bad else 0


def cmd_expand(a) -> int:
    items = S.resolve_targets(a.targets)
    data = C.resolve_data(a.data)
    try:
        host_sha = C.resolve_host(a.host).build_sha256
    except FileNotFoundError:
        host_sha = "0" * 64
    dll = C.resolve_dll(a.dll)
    rows = []
    for it in items:
        t = S.effective_turns(it.scenario, a.turns)
        key = S.sim_key(S.canonical(it.scenario, t or 0), dll.sha256, data.hash, host_sha)
        rows.append({"name": it.name, "key": key, "turns": t, "civs": len(it.scenario.get("civs", [])), "minors": len(it.scenario.get("minors", [])),
                     "map": it.scenario.get("map_script"), "size": it.scenario.get("world_size"), "seeds": it.scenario.get("seeds")})
    if a.json:
        print(json.dumps(rows, indent=1))
    else:
        for r in rows:
            print(f"{r['name']:34} {r['key'][:12]} turns={r['turns']} civs={r['civs']} minors={r['minors']} seeds={r['seeds']} {r['size']}")
    return 0


def _print_result(r: dict) -> None:
    print(f"{r['name']}: {r['status']}{' (cached)' if r.get('cached') else ''} turns {r['turns_done']}/{r['turns_requested']} "
          f"wall {r['wall_ms'] / 1000:.1f}s sim {r['sim_ms'] / 1000:.1f}s final {r.get('final_digest')}")
    print(f"  key {r['key']}\n  run dir {r['run_dir']}")
    if r["status"] != "ok":
        print(f"  failure: {r.get('failure')}")
        for ln in (r.get("stdout_tail") or [])[-10:]:
            print(f"    | {ln}")


def cmd_run(a) -> int:
    ctx = _ctx(a)
    dll = C.resolve_dll(a.dll)
    items = S.resolve_targets([a.scenario])
    if len(items) != 1:
        print("run takes one scenario; use `batch` for corpora", file=sys.stderr)
        return 2
    try:
        r = RN.run_scenario(ctx, items[0], dll, turns=a.turns, use_cache=not a.no_cache)
    except RN.ValidationError as e:
        print("\n".join(e.errors), file=sys.stderr)
        return 2
    _print_result(r)
    return 0 if r["status"] == "ok" else 1


def cmd_batch(a) -> int:
    ctx = _ctx(a)
    dll = C.resolve_dll(a.dll)
    items = S.resolve_targets(a.targets)
    jobs = max(1, (os.cpu_count() or 2) // 2) if a.jobs == "auto" else int(a.jobs)
    name = a.name or "+".join(Path(t).stem for t in a.targets)[:40]
    try:
        batch = RN.run_batch(ctx, items, dll, jobs=jobs, repeat=a.repeat, turns=a.turns, name=name, use_cache=not a.no_cache,
                             args={"targets": a.targets, "dll": a.dll, "jobs": a.jobs, "repeat": a.repeat, "turns": a.turns})
    except RN.ValidationError as e:
        print("\n".join(e.errors), file=sys.stderr)
        return 2
    s = R.summarize(batch)
    print(f"{s['scenarios']} scenario(s) {s['by_status']} cache hits {s['cache_hits']} wall {s['wall_s']}s overhead {s['overhead']}\nreport {batch['dir']}\\report.md")
    return RN.batch_exit_code(batch)


def cmd_report(a) -> int:
    bdir = Path(a.batch_dir)
    batch = C.read_json(bdir / "batch.json")
    R.write(batch, bdir)
    print((bdir / "report.md").read_text(encoding="utf-8"))
    return 0


def cmd_show(a) -> int:
    p = Path(a.what)
    if p.is_dir() and (p / "batch.json").is_file():
        print(R.render_md(C.read_json(p / "batch.json")))
        return 0
    ctx_root = C.OUT / "runs"
    d = ctx_root / "cache" / a.what[:2] / a.what
    if (d / "result.json").is_file():
        print(json.dumps(C.read_json(d / "result.json"), indent=1))
        return 0
    print(f"nothing found for {a.what}", file=sys.stderr)
    return 2


def cmd_cache(a) -> int:
    root = C.OUT / "runs" / "cache"
    n = size = 0
    by: dict[str, int] = {}
    if root.is_dir():
        for res in root.glob("*/*/result.json"):
            n += 1
            try:
                st = json.loads(res.read_text(encoding="utf-8")).get("status", "?")
            except ValueError:
                st = "?"
            by[st] = by.get(st, 0) + 1
        size = sum(f.stat().st_size for f in root.rglob("*") if f.is_file())
    print(f"cache {root}: {n} entries {by}, {size / 1e6:.1f} MB")
    return 0


def cmd_selftest(a) -> int:
    from . import fakehost as F
    with tempfile.TemporaryDirectory(prefix="simrunner-selftest-") as td:
        sb = F.make_sandbox(Path(td))
        C.use_hash_memo(Path(td) / "hashcache.json")
        ctx = RN.Ctx(host=sb["host"], data=sb["data"], runs_root=sb["runs_root"], batches_root=sb["batches_root"])
        dll = C.resolve_dll(sb["dlls"]["good"])
        items = [S.Item(f"s{i}", F.sample_scenario(f"s{i}", seed=i), "") for i in range(1, 4)]
        t0 = time.time()
        b1 = RN.run_batch(ctx, items, dll, jobs=2, repeat=2, name="selftest")
        b2 = RN.run_batch(ctx, items, dll, jobs=2, name="selftest-again")
        ok = (RN.batch_exit_code(b1) == 0 and all(r["cached"] for r in b2["scenarios"]) and not any(r["repeat_mismatch"] for r in b1["scenarios"]))
        crash = RN.run_scenario(ctx, S.Item("c", F.sample_scenario("c"), ""), C.resolve_dll(sb["dlls"]["crash"]))
        ok = ok and crash["status"] == "crash-repro" and crash["last_turn"] == 3
        print(f"selftest {'ok' if ok else 'FAILED'} ({time.time() - t0:.1f}s): 3 runs x2 repeat, cache re-run all hits, crash classified as {crash['status']}")
        return 0 if ok else 1


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="civsim.simrunner", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    v = sub.add_parser("validate")
    v.add_argument("targets", nargs="+")
    v.add_argument("--data")
    v.add_argument("--turns", type=int)
    e = sub.add_parser("expand")
    e.add_argument("targets", nargs="+")
    e.add_argument("--json", action="store_true")
    e.add_argument("--turns", type=int)
    e.add_argument("--host")
    e.add_argument("--data")
    e.add_argument("--dll", default="golden")
    r = sub.add_parser("run")
    r.add_argument("scenario")
    _common(r)
    r.add_argument("--turns", type=int)
    r.add_argument("--no-cache", action="store_true")
    b = sub.add_parser("batch")
    b.add_argument("targets", nargs="+")
    _common(b)
    b.add_argument("--jobs", default="1")
    b.add_argument("--repeat", type=int, default=1)
    b.add_argument("--turns", type=int)
    b.add_argument("--name")
    b.add_argument("--no-cache", action="store_true")
    rp = sub.add_parser("report")
    rp.add_argument("batch_dir")
    sh = sub.add_parser("show")
    sh.add_argument("what")
    ca = sub.add_parser("cache")
    ca.add_argument("op", choices=["stats"])
    sub.add_parser("selftest")
    a = p.parse_args(argv)
    try:
        return {"validate": cmd_validate, "expand": cmd_expand, "run": cmd_run, "batch": cmd_batch, "report": cmd_report, "show": cmd_show,
                "cache": cmd_cache, "selftest": cmd_selftest}[a.cmd](a)
    except (FileNotFoundError, ValueError) as ex:
        print(f"error: {ex}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
