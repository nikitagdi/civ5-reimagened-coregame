"""Batch report (report.md + report.json) from batch.json."""
from __future__ import annotations

from collections import Counter
from pathlib import Path

from .. import common as C


def summarize(batch: dict) -> dict:
    rows = batch["scenarios"]
    ran = [r for r in rows if not r.get("cached") and r["status"] != "interrupted"]
    sim_ms = sum(r.get("sim_ms") or 0 for r in ran)
    wall_ms = sum(r.get("wall_ms") or 0 for r in ran)
    return {
        "name": batch["name"], "scenarios": len(rows), "by_status": dict(Counter(r["status"] for r in rows)),
        "cache_hits": sum(1 for r in rows if r.get("cached")), "cache_hit_rate": round(sum(1 for r in rows if r.get("cached")) / max(1, len(rows)), 3),
        "repeat_mismatches": [r["name"] for r in rows if r.get("repeat_mismatch")],
        "failures": [r["name"] for r in rows if r["status"] != "ok"],
        "wall_s": batch.get("wall_s"), "sim_s": round(sim_ms / 1000, 2),
        # overhead = 1 - sum(sim ms) / sum(process wall ms) over executed runs (process start, map generation, writes included)
        "overhead": round(1 - sim_ms / wall_ms, 3) if wall_ms else None,
        "interrupted": batch.get("interrupted", False),
    }


def render_md(batch: dict) -> str:
    s = summarize(batch)
    out = [f"# Batch {batch['name']}", "",
           f"dll `{batch['dll']['label']}` ({batch['dll']['sha256'][:12]}), host {batch['host_build_sha256'][:12]}, data {batch['data_hash'][:12]}, "
           f"jobs {batch['jobs']}, repeat {batch['repeat']}", ""]
    if s["failures"] or s["repeat_mismatches"]:
        out += ["## Failures", ""]
        for r in batch["scenarios"]:
            if r["status"] != "ok" or r.get("repeat_mismatch"):
                out.append(f"- **{r['name']}**: {r['status']}{' repeat_mismatch' if r.get('repeat_mismatch') else ''} at turn {r.get('last_turn')} "
                           f"({r.get('failure') or ''}) run dir `{r.get('run_dir')}`")
                for ln in (r.get("stdout_tail") or [])[-5:]:
                    out.append(f"    {ln}")
        out.append("")
    out += ["## Scenarios", "", "| scenario | status | cached | turns | sim s | wall s | ms/turn | final digest |", "|---|---|---|---|---|---|---|---|"]
    for r in batch["scenarios"]:
        td = r.get("turns_done") or 0
        mpt = f"{(r.get('sim_ms') or 0) / td:.1f}" if td and r.get("sim_ms") else "-"
        out.append(f"| {r['name']} | {r['status']} | {'yes' if r.get('cached') else 'no'} | {td}/{r.get('turns_requested')} | "
                   f"{(r.get('sim_ms') or 0) / 1000:.1f} | {(r.get('wall_ms') or 0) / 1000:.1f} | {mpt} | {r.get('final_digest') or '-'} |")
    out += ["", "## Totals", "",
            f"- scenarios {s['scenarios']}, by status {s['by_status']}", f"- cache hits {s['cache_hits']} ({s['cache_hit_rate'] * 100:.0f}%)",
            f"- batch wall {s['wall_s']} s, summed sim {s['sim_s']} s, tooling overhead {s['overhead'] if s['overhead'] is not None else 'n/a (all cached)'}", ""]
    return "\n".join(out)


def write(batch: dict, bdir: Path) -> None:
    C.write_json(bdir / "report.json", {"summary": summarize(batch), "scenarios": batch["scenarios"]})
    (bdir / "report.md").write_text(render_md(batch), encoding="utf-8")
