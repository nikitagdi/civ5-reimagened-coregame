"""Dynamic determinism checks on top of simrunner: digest, check, equiv, bisect, saveload, perturb (V1 cut).

Exit codes: 0 equal/deterministic, 1 run failure, 2 config, 6 divergence found. Reports go to headless/out/detlab/<stamp>-<kind>-<name>/.
Python replaces civdiff.exe for the MVP (digest.bin is small); the CLI contract (exit 6, --json) is the same.
"""
from __future__ import annotations

import argparse
import concurrent.futures as cf
import copy
import json
import re
import sys
from pathlib import Path
from typing import Any

from .. import common as C
from .. import digestbin as D
from .. import rngbin as RB
from ..simrunner import runner as RN
from ..simrunner import scenario as S

AXIS_CLASS = {"heap_pattern": "C2", "stack_pattern": "C2", "addr_shift": "C3", "fpu_cw": "C4", "active_player": "C5", "async_seed": "C5",
              "lua_random": "C5", "clock_offset": "C7", "isolation": "C8",
              # civhost flags (--heap-fill, --stack-fill, --heap-shift, --heap-jitter, --fpu-pc): the axis name is the flag with '_' for '-'
              "heap_fill": "C2", "stack_fill": "C2", "heap_shift": "C3", "heap_jitter": "C3", "fpu_pc": "C4", "fpu_pc_global": "C4"}


# ---------------------------------------------------------------- helpers
def _add_common(p: argparse.ArgumentParser) -> None:
    p.add_argument("--host")
    p.add_argument("--data")
    p.add_argument("--game")
    p.add_argument("--timeout", type=int)
    p.add_argument("--turns", type=int, help="override the scenario turn count")
    p.add_argument("--jobs", type=int, default=1)
    p.add_argument("--out", help="report directory (default headless/out/detlab/<stamp>-<kind>-<name>)")


def _ctx(a) -> RN.Ctx:
    return RN.Ctx(host=a.host, data=a.data, game=a.game, timeout_s=a.timeout, log=lambda m: print(m, file=sys.stderr))


def _report_dir(a, kind: str, name: str) -> Path:
    d = Path(a.out) if getattr(a, "out", None) else C.OUT / "detlab" / f"{C.utc_stamp()}-{kind}-{RN.safe_name(name)[:40]}"
    d.mkdir(parents=True, exist_ok=True)
    return d


def load_digest(spec: str) -> D.Digest:
    """digest.bin path, a run dir, or a 64-hex sim key of a cached run."""
    p = Path(spec)
    if p.is_dir():
        p = p / "digest.bin"
    elif not p.exists() and re.fullmatch(r"[0-9a-f]{64}", spec):
        p = C.OUT / "runs" / "cache" / spec[:2] / spec / "digest.bin"
    return D.parse(p)


def _digest_in(run: dict) -> D.Digest:
    return D.parse(Path(run["run_dir"]) / "digest.bin")


def _cmp_runs(ra: dict, rb: dict) -> dict:
    return D.compare(_digest_in(ra), _digest_in(rb))


def _one(items: list[S.Item], what: str) -> S.Item:
    if len(items) != 1:
        raise ValueError(f"{what} takes exactly one scenario (got {len(items)}); pick one file")
    return items[0]


def hex_context(a: bytes, b: bytes, off: int, radius: int = 32) -> dict:
    lo = max(0, off - radius)
    return {"start": lo, "caret": off - lo, "a": a[lo:off + radius].hex(" "), "b": b[lo:off + radius].hex(" ")}


def byte_diff(a: bytes, b: bytes) -> dict:
    n = min(len(a), len(b))
    off = next((i for i in range(n) if a[i] != b[i]), None)
    if off is None and len(a) != len(b):
        off = n
    return {"size_a": len(a), "size_b": len(b), "first_diff_offset": off,
            "n_diff_bytes": sum(1 for i in range(n) if a[i] != b[i]) + abs(len(a) - len(b)),
            "context": hex_context(a, b, off) if off is not None else None}


def _write_json_md(d: Path, stem: str, obj: dict, md: str) -> None:
    C.write_json(d / f"{stem}.json", obj)
    (d / f"{stem}.md").write_text(md, encoding="utf-8")


