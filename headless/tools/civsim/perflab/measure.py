"""measure: per-turn host times for DLL A and B on a corpus, ABBA-interleaved, with ratio / geomean / bootstrap CI."""
from __future__ import annotations

import concurrent.futures as cf
import os
import platform
import shutil
import sys
import threading
import time
from pathlib import Path

from .. import common as C
from ..simrunner import runner as RN
from ..simrunner import scenario as S
from . import stats as ST


def parse_window(s: str | None) -> tuple[int, int] | None:
    if not s:
        return None
    a, _, b = s.partition("-")
    return int(a), int(b)


def _turn_times(run_dir: Path) -> tuple[dict[int, float], float]:
    rj = C.read_json(run_dir / "run.json")
    ts = rj.get("turns", [])
    pre = sum(1 for t in ts if t.get("cpu_ms") and t["ms"] > t["cpu_ms"] * 1.02)
    return {t["turn"]: t["ms"] for t in ts}, (pre / len(ts) if ts else 0.0)


def _slim(d: Path) -> None:
    for f in d.iterdir():
        if f.name not in ("run.json", "result.json", "digest.bin", "scenario.json"):
            shutil.rmtree(f, ignore_errors=True) if f.is_dir() else f.unlink()


def abba_order(k: int) -> list[str]:
    """Run order for K repeats: rep 0 = A B, rep 1 = B A, ... (never all A then all B)."""
    order: list[str] = []
    for r in range(k):
        order += ["A", "B"] if r % 2 == 0 else ["B", "A"]
    return order


def measure(ctx: RN.Ctx, items: list[S.Item], dll_a: C.Dll, dll_b: C.Dll, *, repeat: int = 3, window: tuple[int, int] | None = None,
            skip_first: int = 0, jobs: int = 1, pin: bool = False, turns: int | None = None, out_dir: Path, bootstrap_iters: int = 2000) -> dict:
    out_dir.mkdir(parents=True, exist_ok=True)
    preps = {(it.name, w): RN.prepare(ctx, it, d, turns) for it in items for w, d in (("A", dll_a), ("B", dll_b))}
    counter = iter(range(1000))
    local = threading.local()
    lock = threading.Lock()

    def init() -> None:
        with lock:
            local.slot = next(counter)

    def one(it: S.Item) -> dict:
        data: dict[str, list] = {"A": [], "B": []}
        runs = {"A": [], "B": []}
        failures: list[str] = []
        mask = (1 << (2 * local.slot)) if pin and os.name == "nt" else None
        for n, which in enumerate(abba_order(repeat)):
            d = out_dir / "runs" / RN.safe_name(it.name) / f"{which}{n}"
            if d.exists():
                shutil.rmtree(d, ignore_errors=True)
            r = RN.execute(ctx, preps[(it.name, which)], d, affinity_mask=mask)
            if r["status"] != "ok":
                failures.append(f"{which}{n}: {r['status']} {r.get('failure') or ''}")
                continue
            tt, pre = _turn_times(d)
            runs[which].append({"times": tt, "preempted": pre, "digest": r["digest_sha256"], "sim_ms": r["sim_ms"]})
            _slim(d)
        res: dict = {"name": it.name, "failures": failures, "runs": {k: [x["sim_ms"] for x in v] for k, v in runs.items()}}
        if not failures:
            win = window
            res["med_a"] = ST.per_turn_medians([x["times"] for x in runs["A"]], win, skip_first)
            res["med_b"] = ST.per_turn_medians([x["times"] for x in runs["B"]], win, skip_first)
            res["digest_equal"] = len({x["digest"] for x in runs["A"] + runs["B"]}) == 1
            res["preempted_frac"] = {k: round(max((x["preempted"] for x in v), default=0.0), 3) for k, v in runs.items()}
        return res

    t0 = time.time()
    with cf.ThreadPoolExecutor(max_workers=max(1, jobs), initializer=init) as ex:
        rows = list(ex.map(one, items))
    good = [r for r in rows if not r["failures"] and r.get("med_a") and r.get("med_b")]
    summ = ST.summarize(good, iters=bootstrap_iters)
    ci = summ["ci95"]
    result = {
        "schema": 1, "name": out_dir.name, "created": time.strftime("%Y-%m-%dT%H:%M:%S"), "order": "ABBA", "repeat": repeat,
        "window": list(window) if window else None, "skip_first": skip_first, "pin": pin,
        "dll_a": {"label": dll_a.label, "sha256": dll_a.sha256}, "dll_b": {"label": dll_b.label, "sha256": dll_b.sha256},
        "host_build_sha256": ctx.host.build_sha256, "data_hash": ctx.data.hash, "jobs": jobs,
        "machine": {"cpu_count": os.cpu_count(), "platform": platform.platform(), "python": sys.version.split()[0]},
        "ratio_definition": "sum(median_K(ms_A)) / sum(median_K(ms_B)) over the window; >1 means B is faster",
        **summ, "ci_halfwidth_rel": round((ci[1] - ci[0]) / 2 / summ["geomean"], 4) if ci and summ["geomean"] else None,
        "failed": [r["name"] for r in rows if r["failures"]], "digest_mismatch": [r["name"] for r in good if not r["digest_equal"]],
        "noisy": [r["name"] for r in good if max(r["preempted_frac"].values()) > 0.05], "wall_s": round(time.time() - t0, 1),
        "scenarios": [{"name": r["name"], "failures": r["failures"], "r": r.get("r"), "n_turns": len(r.get("med_a", {})),
                       "digest_equal": r.get("digest_equal"), "sim_ms_runs": r["runs"],
                       "med_a": {str(k): round(v, 3) for k, v in r.get("med_a", {}).items()},
                       "med_b": {str(k): round(v, 3) for k, v in r.get("med_b", {}).items()}} for r in rows],
    }
    C.write_json(out_dir / "measure.json", result)
    (out_dir / "measure.md").write_text(render_md(result), encoding="utf-8")
    return result


def fmt_ci(r: dict) -> str:
    ci = r.get("ci95")
    return f"[{ci[0]:.3f}, {ci[1]:.3f}]" if ci else "n/a"


def render_md(r: dict) -> str:
    md = [f"# perf: {r['dll_a']['label']} / {r['dll_b']['label']}", "",
          f"ratio = {r['ratio_definition']}", f"repeat {r['repeat']} ({r['order']}), window {r['window'] or 'all turns'}, jobs {r['jobs']}, wall {r['wall_s']} s", ""]
    if r["geomean"]:
        md += [f"**geometric mean {r['geomean']:.3f}**, 95% CI {fmt_ci(r)}, min {r['min']:.3f}, max {r['max']:.3f}, scenarios {r['n_scenarios']}", ""]
    for key, label in (("failed", "run failures"), ("digest_mismatch", "digest differs between A and B (different work, ratio not valid)"), ("noisy", "more than 5% preempted turns")):
        if r[key]:
            md.append(f"- {label}: {', '.join(r[key])}")
    md += ["", "| scenario | turns | ratio | digests equal |", "|---|---|---|---|"]
    md += [f"| {s['name']} | {s['n_turns']} | {s['r']:.3f} | {s['digest_equal']} |" if s.get("r") else f"| {s['name']} | - | failed | - |" for s in r["scenarios"]]
    return "\n".join(md) + "\n"
