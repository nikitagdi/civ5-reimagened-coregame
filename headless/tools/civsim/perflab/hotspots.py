"""hotspots: `sample` (run civhost under the external sampler, or attach to a PID) and offline analysis of samples.json.gz.

Scopes: all samples, and the phases they fall into (update = inside the DLL's per-turn Update(), digest = the host's per-turn state
hashing, other = launch/map generation/shutdown). `update` is what perflab measures (run.json `ms` is the Update() time only), so the
Amdahl line and the hotspot tables are primarily for that scope. hotspots.json/.md in headless/out/perf/<name>/.
"""
from __future__ import annotations

import bisect
import dataclasses
import gzip
import json
import re
import shutil
import sys
import time
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any, Callable

from .. import common as C
from ..simrunner import runner as RN
from ..simrunner import scenario as S

DEFAULT_UPDATE_RE = r"^CvDllGame::Update$|^CvGame::update$"
DEFAULT_DIGEST_RE = r"(?i)digest|xxh64|HstDump"

# subsystem partition: first regex (from the leaf outward) that matches a function name in the stack decides the sample's subsystem
SUBSYSTEMS: list[tuple[str, str]] = [
    ("pathfinding", r"CvAStar|PathFinder|CvTwoLayer|CvPathNode|CvStepFinder|CvIgnoreUnitsPathFinder|CvRouteFinder|CvInfluenceMapAStar|CvPlotsOnPathFinder"),
    ("danger plots", r"CvDangerPlots"),
    ("tactical map / tactical AI", r"CvTacticalAnalysisMap|CvTacticalAI|CvTacticalMap|CvTacticalTarget|TacticalAIHelpers"),
    ("homeland AI", r"CvHomelandAI"),
    ("military AI / operations", r"CvMilitaryAI|CvAIOperation|CvAIOperationMilitary|CvArmyAI|CvAIMilitaryOperation|CvAIFormationAI|CvFormation"),
    ("diplomacy AI / deals", r"CvDiplomacyAI|CvDealAI|CvDeal\b|CvGameDeals|CvDeal::"),
    ("economic / city / grand strategy AI", r"CvEconomicAI|CvCityStrategyAI|CvCitySpecializationAI|CvGrandStrategyAI|CvEmphasisAI|CvTechAI|CvPolicyAI|CvReligionAI|CvCityConnections|CvBuilderTaskingAI|CvWonderProductionAI|CvBuildingProductionAI|CvUnitProductionAI|CvProjectProductionAI|CvProcessProductionAI|CvLeagueAI|CvVotingSystem|CvFlavorManager"),
    ("lua bindings / script events", r"CvLua|LuaSupport|CallHook|CallTest|CallAccumulator|CvDllScriptSystem|ScriptSystemUtility"),
    ("trade routes", r"CvGameTrade|CvPlayerTrade|CvTradeRoute"),
    ("plot / map / vision", r"CvPlot::|CvMap::|CvArea::|CvTeam::.*[Rr]eveal|changeVisibility|updateSight|updateVisibility|CvRandom|CvCityManager"),
    ("city (growth, yields, production)", r"CvCity::|CvCityBuildings|CvCityCitizens|CvCityReligions|CvCityEspionage|CvCityCulture|CvCityEmphases"),
    ("unit (moves, combat, AI update)", r"CvUnit::|CvUnitCombat|CvUnitMission|CvUnitEntry|CvUnitAI"),
    ("player / team / game turn bookkeeping (generic)", r"CvPlayer::|CvPlayerAI::|CvPlayerPolicies|CvPlayerTechs|CvPlayerReligions|CvPlayerCulture|CvTeam::|CvTeamTechs|CvGameReligions|CvGameCulture|CvGame::|CvMinorCivAI|CvLeague|CvEspionage|CvNotifications|CvDiplomacy|CvPlayerEspionage"),
]