# ---------------------------------------------------------------- digest A B
def cmd_digest(a) -> int:
    da, db = load_digest(a.a), load_digest(a.b)
    r = D.compare(da, db)
    if a.json:
        print(json.dumps(r, indent=1))
    else:
        if r["identical"]:
            print(f"equal: {r['common_turns']} turns, {len(da.segments)} segments")
        elif r["first_divergent_turn"] is not None:
            print(f"DIVERGENCE at turn {r['first_divergent_turn']}: {', '.join(r['divergent_segments'])} "
                  f"({r['n_divergent_turns']} divergent turn(s) of {r['common_turns']} common)")
        else:
            print(f"no divergence over {r['common_turns']} common turns, but turn sets differ: a={r['turns_a']} b={r['turns_b']}"
                  f"{' (segment tables differ)' if r['segments_mismatch'] else ''}")
    return 0 if r["identical"] else (6 if r["first_divergent_turn"] is not None or r["segments_mismatch"] else 1)


# ---------------------------------------------------------------- check
def cmd_check(a) -> int:
    ctx = _ctx(a)
    dll = C.resolve_dll(a.dll)
    items = S.resolve_targets(a.targets)
    try:
        batch = RN.run_batch(ctx, items, dll, jobs=a.jobs, repeat=a.repeat, turns=a.turns, name="check-" + "+".join(Path(t).stem for t in a.targets)[:30])
    except RN.ValidationError as e:
        print("\n".join(e.errors), file=sys.stderr)
        return 2
    rows = []
    for r in batch["scenarios"]:
        row = {"name": r["name"], "status": r["status"], "repeats": 1 + len(r["repeats"]), "deterministic": None, "first_divergent_turn": None,
               "divergent_segments": []}
        if r["status"] == "ok":
            row["deterministic"] = not r["repeat_mismatch"]
            if r["repeat_mismatch"]:
                base = D.parse(Path(r["run_dir"]) / "digest.bin")
                for k in range(2, a.repeat + 1):
                    other = Path(batch["dir"]) / "repeats" / f"{RN.safe_name(r['name'])}-r{k}" / "digest.bin"
                    if other.is_file():
                        c = D.compare(base, D.parse(other))
                        if not c["identical"]:
                            row.update(first_divergent_turn=c["first_divergent_turn"], divergent_segments=c["divergent_segments"])
                            break
        rows.append(row)
    d = _report_dir(a, "check", "+".join(Path(t).stem for t in a.targets))
    failed = [r for r in rows if r["status"] != "ok"]
    nondet = [r for r in rows if r["deterministic"] is False]
    out = {"schema": 1, "dll": {"label": dll.label, "sha256": dll.sha256}, "repeat": a.repeat, "batch": batch["dir"], "scenarios": rows,
           "all_deterministic": not failed and not nondet}
    md = ["# determinism check", "", f"dll `{dll.label}`, {a.repeat} runs per scenario, batch `{batch['dir']}`", "",
          "| scenario | status | deterministic | first divergent turn | segments |", "|---|---|---|---|---|"]
    md += [f"| {r['name']} | {r['status']} | {r['deterministic']} | {r['first_divergent_turn'] or '-'} | {', '.join(r['divergent_segments']) or '-'} |" for r in rows]
    _write_json_md(d, "check", out, "\n".join(md) + "\n")
    print(f"check: {len(rows)} scenario(s), {len(failed)} run failure(s), {len(nondet)} non-deterministic; report {d}")
    for r in nondet:
        print(f"  NON-DETERMINISTIC {r['name']}: turn {r['first_divergent_turn']} {r['divergent_segments']}")
    return 6 if nondet else (1 if failed else 0)


# ---------------------------------------------------------------- equiv
def equiv(ctx: RN.Ctx, items: list[S.Item], dll_a: C.Dll, dll_b: C.Dll, turns: int | None, jobs: int) -> dict:
    ba = RN.run_batch(ctx, items, dll_a, jobs=jobs, turns=turns, name=f"equiv-{dll_a.label}")
    bb = RN.run_batch(ctx, items, dll_b, jobs=jobs, turns=turns, name=f"equiv-{dll_b.label}")
    rows = []
    for ra, rb in zip(ba["scenarios"], bb["scenarios"]):
        row = {"name": ra["name"], "key_a": ra["key"], "key_b": rb["key"], "status_a": ra["status"], "status_b": rb["status"], "turns": ra["turns_requested"],
               "equal": False, "first_divergent_turn": None, "divergent_segments": [], "n_divergent_turns": 0}
        if ra["status"] == "ok" and rb["status"] == "ok":
            c = D.compare(D.parse(Path(ra["run_dir"]) / "digest.bin"), D.parse(Path(rb["run_dir"]) / "digest.bin"))
            row.update(equal=c["identical"], first_divergent_turn=c["first_divergent_turn"], divergent_segments=c["divergent_segments"],
                       n_divergent_turns=c["n_divergent_turns"])
        rows.append(row)
    return {"schema": 1, "dll_a": {"label": dll_a.label, "sha256": dll_a.sha256}, "dll_b": {"label": dll_b.label, "sha256": dll_b.sha256},
            "host_build_sha256": ctx.host.build_sha256, "data_hash": ctx.data.hash, "created": C.utc_stamp(), "scenarios": rows,
            "all_equal": all(r["equal"] for r in rows), "diverged": [r["name"] for r in rows if r["first_divergent_turn"] is not None],
            "failed": [r["name"] for r in rows if "ok" not in (r["status_a"], r["status_b"]) or r["status_a"] != "ok" or r["status_b"] != "ok"]}


