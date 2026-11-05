"""python -m civsim.perflab measure|compare|gate|sample|hotspots (see package docstring)."""
from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

from .. import common as C
from ..simrunner import runner as RN
from ..simrunner import scenario as S
from . import measure as M

SUITES = {"smoke": ("starter", 2), "standard": ("early", 3), "nightly": ("realistic", 5)}   # suite -> (corpus, K)


def _ctx(a) -> RN.Ctx:
    return RN.Ctx(host=a.host, data=a.data, game=a.game, timeout_s=a.timeout, log=lambda m: print(m, file=sys.stderr))


def _common(p: argparse.ArgumentParser) -> None:
    p.add_argument("--host")
    p.add_argument("--data")
    p.add_argument("--game")
    p.add_argument("--timeout", type=int)
    p.add_argument("--turns", type=int)
    p.add_argument("--window")
    p.add_argument("--skip-first", type=int, default=0, help="drop the first N turns of each run (after-load warmup)")
    p.add_argument("--jobs", type=int, default=1)
    p.add_argument("--pin", action="store_true", help="pin each worker's host to one logical CPU (2*slot)")
    p.add_argument("--name")


def _perf_dir(name: str) -> Path:
    return C.OUT / "perf" / name


def _targets(targets: list[str]) -> tuple[list[S.Item], str]:
    if len(targets) == 1 and targets[0] in SUITES:
        corpus = SUITES[targets[0]][0]
        return S.resolve_targets([corpus]), corpus
    return S.resolve_targets(targets), "+".join(Path(t).stem for t in targets)[:40]


def run_measure(a, targets: list[str], dll_a: str, dll_b: str, repeat: int, label: str) -> dict:
    items, cname = _targets(targets)
    da, db = C.resolve_dll(dll_a), C.resolve_dll(dll_b)
    name = a.name or f"{C.utc_stamp()}-{label}-{cname}-{da.label}-vs-{db.label}"
    return M.measure(_ctx(a), items, da, db, repeat=repeat, window=M.parse_window(a.window), skip_first=a.skip_first, jobs=a.jobs, pin=a.pin,
                     turns=a.turns, out_dir=_perf_dir(name))


def print_summary(r: dict) -> None:
    if r["geomean"]:
        print(f"{r['dll_a']['label']} / {r['dll_b']['label']}: geomean {r['geomean']:.3f}, 95% CI {M.fmt_ci(r)}, min {r['min']:.3f}, max {r['max']:.3f}, "
              f"{r['n_scenarios']} scenario(s), window {r['window'] or 'all'}")
    for key in ("failed", "digest_mismatch", "noisy"):
        if r[key]:
            print(f"  {key}: {', '.join(r[key])}")
    print(f"  results: {C.OUT / 'perf' / r['name']}")


def cmd_measure(a) -> int:
    dlls = a.dll.split(",")
    if len(dlls) != 2:
        print("--dll needs two entries: A,B", file=sys.stderr)
        return 2
    r = run_measure(a, a.targets, dlls[0], dlls[1], a.repeat, "measure")
    print_summary(r)
    if r["failed"]:
        return 1
    if a.target:
        ci = r["ci95"]
        worst = min((s["r"] for s in r["scenarios"] if s.get("r")), default=0)
        ok = bool(r["geomean"] and r["geomean"] >= a.target and ci and ci[0] >= a.target and worst >= a.target * 0.8)
        print(f"target x{a.target}: {'MET' if ok else 'NOT MET'} (geomean {r['geomean']:.3f}, CI lower {ci[0] if ci else float('nan'):.3f}, "
              f"worst scenario {worst:.3f} vs floor {a.target * 0.8:.2f})")
        return 0 if ok else 5
    return 0