# functions of the ranked ideas and of the known hot paths: reported with inclusive and self share
WATCH_FUNCS = [
    "CvGame::isOption", "CvPreGame::GetGameOption", "CvGlobals::getInfoTypeForString",
    "CvAStar::GeneratePath", "CvAStar::CreateChildren", "CvAStar::LinkChild", "CvAStar::UpdateParents", "PathValid", "PathCost", "TurnsToReachTarget",
    "CvTacticalAnalysisMap::MarkCellsNearEnemy", "CvTacticalAnalysisMap::PopulateCell", "CvTacticalAI::ProcessDominanceZones", "CvTacticalAI::AssignBarbarianMoves",
    "CvCityCitizens::GetPlotValue", "CvCityCitizens::DoReallocateCitizens", "CvCityCitizens::DoRemoveWorstCitizen", "CvCityCitizens::DoTurn", "CvCity::doTurn",
    "CvDangerPlots::UpdateDanger", "CvPlayer::updatePlotFoundValues", "CvPlayerAI::AI_updateFoundValues", "CvCitySiteEvaluator::PlotFoundValue",
    "CvMinorCivAI::CalculateBullyMetric", "CvPlayerPolicies::GetNumericModifier", "CvTradeAI::PrioritizeTradeRoutes", "CvDiplomacyAI::DoTurn",
    "CvUnit::canMoveInto", "CvUnitMovement::IsSlowedByZOC", "CvPlot::updateSight", "CvPlot::calculateYield", "CvPlot::changeVisibilityCount",
    "CvGame::updateMoves", "CvPlayer::doTurn", "CvPlayerAI::AI_unitUpdate", "CvHomelandAI::AssignHomelandMoves", "CvEconomicAI::DoTurn", "CvPlayerAI::AI_doTurnPre",
    "CvCityStrategyAI::DoTurn", "CvGameReligions::DoPlayerTurn", "CvTeam::doTurn", "CvGame::doTurn", "CvPlayer::DoUnitReset", "CvPlayer::doTurnUnits",
    "LuaSupport::CallHook", "LuaSupport::CallTestAll", "LuaSupport::CallTestAny", "LuaSupport::CallAccumulator",
]


# ---------------------------------------------------------------- categories
def category(mod: str, path: str = "") -> str:
    n = mod.lower()
    if n.startswith("cvgamecore"):
        return "gamecore DLL"
    if n == "civhost.exe":
        return "host (civhost.exe)"
    if n.startswith("lua51"):
        return "lua51"
    if n.startswith(("cvgamedatabase", "cvlocalization")):
        return "database DLL"
    if n.startswith(("msvcr", "msvcp", "msvcrt", "ucrtbase", "vcruntime")):
        return "CRT"
    if path.lower().startswith("c:\\windows") or n in ("ntdll.dll", "kernel32.dll", "kernelbase.dll", "wow64cpu.dll"):
        return "OS"
    return "other" if mod != "?" else "unknown"


def load_samples(path: Path) -> dict:
    with gzip.open(path, "rt", encoding="utf-8") as f:
        return json.load(f)


# ---------------------------------------------------------------- analysis
def _pct(n: int, tot: int) -> float:
    return round(100.0 * n / tot, 2) if tot else 0.0