def find_equivalence(sha_a: str, sha_b: str) -> dict | None:
    """Newest equivalence.json in headless/out/detlab for this exact DLL pair (perf-lab gate uses it)."""
    for f in sorted((C.OUT / "detlab").glob("*/equivalence.json"), reverse=True):
        try:
            e = C.read_json(f)
        except (OSError, ValueError):
            continue
        if e["dll_a"]["sha256"] == sha_a and e["dll_b"]["sha256"] == sha_b:
            return {**e, "path": str(f)}
    return None


def cmd_equiv(a) -> int:
    ctx = _ctx(a)
    dll_a, dll_b = C.resolve_dll(a.dll), C.resolve_dll(a.vs)
    items = S.resolve_targets(a.targets)
    try:
        e = equiv(ctx, items, dll_a, dll_b, a.turns, a.jobs)
    except RN.ValidationError as ex:
        print("\n".join(ex.errors), file=sys.stderr)
        return 2
    d = _report_dir(a, "equiv", f"{'+'.join(Path(t).stem for t in a.targets)}-{dll_a.label}-vs-{dll_b.label}")
    md = [f"# equivalence {dll_a.label} vs {dll_b.label}", "", "| scenario | a | b | equal | first divergent turn | segments |", "|---|---|---|---|---|---|"]
    md += [f"| {r['name']} | {r['status_a']} | {r['status_b']} | {r['equal']} | {r['first_divergent_turn'] or '-'} | {', '.join(r['divergent_segments']) or '-'} |"
           for r in e["scenarios"]]
    _write_json_md(d, "equivalence", e, "\n".join(md) + "\n")
    print(f"equiv {dll_a.label} vs {dll_b.label}: {sum(r['equal'] for r in e['scenarios'])}/{len(e['scenarios'])} equal; {d}\\equivalence.json")
    for n in e["diverged"]:
        print(f"  DIVERGED {n}")
    return 6 if e["diverged"] else (0 if e["all_equal"] else 1)


# ---------------------------------------------------------------- bisect
def bisect(ctx: RN.Ctx, item: S.Item, dll_a: C.Dll, dll_b: C.Dll, turns: int | None, rdir: Path) -> dict:
    """Two DLLs on one scenario (golden vs optimized). Same DLL twice = determinism bisect (side B is a fresh process)."""
    return bisect_sides(ctx, (item, dll_a), (item, dll_b), turns, rdir)