def cmd_compare(a) -> int:
    ra, rb = C.read_json(Path(a.a) / "measure.json"), C.read_json(Path(a.b) / "measure.json")
    if ra["dll_a"]["sha256"] != rb["dll_a"]["sha256"]:
        print("warning: the two measurements use different baseline DLLs (A); the comparison is only indicative", file=sys.stderr)
    sa = {s["name"]: s["r"] for s in ra["scenarios"] if s.get("r")}
    sb = {s["name"]: s["r"] for s in rb["scenarios"] if s.get("r")}
    common = [n for n in sa if n in sb]
    print(f"{'scenario':36} {'r(1)':>8} {'r(2)':>8} {'r2/r1':>8}")
    rel = []
    for n in common:
        rel.append(sb[n] / sa[n])
        print(f"{n:36} {sa[n]:8.3f} {sb[n]:8.3f} {sb[n] / sa[n]:8.3f}")
    if rel:
        print(f"geomean r(1) {ra['geomean']:.3f} {M.fmt_ci(ra)}  r(2) {rb['geomean']:.3f} {M.fmt_ci(rb)}  "
              f"relative (2 vs 1) {math.exp(sum(map(math.log, rel)) / len(rel)):.3f} over {len(rel)} common scenario(s)")
    return 0


def evaluate_gate(m: dict, equiv_state: str, max_ratio: float = 1.03, mde: float = 0.03) -> tuple[int, str, list[str]]:
    """m: measure.json with A = candidate, B = reference (ratio = candidate time / reference time).
    equiv_state: confirmed | missing | diverged | skipped. Returns (exit code, verdict, reasons)."""
    if m["failed"] or not m["geomean"]:
        return 1, "runs-failed", [f"failed: {', '.join(m['failed']) or 'no scenario produced timings'}"]
    r, (lo, hi) = m["geomean"], m["ci95"]
    if m["digest_mismatch"]:
        return 5, "inconclusive", [f"candidate and reference digests differ for {', '.join(m['digest_mismatch'])}: the runs did different work"]
    if r > max_ratio and lo > 1.0:
        return 4, "regression", [f"time ratio {r:.3f} > {max_ratio} and CI lower bound {lo:.3f} > 1.00 (candidate slower than reference)"]
    if equiv_state in ("missing", "diverged"):
        return 5, "inconclusive", [f"equivalence {equiv_state}: run `civsim.detlab equiv` for this DLL pair first (or pass --no-equiv)"]
    if m.get("n_blocks", 99) < 4:
        return 5, "inconclusive", [f"only {m['n_blocks']} resampling block(s) (10 turns each): the CI is not meaningful, measure more turns/scenarios"]
    if m.get("ci_halfwidth_rel") and m["ci_halfwidth_rel"] > mde:
        return 5, "inconclusive", [f"CI half-width {m['ci_halfwidth_rel'] * 100:.1f}% exceeds the minimum detectable effect {mde * 100:.0f}% (more repeats/scenarios needed)"]
    return 0, "ok", [f"time ratio {r:.3f} (CI {lo:.3f}..{hi:.3f}), not a regression"]