def analyze(doc: dict, run_json: dict | None, *, update_re: str = DEFAULT_UPDATE_RE, digest_re: str = DEFAULT_DIGEST_RE, windows: int = 3,
            top_self: int = 50, top_incl: int = 30) -> dict:
    from . import sampler as SM
    mods = doc["modules"]
    sym = SM.Symbolizer([dict(m) for m in mods])
    stacks: list[list[int]] = doc["stacks"]
    seq: list[int] = doc["seq"]
    path_of = {m["name"]: m["path"] for m in mods}
    cat_of: dict[str, str] = {}

    def cat(mod: str) -> str:
        if mod not in cat_of:
            cat_of[mod] = category(mod, path_of.get(mod, ""))
        return cat_of[mod]

    ure, dre = re.compile(update_re), re.compile(digest_re)
    sub_res = [(n, re.compile(p)) for n, p in SUBSYSTEMS]
    # resolve every unique stack once
    F: list[list[tuple[str, str]]] = []
    phase: list[str] = []
    subsys: list[str] = []
    for fr in stacks:
        fl = []
        for i, a in enumerate(fr):
            m, nme, _ = sym.resolve(a, is_return=i > 0)
            fl.append((m, nme))
        F.append(fl)
        names = [n for _, n in fl]
        if any(dre.search(n) for n in names):
            phase.append("digest")
        elif any(ure.search(n) for n in names):
            phase.append("update")
        else:
            phase.append("other")
        # subsystem: innermost gamecore/host frame matching a specific subsystem; the generic player/game bucket only if none does.
        # (CRT/sqlite/OS/lua leaf frames are skipped: their time belongs to the DLL code that caused it.)
        s = "unclassified"
        ghc = [(category(m, path_of.get(m, "")), n) for m, n in fl]
        ghc = [(c, n) for c, n in ghc if c in ("gamecore DLL", "host (civhost.exe)")]
        if ghc and ghc[0][0] == "host (civhost.exe)":
            hn = ghc[0][1]                                                       # nearest gamecore/host frame is a host function
            s = ("host: det heap hooks (HeapAlloc/Free lock + zero fill)" if "HstDet" in hn else
                 "host: script / Lua glue (event dispatch)" if re.search(r"HstScript|HstExeLua|HstLua|DispatchGameEvent|HstArena|HstExe", hn) else
                 "host: engine stubs / other")
        else:
            gh = [n for c, n in ghc if c == "gamecore DLL"]
            for n in gh:
                hit = next((sn for sn, rx in sub_res[:-1] if rx.search(n)), None)
                if hit:
                    s = hit
                    break
            else:
                if any(sub_res[-1][1].search(n) for n in gh):
                    s = sub_res[-1][0]
        subsys.append(s)
    total = len(seq)
    per_stack = Counter(seq)

    def scope_counts(select: Callable[[int], bool]) -> dict[int, int]:
        return {sid: c for sid, c in per_stack.items() if select(sid)}

    GC, HOST = "gamecore DLL", "host (civhost.exe)"

    def aggregate(counts: dict[int, int], with_extra: bool = True) -> dict:
        """Frames of non-PDB modules (CRT/lua/sqlite/OS) come from a heuristic stack scan and may contain stale slots, so inclusive numbers
        count only gamecore and host frames (EBP-chain frames), and every sample is attributed to its *driver*: the frame nearest the leaf
        that belongs to the gamecore DLL (DLL-driven, including its CRT/sqlite/OS/lua callees) or to civhost.exe (host-driven)."""
        tot = sum(counts.values())
        self_c: Counter = Counter()
        incl_c: Counter = Counter()
        mod_c: Counter = Counter()
        cat_s: Counter = Counter()
        driver_c: Counter = Counter()                      # (leaf category, driver category)
        leaf: Counter = Counter()
        sub_c: Counter = Counter()
        callers: dict[str, Counter] = defaultdict(Counter)  # leaf category -> (driver function) samples
        callers_fn: dict[tuple, Counter] = defaultdict(Counter)
        for sid, c in counts.items():
            fl = F[sid]
            k0 = fl[0]
            c0 = cat(k0[0])
            self_c[k0] += c
            mod_c[k0[0]] += c
            cat_s[c0] += c
            leaf[stacks[sid][0]] += c
            sub_c[subsys[sid]] += c
            seen = set()
            drv = None
            for k in fl:
                ck = cat(k[0])
                if ck == GC or ck == HOST:
                    if drv is None:
                        drv = k
                    if k not in seen:
                        seen.add(k)
                        incl_c[k] += c
            dcat = cat(drv[0]) if drv else "none"
            driver_c[c0, dcat] += c
            dname = drv[1] if drv else "(no gamecore/host frame)"
            if c0 != GC:
                callers[c0][dname if dcat != HOST else f"[host] {dname}"] += c
                callers_fn[k0][dname if dcat != HOST else f"[host] {dname}"] += c
        out: dict[str, Any] = {"samples": tot}
        cats = sorted(cat_s, key=lambda x: -cat_s[x])
        out["categories"] = {k: {"self": cat_s[k], "self_pct": _pct(cat_s[k], tot),
                                 "dll_driven_pct": _pct(driver_c.get((k, GC), 0), tot), "host_driven_pct": _pct(driver_c.get((k, HOST), 0), tot),
                                 "unattributed_pct": _pct(driver_c.get((k, "none"), 0), tot)} for k in cats}
        out["modules"] = [{"module": m, "self": c, "self_pct": _pct(c, tot)} for m, c in mod_c.most_common()]
        out["top_self"] = [{"rank": i + 1, "function": k[1], "module": k[0], "self": c, "self_pct": _pct(c, tot),
                            "incl_pct": _pct(incl_c[k], tot) if cat(k[0]) in (GC, HOST) else None}
                           for i, (k, c) in enumerate(self_c.most_common(top_self))]
        out["top_incl"] = [{"rank": i + 1, "function": k[1], "module": k[0], "incl": c, "incl_pct": _pct(c, tot), "self_pct": _pct(self_c.get(k, 0), tot)}
                           for i, (k, c) in enumerate(incl_c.most_common(top_incl))]
        out["subsystems"] = [{"subsystem": k, "samples": c, "pct": _pct(c, tot)} for k, c in sub_c.most_common()]
        if with_extra:
            watch = []
            for wf in WATCH_FUNCS:
                inc = sum(c for k, c in incl_c.items() if k[1] == wf)
                slf = sum(c for k, c in self_c.items() if k[1] == wf)
                if inc or slf:
                    watch.append({"function": wf, "incl_pct": _pct(inc, tot), "self_pct": _pct(slf, tot)})
            out["watch"] = sorted(watch, key=lambda w: -w["incl_pct"])
            lines = []
            for addr, c in leaf.most_common(40):
                m, nme, _ = sym.resolve(addr)
                lines.append({"addr": f"0x{addr:08x}", "module": m, "function": nme, "line": sym.line(addr) if cat(m) in (GC, HOST) else None,
                              "samples": c, "pct": _pct(c, tot)})
            out["top_lines"] = lines
            out["callers_by_category"] = {k: {"self_pct": _pct(cat_s[k], tot), "drivers": [{"function": f, "pct_of_scope": _pct(n, tot)} for f, n in cc.most_common(8)]}
                                          for k, cc in sorted(callers.items(), key=lambda kv: -cat_s[kv[0]])}
            out["callers"] = [{"function": k[1], "module": k[0], "self_pct": _pct(self_c[k], tot),
                               "callers": [{"driver": a, "pct_of_function": _pct(n, self_c[k])} for a, n in cc.most_common(5)]}
                              for k, cc in sorted(callers_fn.items(), key=lambda kv: -self_c[kv[0]])[:15]]
        dll_driven = sum(v for (lc, dc), v in driver_c.items() if dc == GC)
        host_driven = sum(v for (lc, dc), v in driver_c.items() if dc == HOST)
        gc_self = cat_s.get(GC, 0)
        gc_unres = sum(c for k, c in self_c.items() if cat(k[0]) == GC and re.search(r"\+0x[0-9a-f]+$", k[1]))
        out["gamecore_resolved_pct"] = _pct(gc_self - gc_unres, gc_self)

        def ceil(p: float) -> float | None:
            return round(1.0 / (1.0 - p), 2) if 0 < p < 1 else None
        p1, p2, p3 = (gc_self / tot if tot else 0), (dll_driven / tot if tot else 0), (host_driven / tot if tot else 0)
        out["amdahl"] = {"p_dll_self": round(p1, 4), "max_speedup_dll_self": ceil(p1),
                         "p_dll_driven": round(p2, 4), "max_speedup_dll_driven": ceil(p2),
                         "p_host_driven": round(p3, 4), "p_unattributed": round(1 - p2 - p3, 4) if tot else 0}
        return out

    scopes = {"all": aggregate(per_stack)}
    shares = {}
    for ph in ("update", "digest", "other"):
        cn = scope_counts(lambda sid, ph=ph: phase[sid] == ph)
        shares[ph] = sum(cn.values())
        if cn:
            scopes[ph] = aggregate(cn)
    t_us = doc.get("t_us") or []
    period_ms = (t_us[-1] - t_us[0]) / 1000 / max(1, len(t_us) - 1) if len(t_us) > 1 else 1.0
    turn_ms = [t["ms"] for t in (run_json or {}).get("turns", [])]
    out: dict[str, Any] = {
        "schema": 1, "stats": {k: v for k, v in doc["stats"].items() if k != "modules"},
        "phases": {ph: {"samples": n, "pct": _pct(n, total), "est_s": round(n * period_ms / 1000, 2)} for ph, n in shares.items()},
        "update_re": update_re, "digest_re": digest_re, "sample_period_ms": round(period_ms, 3),
        "modules": [], "scopes": scopes, "turn_ms": [round(x, 2) for x in turn_ms],
        "turn_digest_ms": [round(t.get("digest_ms", 0), 2) for t in (run_json or {}).get("turns", [])],
    }
    for m in mods:
        out["modules"].append({"name": m["name"], "path": m["path"], "base": f"0x{m['base']:08x}", "size": m["size"], "category": cat(m["name"]),
                               "fp_frames": m.get("fp"), "symbols": {k[1]: v for k, v in sym.pdb_hits.items() if k[0] == m["name"]}})
    # turn windows: the j-th update sample of N maps to time (j+.5)/N * sum(ms) of the update phase -> turn by cumulative ms
    upd = [i for i, sid in enumerate(seq) if phase[sid] == "update"]
    if upd and turn_ms and windows > 0:
        cum, s = [], 0.0
        for x in turn_ms:
            s += x
            cum.append(s)
        nt = len(turn_ms)
        edges = [round(nt * k / windows) for k in range(windows + 1)]
        win_counts: list[Counter] = [Counter() for _ in range(windows)]
        for j, i in enumerate(upd):
            tm = (j + 0.5) / len(upd) * cum[-1]
            turn = bisect.bisect_left(cum, tm)               # 0-based turn index
            w = min(windows - 1, max(0, bisect.bisect_right(edges, turn) - 1))
            win_counts[w][seq[i]] += 1
        out["windows"] = []
        for w in range(windows):
            a, b = edges[w] + 1, edges[w + 1]
            agg = aggregate(dict(win_counts[w]), with_extra=False)
            out["windows"].append({"turns": [a, b], "ms_per_turn": round(sum(turn_ms[a - 1:b]) / max(1, b - a + 1), 1), "scope": agg})
    sym.close()
    return out