def bisect_sides(ctx: RN.Ctx, side_a: tuple[S.Item, C.Dll], side_b: tuple[S.Item, C.Dll], turns: int | None, rdir: Path) -> dict:
    """First divergent turn -> segment -> byte offset between two (scenario, DLL) runs; sides may differ in DLL (A/B) or in scenario (perturb)."""
    (item, dll_a), (item_b, dll_b) = side_a, side_b
    prep_a, prep_b = RN.prepare(ctx, item, dll_a, turns), RN.prepare(ctx, item_b, dll_b, turns)
    ra = RN.run_scenario(ctx, item, dll_a, turns=turns, prep=prep_a)
    if prep_a.key == prep_b.key:         # identical inputs: a determinism bisect, side B must be a fresh process, not the cache hit
        rb = RN.execute(ctx, prep_b, rdir / "run-b", turns=turns)
        rb["run_dir"] = str(rdir / "run-b")
    else:
        rb = RN.run_scenario(ctx, item_b, dll_b, turns=turns, prep=prep_b)
    out: dict[str, Any] = {"schema": 1, "scenario": item.name if item.name == item_b.name else f"{item.name} vs {item_b.name}",
                           "dll_a": dll_a.label, "dll_b": dll_b.label, "status": None, "notes": [],
                           "first_divergent_turn": None, "divergent_segments": [], "segments": [], "keys": [ra["key"], rb["key"]]}
    if ra["status"] != "ok" or rb["status"] != "ok":
        out.update(status="run-failed", notes=[f"a: {ra['status']} {ra.get('failure')}", f"b: {rb['status']} {rb.get('failure')}"])
        return out
    cmp1 = _cmp_runs(ra, rb)
    out["compare"] = cmp1
    if cmp1["identical"]:
        out["status"] = "equal"
        return out
    t = cmp1["first_divergent_turn"]
    if t is None:
        out.update(status="run-failed", notes=["digests differ only in length/segment table; no divergent hash"])
        return out
    out.update(status="divergent", first_divergent_turn=t, divergent_segments=cmp1["divergent_segments"])
    if ctx.flags and "--dump-state-at" not in ctx.flags:
        out["notes"].append("host has no --dump-state-at: stopped at turn + segment level")
        return out
    # Rerun both sides to the divergent turn with the raw segment bytes dumped (in parallel). A deterministic divergence reproduces; a
    # nondeterministic one may show up elsewhere or earlier, so follow the rerun's own first divergent turn (max 3 rounds).
    t_dump, segs, cmp2 = t, cmp1["divergent_segments"], None
    rng_flags = ["--rng-digest"] if "--rng-digest" in ctx.flags else []    # per-phase RNG stream hashes (diag DLLs; ignored by others)
    for _round in range(3):
        with cf.ThreadPoolExecutor(2) as ex:
            futs = {side: ex.submit(RN.execute, ctx, RN.prepare(ctx, it, dll, turns=t_dump), rdir / side, turns=t_dump, dump_state_at=t_dump,
                                    extra=rng_flags)
                    for side, it, dll in (("a", item, dll_a), ("b", item_b, dll_b))}
            redo = {k: f.result() for k, f in futs.items()}
        if redo["a"]["status"] != "ok" or redo["b"]["status"] != "ok":
            out["notes"].append(f"state rerun failed: a={redo['a']['status']} b={redo['b']['status']}")
            return out
        cmp2 = D.compare(D.parse(rdir / "a" / "digest.bin"), D.parse(rdir / "b" / "digest.bin"))
        if cmp2["first_divergent_turn"] is None:
            segs = []
            break
        segs = cmp2["divergent_segments"]
        if cmp2["first_divergent_turn"] == t_dump:
            break
        t_dump = cmp2["first_divergent_turn"]
    assert cmp2 is not None
    out["reproduced_on_rerun"] = cmp2["first_divergent_turn"] == t and cmp2["divergent_segments"] == cmp1["divergent_segments"]
    out["dump_turn"] = t_dump
    # Heisenberg check: dumping state must not change the digests of the original runs (compared over the turns both cover)
    out["dump_perturbs_digest"] = not (D.compare(D.parse(rdir / "a" / "digest.bin"), _digest_in(ra))["equal"]
                                       and D.compare(D.parse(rdir / "b" / "digest.bin"), _digest_in(rb))["equal"])
    if not out["reproduced_on_rerun"]:
        out["notes"].append("the rerun with state dump did not reproduce the same divergence" + (
            " (the rerun pair was identical)" if cmp2["first_divergent_turn"] is None else " (it diverged elsewhere; segments below are from the rerun)")
            + ": the digest differs between processes without a DLL change = nondeterminism, suspect classes C2/C3/C8; run `check`, then `perturb`")
    for seg in segs:
        fa, fb = rdir / "a" / f"state-{t_dump}" / f"{seg}.bin", rdir / "b" / f"state-{t_dump}" / f"{seg}.bin"
        if not (fa.is_file() and fb.is_file()):
            out["segments"].append({"name": seg, "error": "no state dump for this segment"})
            continue
        out["segments"].append({"name": seg, **byte_diff(fa.read_bytes(), fb.read_bytes())})
    first = next((s for s in out["segments"] if s.get("first_diff_offset") is not None), None)
    out["first_segment"] = first["name"] if first else None
    _rng_level(ctx, (item, dll_a), (item_b, dll_b), rdir, t_dump, out)
    return out