def cmd_gate(a) -> int:
    from ..detlab import dyn
    cand, ref = C.resolve_dll(a.dll), C.resolve_dll(a.against)
    if a.reuse:
        m = C.read_json(Path(a.reuse) / "measure.json")
        mdir = Path(a.reuse)
    else:
        k = a.repeat or (SUITES[a.targets[0]][1] if len(a.targets) == 1 and a.targets[0] in SUITES else 3)
        m = run_measure(a, a.targets, a.dll, a.against, k, "gate")
        mdir = C.OUT / "perf" / m["name"]
    if a.no_equiv:
        eq_state, eq = "skipped", None
    else:
        eq = dyn.find_equivalence(ref.sha256, cand.sha256)
        eq_state = "missing" if eq is None else ("confirmed" if eq["all_equal"] else "diverged")
    code, verdict, reasons = evaluate_gate(m, eq_state, a.max_ratio, a.mde)
    out = {"schema": 1, "verdict": verdict, "exit_code": code, "reasons": reasons, "equivalence": eq_state, "equivalence_file": eq and eq["path"],
           "candidate": m["dll_a"], "reference": m["dll_b"], "ratio": m["geomean"], "ci95": m["ci95"], "max_ratio": a.max_ratio, "mde": a.mde}
    C.write_json(mdir / "gate.json", out)
    print(f"gate {verdict.upper()} (exit {code}): " + "; ".join(reasons))
    return code


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="civsim.perflab", description=__doc__.split("\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("measure")
    s.add_argument("targets", nargs="+")
    s.add_argument("--dll", required=True, help="A,B (ratio = A/B)")
    s.add_argument("--repeat", type=int, default=3)
    s.add_argument("--target", type=float, help="speed-up target, e.g. 5.0: geomean and CI lower bound >= target, no scenario below 80%")
    _common(s)
    s = sub.add_parser("compare")
    s.add_argument("a")
    s.add_argument("b")
    s = sub.add_parser("gate")
    s.add_argument("targets", nargs="+")
    s.add_argument("--dll", required=True, help="candidate")
    s.add_argument("--against", default="golden", help="reference DLL")
    s.add_argument("--repeat", type=int)
    s.add_argument("--max-ratio", type=float, default=1.03)
    s.add_argument("--mde", type=float, default=0.03)
    s.add_argument("--no-equiv", action="store_true")
    s.add_argument("--reuse", help="evaluate an existing measure dir instead of measuring")
    _common(s)
    from . import hotspots as H
    s = sub.add_parser("sample", help="external sampling profiler: run civhost under it (or --pid) and write hotspots.md/json")
    s.add_argument("target", nargs="?", help="scenario json / corpus (omit with --pid)")
    s.add_argument("--pick", help="index or name substring when the target expands to several scenarios")
    s.add_argument("--dll", default="profile", help="DLL variant/path (default: profile = golden + /Oy- frame pointers)")
    s.add_argument("--pid", type=int, help="attach to a running process instead of launching civhost")
    s.add_argument("--duration", type=float, help="seconds to sample (with --pid; default until exit)")
    s.add_argument("--hz", type=int, default=1000)
    s.add_argument("--stack-bytes", type=int, default=16384, help="max stack window copied per sample (bounded by the thread's stack base)")
    s.add_argument("--max-depth", type=int, default=64)
    s.add_argument("--fp-modules", default="auto", help="auto | none | comma list of module names whose code keeps EBP frames")
    s.add_argument("--verify", action="store_true", help="also run unsampled (cached) and require an identical digest.bin")
    s.add_argument("--host")
    s.add_argument("--data")
    s.add_argument("--game")
    s.add_argument("--timeout", type=int)
    s.add_argument("--turns", type=int)
    s.add_argument("--name")
    s.add_argument("--windows", type=int, default=3, help="turn windows for the per-window tables")
    s.add_argument("--update-re", default=H.DEFAULT_UPDATE_RE)
    s.add_argument("--digest-re", default=H.DEFAULT_DIGEST_RE)
    s = sub.add_parser("hotspots", help="re-run the offline analysis of a perf dir written by `sample`")
    s.add_argument("dir")
    s.add_argument("--windows", type=int, default=3)
    s.add_argument("--update-re", default=H.DEFAULT_UPDATE_RE)
    s.add_argument("--digest-re", default=H.DEFAULT_DIGEST_RE)
    a = p.parse_args(argv)
    if a.cmd == "sample" and not (a.target or a.pid):
        p.error("sample needs a target scenario or --pid")
    try:
        return {"measure": cmd_measure, "compare": cmd_compare, "gate": cmd_gate, "sample": H.cmd_sample, "hotspots": H.cmd_hotspots}[a.cmd](a)
    except RN.ValidationError as ex:
        print("\n".join(ex.errors), file=sys.stderr)
        return 2
    except (FileNotFoundError, ValueError) as ex:
        print(f"error: {ex}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