# ---------------------------------------------------------------- rendering
def _tbl(rows: list[list], head: list[str], align: str | None = None) -> list[str]:
    align = align or ("l" * len(head))
    sep = "|".join(":---" if a == "l" else "---:" for a in align)
    out = ["| " + " | ".join(head) + " |", "|" + sep + "|"]
    for r in rows:
        out.append("| " + " | ".join(str(x).replace("|", "\\|") for x in r) + " |")
    return out


def render_md(res: dict, meta: dict) -> str:
    md = [f"# Hotspots: {meta.get('title', '')}", ""]
    st = res["stats"]
    for k in ("scenario", "dll", "turns", "host", "note"):
        if meta.get(k):
            md.append(f"- {k}: {meta[k]}")
    md.append(f"- samples: {st['samples']} at {st['achieved_hz']} Hz (target {st['hz_target']}), wall {st['wall_s']} s, failed {st['failed']}, "
              f"mean suspend {st['mean_suspend_us']} us ({st['suspended_frac'] * 100:.1f}% of wall suspended), unique stacks {st['unique_stacks']}, "
              f"mean depth {st['mean_depth']}, scan steps/sample {st['scan_steps_per_sample']}, depth-capped {st['depth_capped']}")
    if meta.get("verify"):
        v = meta["verify"]
        md.append(f"- digest check: {v['text']}")
    md.append("")
    md += ["## Phases", ""]
    md += _tbl([[p, d["samples"], f"{d['pct']}%", d["est_s"]] for p, d in res["phases"].items()], ["phase", "samples", "share", "~seconds"], "lrrr")
    md += ["", "update = inside the DLL's per-turn Update() (what `perflab` measures: run.json `ms`); digest = host-side per-turn state hashing "
           "(ICvGame1::Write & co + xxh64); other = launch, map generation, shutdown.", ""]
    tm = res.get("turn_ms") or []
    if tm:
        n = len(tm)
        pts = sorted({1, min(n, 10), min(n, 25), min(n, 50), min(n, 75), n})
        dg = res.get("turn_digest_ms") or []
        md += ["## Turn time growth (run.json)", ""]
        md += _tbl([[t, tm[t - 1], dg[t - 1] if dg else ""] for t in pts], ["turn", "update ms", "digest ms"], "lrr")
        w = max(1, n // 10)
        md += ["", f"mean of first {w} turns {sum(tm[:w]) / w:.1f} ms, last {w} turns {sum(tm[-w:]) / w:.1f} ms (x{(sum(tm[-w:]) / w) / (sum(tm[:w]) / w):.1f}); total {sum(tm) / 1000:.1f} s", ""]
    primary = "update" if "update" in res["scopes"] else "all"
    for scope_name in dict.fromkeys([primary, "all"]):
        sc = res["scopes"][scope_name]
        md += [f"## Scope `{scope_name}` ({sc['samples']} samples)", "", f"Samples inside the gamecore DLL resolved to a function name: {sc['gamecore_resolved_pct']}%.", ""]
        a = sc["amdahl"]
        md.append(f"**Amdahl** (scope {scope_name}): p_DLL = {a['p_dll_self'] * 100:.1f}% of samples execute inside the gamecore DLL itself -> "
                  f"max speedup if only the DLL's own code got faster = 1/(1-p_DLL) = **x{a['max_speedup_dll_self']}**. "
                  f"Counting the work the DLL causes in the CRT/sqlite/OS/lua (DLL-driven: nearest gamecore-or-host frame is a gamecore frame) as DLL-controllable "
                  f"(e.g. by caching or calling less): p = {a['p_dll_driven'] * 100:.1f}% -> x{a['max_speedup_dll_driven']}; "
                  f"host-driven (engine stubs, loopback, det heap, Lua glue) {a['p_host_driven'] * 100:.1f}%, unattributed {a['p_unattributed'] * 100:.1f}%. "
                  f"A x5 target needs p >= 80%.")
        md += ["", "### Share by module category (self) and who drives it", ""]
        md += _tbl([[k, v["self"], f"{v['self_pct']}%", f"{v['dll_driven_pct']}%", f"{v['host_driven_pct']}%", f"{v['unattributed_pct']}%"]
                    for k, v in sc["categories"].items()], ["category (leaf)", "self samples", "self", "DLL-driven", "host-driven", "unattributed"], "lrrrrr")
        md += ["", "### Share by module (self)", ""]
        md += _tbl([[m["module"], m["self"], f"{m['self_pct']}%"] for m in sc["modules"][:12]], ["module", "samples", "self"], "lrr")
        md += ["", "### Subsystems (innermost gamecore/host frame of a subsystem class, partition; CRT/sqlite/OS time is charged to its caller)", ""]
        md += _tbl([[s["subsystem"], s["samples"], f"{s['pct']}%"] for s in sc["subsystems"]], ["subsystem", "samples", "share"], "lrr")
        lim_s = 50 if scope_name == primary else 15
        lim_i = 30 if scope_name == primary else 10
        md += ["", f"### Top {lim_s} functions by self samples", ""]
        md += _tbl([[r["rank"], f"{r['self_pct']}%", "-" if r["incl_pct"] is None else f"{r['incl_pct']}%", r["function"], r["module"]] for r in sc["top_self"][:lim_s]],
                   ["#", "self", "incl", "function", "module"], "rrrll")
        md += ["", f"### Top {lim_i} functions by inclusive samples (gamecore and host frames only)", ""]
        md += _tbl([[r["rank"], f"{r['incl_pct']}%", f"{r['self_pct']}%", r["function"], r["module"]] for r in sc["top_incl"][:lim_i]],
                   ["#", "incl", "self", "function", "module"], "rrrll")
        if scope_name == primary:
            md += ["", "### Watch list (ranked ideas and known hot paths)", ""]
            md += _tbl([[w["incl_pct"].__str__() + "%", w["self_pct"].__str__() + "%", w["function"]] for w in sc["watch"]], ["incl", "self", "function"], "rrl")
            md += ["", "### Hottest instruction lines (self)", ""]
            md += _tbl([[l["pct"].__str__() + "%", l["line"] or "", l["function"], l["module"], l["addr"]] for l in sc["top_lines"][:25]],
                       ["self", "source line", "function", "module", "address"], "rllll")
            md += ["", "### Who drives the time spent outside the gamecore DLL (nearest gamecore/host function above the leaf, % of the scope)", ""]
            for cname, cv in sc["callers_by_category"].items():
                md.append(f"- **{cname}** ({cv['self_pct']}% self): " + ", ".join(f"`{d['function']}` {d['pct_of_scope']}%" for d in cv["drivers"][:6]))
            md += ["", "### Callers of the hottest non-gamecore functions", ""]
            for c in sc["callers"][:10]:
                md.append(f"- `{c['function']}` ({c['module']}, {c['self_pct']}% self): " +
                          ", ".join(f"`{x['driver']}` {x['pct_of_function']}%" for x in c["callers"][:4]))
        md.append("")
    if res.get("windows"):
        md += ["## Turn windows (update scope)", ""]
        for w in res["windows"]:
            sc = w["scope"]
            md += [f"### turns {w['turns'][0]}-{w['turns'][1]} (mean {w['ms_per_turn']} ms/turn, {sc['samples']} samples)", ""]
            md += _tbl([[s["subsystem"], f"{s['pct']}%"] for s in sc["subsystems"][:8]], ["subsystem", "share"], "lr")
            md += [""]
            md += _tbl([[r["rank"], f"{r['self_pct']}%", r["function"], r["module"]] for r in sc["top_self"][:12]], ["#", "self", "function", "module"], "rrll")
            md.append("")
    md += ["## Modules and symbols", ""]
    md += _tbl([[m["name"], m["category"], m["base"], m["size"], "yes" if m["fp_frames"] else "", ", ".join(f"{k}:{v}" for k, v in m["symbols"].items())]
                for m in res["modules"] if m["category"] in ("gamecore DLL", "host (civhost.exe)", "lua51", "database DLL", "CRT")],
               ["module", "category", "base", "size", "EBP frames", "symbol lookups"], "lllrll")
    md += ["", "Names: `~name` = nearest preceding export of a module without PDB (approximate); `module+0xNNN00` = 1 KB bucket when no export is near.", "",
           "Method: external sampler (Wow64SuspendThread + Wow64GetThreadContext + ReadProcessMemory of the stack window, ResumeThread); frames from an EBP chain where "
           "the module keeps frame pointers (profile DLL /Oy-, civhost.exe) and a validated return-address scan otherwise (CRT, lua, sqlite, OS). "
           "Function names via dbghelp (PDB for the DLL and civhost.exe, exports for the rest).", ""]
    return "\n".join(md)


# ---------------------------------------------------------------- commands
def _perf_dir(name: str) -> Path:
    return C.OUT / "perf" / name


def write_outputs(pdir: Path, res: dict, meta: dict) -> None:
    res = {**res, "meta": meta}
    C.write_json(pdir / "hotspots.json", res)
    (pdir / "hotspots.md").write_text(render_md(res, meta), encoding="utf-8")


def cmd_hotspots(a) -> int:
    pdir = Path(a.dir)
    doc = load_samples(pdir / "samples.json.gz")
    rj = None
    run = pdir / "run" / "run.json"
    if run.is_file():
        rj = C.read_json(run)
    old = C.read_json(pdir / "hotspots.json").get("meta", {}) if (pdir / "hotspots.json").is_file() else {}
    res = analyze(doc, rj, update_re=a.update_re, digest_re=a.digest_re, windows=a.windows)
    write_outputs(pdir, res, old)
    print(f"{pdir / 'hotspots.md'}")
    return 0


def pin_binaries(ctx: RN.Ctx, dll: C.Dll) -> tuple[list[str], C.Dll]:
    """Private copies of the host exe and the DLL with their PDBs, content-addressed under headless/out/perf/_bin/: the host and DLL get
    rebuilt while profiles run (and symbols must match the binary that was sampled). Returns (host argv prefix, DLL copy)."""
    root = C.OUT / "perf" / "_bin"
    cmd = list(ctx.host.cmd)
    if ctx.host.path.suffix.lower() == ".exe":
        d = root / ("host-" + C.file_sha256(ctx.host.path)[:16])      # current content, not the sha ctx was created with
        if not (d / ctx.host.path.name).is_file():
            d.mkdir(parents=True, exist_ok=True)
            pdb = ctx.host.path.with_suffix(".pdb")
            if pdb.is_file():
                shutil.copy2(pdb, d / pdb.name)
            shutil.copy2(ctx.host.path, d / ctx.host.path.name)
        cmd = [str(d / ctx.host.path.name)]
    dsha = C.file_sha256(dll.path)
    d = root / ("dll-" + dsha[:16])
    if not (d / dll.path.name).is_file():
        d.mkdir(parents=True, exist_ok=True)
        pdb = dll.path.with_suffix(".pdb")
        if pdb.is_file():
            shutil.copy2(pdb, d / pdb.name)
        shutil.copy2(dll.path, d / dll.path.name)
    return cmd, C.Dll(label=dll.label, path=d / dll.path.name, sha256=dsha)


def _pick(items: list[S.Item], pick: str | None) -> S.Item:
    if len(items) == 1 and not pick:
        return items[0]
    if not pick:
        raise ValueError("the target expands to %d scenarios; choose one with --pick <index|name substring>:\n  " % len(items) +
                         "\n  ".join(f"{i}: {it.name}" for i, it in enumerate(items)))
    if pick.isdigit() and int(pick) < len(items):
        return items[int(pick)]
    hit = [it for it in items if pick in it.name]
    if not hit:
        raise ValueError(f"--pick {pick}: no scenario name contains it")
    return hit[0]


def cmd_sample(a) -> int:
    from . import sampler as SM
    log = lambda m: print(m, file=sys.stderr)
    fp = a.fp_modules
    if a.pid:
        name = a.name or f"sample-pid{a.pid}-{time.strftime('%H%M%S')}"
        pdir = _perf_dir(name)
        pdir.mkdir(parents=True, exist_ok=True)
        s = SM.attach_sample(a.pid, a.hz, a.stack_bytes, a.max_depth, fp, a.duration, log)
        meta = {"title": name, "note": f"attached to pid {a.pid}"}
        rj = None
    else:
        ctx0 = RN.Ctx(host=a.host, data=a.data, game=a.game, timeout_s=a.timeout, log=log)
        dll0 = C.resolve_dll(a.dll)
        # run (reference and sampled) from private copies of host/DLL + PDBs: the other agents rebuild them while this runs
        host_cmd, dll = pin_binaries(ctx0, dll0)
        ctx = RN.Ctx(host=host_cmd[0], data=a.data, game=a.game, timeout_s=a.timeout, log=log) if ctx0.host.path.suffix.lower() == ".exe" else ctx0
        item = _pick(S.resolve_targets([a.target]), a.pick)
        prep = RN.prepare(ctx, item, dll, a.turns)
        name = a.name or f"sample-{RN.safe_name(item.name)}-{dll0.label}-{prep.turns}t"
        pdir = _perf_dir(name)
        shutil.rmtree(pdir, ignore_errors=True)
        pdir.mkdir(parents=True, exist_ok=True)
        ref = None
        if a.verify:
            log(f"reference run (unsampled), {prep.turns} turns ...")
            ref = RN.run_scenario(ctx, item, dll, turns=a.turns, use_cache=True, prep=prep)
            log(f"  reference: {ref['status']}{' (cached)' if ref.get('cached') else ''} sim {ref['sim_ms'] / 1000:.1f}s wall {ref['wall_ms'] / 1000:.1f}s digest {ref.get('final_digest')}")
        run_dir = pdir / "run"
        run_dir.mkdir(parents=True, exist_ok=True)
        scn = run_dir / "scenario.json"
        C.write_json(scn, {**prep.canon, "turns": prep.turns, "name": prep.name})
        argv, unsupported = RN.host_argv(ctx, prep, run_dir, prep.turns, scn)
        if unsupported:
            raise ValueError(f"host does not support {unsupported}")
        log(f"sampled run: {' '.join(argv[:1])} ... ({prep.turns} turns, {a.hz} Hz)")
        t0 = time.perf_counter()
        proc, h, tid = SM.launch_suspended(argv, run_dir / "stdout.log")
        s = SM.sample_process(proc, h, tid, a.hz, a.stack_bytes, a.max_depth, fp, log)
        rc = proc.wait()
        wall = time.perf_counter() - t0
        rj = C.read_json(run_dir / "run.json") if (run_dir / "run.json").is_file() else None
        dg = run_dir / "digest.bin"
        meta = {"title": name, "scenario": f"{item.name} ({len(prep.scenario.get('civs', []))} civs + {len(prep.scenario.get('minors', []))} minors, "
                f"{prep.scenario.get('world_size')})", "dll": f"{dll0.label} ({dll.sha256[:12]})", "turns": f"{prep.turns} requested, "
                f"{(rj or {}).get('turns_completed')} completed", "host": f"{(rj or {}).get('host')} ({Path(host_cmd[0]).parent.name}), exit {rc}, wall {wall:.1f} s"}
        if a.verify and ref is not None:
            sampled_sha = C.sha256_bytes(dg.read_bytes()) if dg.is_file() else None
            eq = bool(ref.get("digest_sha256") and sampled_sha == ref["digest_sha256"])
            sim = sum(t.get("ms", 0.0) for t in (rj or {}).get("turns", []))
            v: dict[str, Any] = {"digest_equal": eq, "ref_final": ref.get("final_digest"), "sampled_final": (rj or {}).get("final_digest"),
                                 "ref_sim_ms": ref["sim_ms"], "sampled_sim_ms": round(sim, 1),
                                 "overhead": round(sim / ref["sim_ms"], 3) if ref["sim_ms"] else None, "inherent_nondeterminism": False}
            text = "digest.bin identical to the unsampled run" if eq else "DIGEST MISMATCH between sampled and unsampled run"
            if not eq and dg.is_file() and (Path(ref["run_dir"]) / "digest.bin").is_file():
                from .. import digestbin
                da = digestbin.parse(Path(ref["run_dir"]) / "digest.bin")
                cmp_s = digestbin.compare(da, digestbin.parse(dg))
                v["sampled_vs_ref"] = {k: cmp_s[k] for k in ("first_divergent_turn", "divergent_segments", "n_divergent_turns")}
                log(f"digests differ (first turn {cmp_s['first_divergent_turn']}: {cmp_s['divergent_segments'][:6]}); running a second unsampled run to classify (A/A) ...")
                aa_dir = pdir / "aa"
                aa = RN.execute(ctx, prep, aa_dir)
                cmp_a = digestbin.compare(da, digestbin.parse(aa_dir / "digest.bin")) if (aa_dir / "digest.bin").is_file() else None
                if cmp_a is not None:
                    v["aa_vs_ref"] = {k: cmp_a[k] for k in ("first_divergent_turn", "divergent_segments", "n_divergent_turns", "equal")}
                    v["inherent_nondeterminism"] = not cmp_a["equal"]
                    text += (f"; the two unsampled runs differ as well (first turn {cmp_a['first_divergent_turn']}, segments {cmp_a['divergent_segments'][:4]}): inherent "
                             f"nondeterminism of this scenario, not caused by sampling (sampled vs unsampled: first turn {cmp_s['first_divergent_turn']}, "
                             f"segments {cmp_s['divergent_segments'][:4]})" if not cmp_a["equal"] else
                             "; the second unsampled run matches the first: SAMPLING CHANGED THE RESULT")
                shutil.rmtree(aa_dir / "state-0", ignore_errors=True)
                RN._prune(aa_dir)
            v["text"] = text + f" (final {(rj or {}).get('final_digest')}); Update() time sampled {sim / 1000:.1f} s vs unsampled {ref['sim_ms'] / 1000:.1f} s " \
                               f"(x{sim / ref['sim_ms']:.3f})" if ref["sim_ms"] else text
            v["ok"] = eq or v["inherent_nondeterminism"]
            meta["verify"] = v
            log(v["text"])
        RN._prune(run_dir)
        if rc != 0:
            log(f"warning: host exit code {rc}")
    s.save(pdir / "samples.json.gz")
    doc = load_samples(pdir / "samples.json.gz")
    res = analyze(doc, rj, update_re=a.update_re, digest_re=a.digest_re, windows=a.windows)
    write_outputs(pdir, res, meta)
    print(f"samples {s.stats['samples']} ({s.stats['achieved_hz']} Hz), phases " + ", ".join(f"{k} {v['pct']}%" for k, v in res["phases"].items()))
    sc = res["scopes"].get("update") or res["scopes"]["all"]
    am = sc["amdahl"]
    print(f"Amdahl (update scope): p_DLL(self) {am['p_dll_self'] * 100:.1f}% -> max speedup x{am['max_speedup_dll_self']}; "
          f"DLL-driven {am['p_dll_driven'] * 100:.1f}% -> x{am['max_speedup_dll_driven']}; host-driven {am['p_host_driven'] * 100:.1f}%")
    print(f"results: {pdir}")
    if meta.get("verify") and not meta["verify"]["ok"]:
        return 6
    return 0