def _rng_level(ctx: RN.Ctx, side_a: tuple[S.Item, C.Dll], side_b: tuple[S.Item, C.Dll], rdir: Path, t_dump: int, out: dict) -> None:
    """First differing RNG call (diag / optimized-diag DLLs on both sides): per-phase stream digest of the state rerun, then the call log
    of the first differing phase. Adds out["rng"]; a missing capability only adds a note."""
    da, db = rdir / "a" / "rng-digest.bin", rdir / "b" / "rng-digest.bin"
    if not (da.is_file() and db.is_file()):
        out["notes"].append("RNG level skipped: no rng-digest.bin on both sides (" + (
            "the host has no --rng-digest" if "--rng-digest" not in ctx.flags else
            "a DLL without HeadlessDiagInstall: use diag / optimized-diag builds on both sides to get the first differing random call") + ")")
        return
    pa, pb = RB.parse_digest(da), RB.parse_digest(db)
    dv = RB.digest_divergence(pa, pb)
    rng: dict[str, Any] = {"phases_compared": min(len(pa), len(pb)), "digest_divergence": dv, "first_call": None}
    out["rng"] = rng
    if dv is None:
        out["notes"].append(f"RNG streams are identical in every phase up to turn {t_dump} although the state differs: the divergence is not a different "
                            "RNG call sequence (suspect uninitialised memory, addresses or float precision acting on non-RNG state)")
        return
    tr = dv["turn"]
    if "--rng-log" not in ctx.flags:
        out["notes"].append("host has no --rng-log: stopped at the per-phase RNG digest")
        return
    window, turns = ("pre", 1) if tr < 0 else (f"{tr}-{tr}", tr + 1)
    with cf.ThreadPoolExecutor(2) as ex:
        futs = {side: ex.submit(RN.execute, ctx, RN.prepare(ctx, it, dll, turns=turns), rdir / f"rng-{side}", turns=turns, extra=["--rng-log", window])
                for side, (it, dll) in (("a", side_a), ("b", side_b))}
        redo = {k: f.result() for k, f in futs.items()}
    if redo["a"]["status"] != "ok" or redo["b"]["status"] != "ok":
        out["notes"].append(f"RNG log rerun failed: a={redo['a']['status']} b={redo['b']['status']}")
        return
    la, lb = rdir / "rng-a" / "rng.log.bin", rdir / "rng-b" / "rng.log.bin"
    if la.is_file() and lb.is_file():
        rng["window"] = window
        rng["first_call"] = RB.log_divergence(la, lb)


def _render_rng(r: dict) -> list[str]:
    rng = r.get("rng")
    if not rng:
        return []
    dv = rng.get("digest_divergence")
    if dv is None:
        return ["## RNG level", "", f"RNG streams identical in all {rng['phases_compared']} phases compared.", ""]
    phase = "pregame (game creation / load)" if dv["turn"] < 0 else f"turn {dv['turn']}"
    md = ["## RNG level", "", f"- first differing RNG phase: **{phase}**, streams: {', '.join(dv['kinds']) or '-'} ({dv['reason']})"]
    for side in ("a", "b"):
        for kind, k in (dv.get(side) or {}).items():
            md.append(f"  - {side} {kind}: calls {k['calls']}, reseeds {k['reseeds']}, first seed {k['first_seed']}, last seed {k['last_seed']}, hash {k['hash']:016x}")
    fc = rng.get("first_call")
    if fc:
        md.append(f"- call log window {rng.get('window')}: {fc['calls_a']} / {fc['calls_b']} calls; " + (
            "identical" if fc["reason"] == "identical" else f"first difference at call index {fc['first_diff_index']} ({fc['reason']})"))
        if fc.get("a"):
            md += ["", "```", "side  idx   turn slice zone kind  range result seed_before   site (function, log text)"]
            for side in ("a", "b"):
                for j, c in enumerate(fc[side]):
                    idx = fc["context_start"] + j
                    mark = ">>" if idx == fc["first_diff_index"] else "  "
                    md.append(f"{mark}{side}  {idx:<5} {c['turn']:<4} {c['slice']:<5} {str(c['zone']):<4} {c['kind']:<5} {c['range']:<5} {c['result']:<6} {c['seed_before']:<12} "
                              f"{c['function'] or '?'} [{c['log'] or ''}]" + (f" {Path(c['file']).name}:{c['line']}" if c.get("file") else ""))
                md.append("")
            md.append("```")
    md.append("")
    return md


def render_bisect_md(r: dict) -> str:
    md = [f"# Divergence report: {r['scenario']}", "", f"`{r['dll_a']}` vs `{r['dll_b']}`: **{r['status']}**", ""]
    if r["status"] == "divergent":
        md += [f"- first divergent turn: **{r['first_divergent_turn']}**", f"- divergent segments at that turn: {', '.join(r['divergent_segments'])}",
               f"- divergent turns overall: {r['compare']['n_divergent_turns']} of {r['compare']['common_turns']}"]
        if "reproduced_on_rerun" in r:
            md.append(f"- reproduced on rerun with state dump: {r['reproduced_on_rerun']} (state compared at turn {r['dump_turn']})")
        md.append("")
        for s in r["segments"]:
            if s.get("error"):
                md.append(f"## {s['name']}\n\n{s['error']}\n")
                continue
            if s["first_diff_offset"] is None:
                md.append(f"## {s['name']}\n\nthe two state dumps are byte-identical although the original digests differed: "
                          f"the divergence did not reproduce on rerun (nondeterminism between processes)\n")
                continue
            md += [f"## {s['name']}", "", f"first differing byte: offset {s['first_diff_offset']} (0x{s['first_diff_offset']:x}), "
                   f"{s['n_diff_bytes']} differing byte(s), sizes {s['size_a']} / {s['size_b']}", ""]
            if s.get("context"):
                c = s["context"]
                md += ["```", f"context from offset {c['start']} (0x{c['start']:x}), +-32 bytes around the first difference",
                       f"a: {c['a']}", f"b: {c['b']}", "   " + "   " * c["caret"] + "^^ first difference", "```", ""]
        md += _render_rng(r)
    for n in r["notes"]:
        md.append(f"- note: {n}")
    return "\n".join(md) + "\n"


def cmd_bisect(a) -> int:
    ctx = _ctx(a)
    dll_a, dll_b = C.resolve_dll(a.dll), C.resolve_dll(a.vs)
    item = _one(S.resolve_targets([a.scenario]), "bisect")
    rdir = _report_dir(a, "bisect", f"{item.name}-{dll_a.label}-vs-{dll_b.label}")
    try:
        r = bisect(ctx, item, dll_a, dll_b, a.turns, rdir)
    except RN.ValidationError as ex:
        print("\n".join(ex.errors), file=sys.stderr)
        return 2
    _write_json_md(rdir, "divergence", r, render_bisect_md(r))
    if r["status"] == "equal":
        print("no divergence")
        return 0
    if r["status"] == "run-failed":
        print("run failed: " + "; ".join(r["notes"]), file=sys.stderr)
        return 1
    print(f"DIVERGENCE at turn {r['first_divergent_turn']}, segments {r['divergent_segments']}"
          + (f", first differing segment {r.get('first_segment')}" if r.get("first_segment") else "") + f"\nreport {rdir}\\divergence.md")
    return 6


# ---------------------------------------------------------------- saveload
def _find_ckpt(d: Path, turn: int) -> Path | None:
    cands = [f for f in d.rglob("*") if f.is_file() and f.suffix in (".ckpt", ".civsimsave", ".civcheckpoint") or f.name.endswith(".ckpt")]
    pref = [f for f in cands if re.search(rf"(?<!\d)0*{turn}(?!\d)", f.stem)]
    return (pref or cands or [None])[0]


def saveload(ctx: RN.Ctx, item: S.Item, dll: C.Dll, at: int, until: int, rdir: Path) -> dict:
    out: dict[str, Any] = {"schema": 1, "scenario": item.name, "dll": dll.label, "at": at, "until": until, "status": None, "notes": []}
    if ctx.flags and not ({"--checkpoint-every", "--load"} <= ctx.flags):
        out.update(status="skipped", notes=["host does not support --checkpoint-every/--load yet; nothing compared"])
        return out
    ref = RN.run_scenario(ctx, item, dll, turns=until)
    if ref["status"] != "ok":
        out.update(status="run-failed", notes=[f"reference run: {ref['status']} {ref.get('failure')}"])
        return out
    prep = RN.prepare(ctx, item, dll, turns=until)
    b1 = RN.execute(ctx, prep, rdir / "save", turns=at, checkpoint_every=at)
    ck = _find_ckpt(rdir / "save", at) if b1["status"] == "ok" else None
    if ck is None:
        out.update(status="run-failed", notes=[f"save phase: {b1['status']}; no checkpoint for turn {at} found in {rdir / 'save'}"])
        return out
    b2 = RN.execute(ctx, prep, rdir / "load", turns=until - at, load=ck)
    if b2["status"] != "ok":
        out.update(status="run-failed", notes=[f"load phase: {b2['status']} {b2.get('failure')}"])
        return out
    dref = D.parse(Path(ref["run_dir"]) / "digest.bin")
    c1 = D.compare(dref, D.parse(rdir / "save" / "digest.bin"))
    c2 = D.compare(dref, D.parse(rdir / "load" / "digest.bin"))
    out.update(status="equal" if c1["equal"] and c2["equal"] and c2["common_turns"] >= until - at else "divergent", checkpoint=str(ck),
               first_turn_phase=c1, load_phase=c2, first_divergent_turn=c2["first_divergent_turn"], divergent_segments=c2["divergent_segments"])
    if c2["common_turns"] < until - at:
        out["notes"].append(f"load phase covers {c2['common_turns']} turns, expected {until - at}")
    return out


def cmd_saveload(a) -> int:
    ctx = _ctx(a)
    dll = C.resolve_dll(a.dll)
    item = _one(S.resolve_targets([a.scenario]), "saveload")
    if not 0 < a.at < a.until:
        print("need 0 < --at < --until", file=sys.stderr)
        return 2
    rdir = _report_dir(a, "saveload", item.name)
    try:
        r = saveload(ctx, item, dll, a.at, a.until, rdir)
    except RN.ValidationError as ex:
        print("\n".join(ex.errors), file=sys.stderr)
        return 2
    C.write_json(rdir / "saveload.json", r)
    print(f"saveload {item.name} 0->{a.until} vs 0->{a.at} + load -> {a.until}: {r['status']}" + ("".join(f"\n  {n}" for n in r["notes"])))
    if r["status"] == "divergent":
        print(f"  first divergent turn {r['first_divergent_turn']} segments {r['divergent_segments']}  (class C1 suspect: state not in Write/Read)")
    return {"equal": 0, "skipped": 0, "run-failed": 1, "divergent": 6}[r["status"]]


# ---------------------------------------------------------------- perturb
def _coerce(v: str) -> int | str:
    return int(v) if re.fullmatch(r"-?\d+", v) else v


def _variant(item: S.Item, axis: str, v: str) -> S.Item:
    scn = copy.deepcopy(item.scenario)
    if axis == "active_player" and isinstance(_coerce(v), int):
        scn["active_player"] = _coerce(v)      # a scenario field of the host, not a flag
    else:
        scn.setdefault("env", {})[axis] = _coerce(v)
    return S.Item(f"{item.name}[{axis}={v}]", scn, "")


def _compact_segments(segs: list[str]) -> str:
    """['player00', ..., 'player63', 'map'] -> 'player00..player63 (64), map' (runs of numbered segments of one kind collapsed)."""
    out: list[str] = []
    i = 0
    while i < len(segs):
        m = re.fullmatch(r"([a-z]+)(\d+)", segs[i])
        j = i
        if m:
            while j + 1 < len(segs) and re.fullmatch(rf"{m.group(1)}\d+", segs[j + 1]):
                j += 1
        out.append(segs[i] if j - i < 3 else f"{segs[i]}..{segs[j]} ({j - i + 1})")
        if j - i < 3:
            out.extend(segs[i + 1:j + 1])
        i = j + 1
    return "[" + ", ".join(out) + "]"


def perturb(ctx: RN.Ctx, item: S.Item, dll: C.Dll, axes: list[tuple[str, list[str]]], turns: int | None, jobs: int,
            bisect_dir: Path | None = None) -> dict:
    jobs_list: list[tuple[str, str]] = []
    unsupported: dict[str, str] = {}
    for axis, vals in axes:
        flag = "--" + axis.replace("_", "-")
        if axis != "active_player" and ctx.flags and flag not in ctx.flags:
            unsupported[axis] = f"host has no {flag}"
            continue
        jobs_list += [(axis, v) for v in vals]

    def one(av: tuple[str, str]) -> tuple[tuple[str, str], dict]:
        return av, RN.run_scenario(ctx, _variant(item, av[0], av[1]), dll, turns=turns)

    with cf.ThreadPoolExecutor(max_workers=max(1, jobs)) as ex:
        runs = dict(ex.map(one, jobs_list))
    rows = []
    for axis, vals in axes:
        if axis in unsupported:
            rows.append({"axis": axis, "class": AXIS_CLASS.get(axis), "status": "unsupported", "note": unsupported[axis], "values": vals})
            continue
        base_v = vals[0]
        base = runs[(axis, base_v)]
        entries = []
        verdict = "insensitive"
        for v in vals:
            r = runs[(axis, v)]
            e: dict[str, Any] = {"value": v, "status": r["status"], "final_digest": r.get("final_digest"), "key": r["key"]}
            if r["status"] != "ok" or base["status"] != "ok":
                verdict = "run-failed" if verdict == "insensitive" else verdict
            elif v != base_v:
                c = _cmp_runs(base, r)
                e.update(equal=c["identical"], first_divergent_turn=c["first_divergent_turn"], divergent_segments=c["divergent_segments"])
                if not c["identical"]:
                    verdict = "sensitive"
            entries.append(e)
        row = {"axis": axis, "class": AXIS_CLASS.get(axis), "baseline": base_v, "status": verdict, "values": entries}
        if verdict == "sensitive" and bisect_dir is not None:      # byte-level localization of the first sensitive value
            v = next(e["value"] for e in entries if e.get("equal") is False)
            bd = bisect_dir / f"{axis}-{RN.safe_name(v)}"
            bd.mkdir(parents=True, exist_ok=True)
            rep = bisect_sides(ctx, (_variant(item, axis, base_v), dll), (_variant(item, axis, v), dll), turns, bd)
            _write_json_md(bd, "divergence", rep, render_bisect_md(rep))
            row["bisect"] = {"value": v, "report": str(bd / "divergence.md"), "first_segment": rep.get("first_segment"),
                             "segments": [{k: x.get(k) for k in ("name", "first_diff_offset", "n_diff_bytes")} for x in rep.get("segments", [])]}
        rows.append(row)
    return {"schema": 1, "scenario": item.name, "dll": dll.label, "axes": rows, "sensitive": [r["axis"] for r in rows if r["status"] == "sensitive"],
            "failed": [r["axis"] for r in rows if r["status"] == "run-failed"]}


def cmd_perturb(a) -> int:
    ctx = _ctx(a)
    dll = C.resolve_dll(a.dll)
    item = _one(S.resolve_targets([a.scenario]), "perturb")
    axes = []
    for spec in a.axis:
        name, _, vals = spec.partition("=")
        if not vals:
            print(f"--axis needs name=v1,v2 (got {spec})", file=sys.stderr)
            return 2
        axes.append((name, vals.split(",")))
    d = _report_dir(a, "perturb", item.name)
    try:
        r = perturb(ctx, item, dll, axes, a.turns, a.jobs, d / "bisect" if a.bisect else None)
    except RN.ValidationError as ex:
        print("\n".join(ex.errors), file=sys.stderr)
        return 2
    md = [f"# perturbation: {item.name} ({dll.label})", "", "| axis | class | verdict | detail |", "|---|---|---|---|"]
    for row in r["axes"]:
        detail = row.get("note") or "; ".join(f"{e['value']}: " + ("same" if e.get("equal", True) and e["status"] == "ok" else
                                                 f"turn {e.get('first_divergent_turn')} {_compact_segments(e.get('divergent_segments') or [])}" if e["status"] == "ok" else e["status"])
                                               for e in row["values"][1:])
        if row.get("bisect"):
            detail += f" -> first segment {row['bisect']['first_segment']} ({row['bisect']['report']})"
        md.append(f"| {row['axis']} | {row['class'] or '?'} | {row['status']} | {detail} |")
    _write_json_md(d, "perturb", r, "\n".join(md) + "\n")
    print("\n".join(md[4:]) + f"\nreport {d}")
    return 6 if r["sensitive"] else (1 if r["failed"] else 0)


# ---------------------------------------------------------------- CLI
def main(argv: list[str]) -> int:
    p = argparse.ArgumentParser(prog="civsim.detlab")
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("digest", help="compare two digest.bin (file, run dir or sim key); exit 6 on divergence")
    s.add_argument("a")
    s.add_argument("b")
    s.add_argument("--json", action="store_true")
    s = sub.add_parser("check", help="same DLL, N runs, digests must be identical")
    s.add_argument("targets", nargs="+")
    s.add_argument("--repeat", type=int, default=2)
    s.add_argument("--dll", default="golden")
    _add_common(s)
    s = sub.add_parser("equiv", help="two DLLs over a corpus; writes equivalence.json")
    s.add_argument("targets", nargs="+")
    s.add_argument("--dll", default="golden")
    s.add_argument("--vs", required=True)
    _add_common(s)
    s = sub.add_parser("bisect", help="first divergent turn -> segment -> byte offset between two DLLs")
    s.add_argument("scenario")
    s.add_argument("--dll", default="golden")
    s.add_argument("--vs", required=True)
    _add_common(s)
    s = sub.add_parser("saveload", help="0->M vs 0->N + checkpoint + load -> M (needs host checkpoints)")
    s.add_argument("scenario")
    s.add_argument("--at", type=int, required=True)
    s.add_argument("--until", type=int, required=True)
    s.add_argument("--dll", default="golden")
    _add_common(s)
    s = sub.add_parser("perturb", help="vary one environment axis per run and compare digests")
    s.add_argument("scenario")
    s.add_argument("--axis", action="append", required=True, help="name=v1,v2,... (first value is the baseline); repeatable")
    s.add_argument("--bisect", action="store_true", help="for each sensitive axis, dump state at the first divergent turn and byte-diff the segments")
    s.add_argument("--dll", default="golden")
    _add_common(s)
    a = p.parse_args(argv)
    try:
        return {"digest": cmd_digest, "check": cmd_check, "equiv": cmd_equiv, "bisect": cmd_bisect, "saveload": cmd_saveload, "perturb": cmd_perturb}[a.cmd](a)
    except (FileNotFoundError, ValueError) as ex:
        print(f"error: {ex}", file=sys.stderr)
        return 2
