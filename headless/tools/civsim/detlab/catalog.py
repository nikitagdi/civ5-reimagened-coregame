"""Desync catalog: headless/determinism/catalog.json -> checks, listing.

  python -m civsim.detlab catalog list
  python -m civsim.detlab catalog show DS-0003
  python -m civsim.detlab catalog check [--no-lines]
  python -m civsim.detlab catalog render [--out headless/generated-docs/DESYNC-CATALOG.md] [--catalog PATH]
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[4]
CATALOG = REPO / "headless" / "determinism" / "catalog.json"
OUT_MD = REPO / "headless" / "generated-docs" / "DESYNC-CATALOG.md"
GAME_LEKMOD_DLC = Path(os.environ.get("CIV5_GAME_DIR") or r"C:\Program Files (x86)\Steam\steamapps\common\Sid Meier's Civilization V") / "Assets" / "DLC" / "LEKMOD_v34.15"   # CIV5_GAME_DIR: game install

STATUS_RE = re.compile(r"^(suspected|likely|confirmed|wontfix|fixed-in:\S+)$")
CLASSES = {f"C{i}" for i in range(1, 10)}
BEHAVIOR = {"none", "sim", "balance"}
DETECTION = {"rng", "silent"}
AXES = {"active_player", "heap_pattern", "stack_pattern", "addr_shift", "warm-fresh", "saveload", "lockstep",
        "clock_offset", "net_faults", "async_seed", "lua_random",
        # lockstep peers whose human slots are played by seeded Lua bots (python -m civsim.lockstep bots)
        "lockstep-bots",
        # civhost perturbation flags (civhost --help): the axis name is the flag with '_' for '-'
        "heap_fill", "stack_fill", "heap_shift", "heap_jitter", "fpu_pc", "fpu_pc_global"}
REQUIRED = ("id", "title", "class", "status", "location", "mechanism", "evidence", "trigger", "proposed_fix",
            "behavior_change", "trap", "confidence", "impact")
LOC_RE = re.compile(r"^(?P<path>[^:]+(?::[^:]+)?):(?P<line>\d+)$")


def load(path: Path = CATALOG) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def rank(e: dict) -> float:
    return round(float(e.get("confidence", 0)) * float((e.get("impact") or {}).get("score", 0)), 2)


def ranked(cat: dict) -> list[dict]:
    return sorted(cat.get("entries", []), key=lambda e: (-rank(e), e.get("id", "")))


# ---------------------------------------------------------------------------------------------------------------- check
def check_schema(cat: dict) -> list[str]:
    errs: list[str] = []
    if cat.get("schema_version") != 1:
        errs.append(f"schema_version must be 1, got {cat.get('schema_version')!r}")
    seen: set[str] = set()
    for i, e in enumerate(cat.get("entries", [])):
        eid = e.get("id", f"#{i}")
        for k in REQUIRED:
            if k not in e or e[k] in (None, "", []):
                errs.append(f"{eid}: missing field {k!r}")
        if not re.fullmatch(r"DS-\d{4}", str(e.get("id", ""))):
            errs.append(f"{eid}: id must look like DS-0001")
        if eid in seen:
            errs.append(f"{eid}: duplicate id")
        seen.add(eid)
        if e.get("class") not in CLASSES:
            errs.append(f"{eid}: class {e.get('class')!r} not in C1..C9")
        for c in e.get("secondary_classes", []):
            if c not in CLASSES:
                errs.append(f"{eid}: secondary class {c!r} not in C1..C9")
        if not STATUS_RE.match(str(e.get("status", ""))):
            errs.append(f"{eid}: bad status {e.get('status')!r}")
        if e.get("behavior_change") not in BEHAVIOR:
            errs.append(f"{eid}: behavior_change {e.get('behavior_change')!r} not in {sorted(BEHAVIOR)}")
        conf = e.get("confidence")
        if not isinstance(conf, (int, float)) or not 0 <= conf <= 1:
            errs.append(f"{eid}: confidence must be a number in 0..1")
        imp = e.get("impact") or {}
        if not isinstance(imp.get("score"), int) or not 1 <= imp.get("score") <= 5:
            errs.append(f"{eid}: impact.score must be an int 1..5")
        if imp.get("detection") not in DETECTION:
            errs.append(f"{eid}: impact.detection must be one of {sorted(DETECTION)}")
        trap = e.get("trap") or {}
        if not trap.get("axis") or not trap.get("idea"):
            errs.append(f"{eid}: trap needs axis and idea")
        for a in trap.get("axis", []):
            if a not in AXES:
                errs.append(f"{eid}: trap axis {a!r} not in {sorted(AXES)}")
        for ev in e.get("evidence", []):
            if not isinstance(ev, dict) or "loc" not in ev or "quote" not in ev:
                errs.append(f"{eid}: evidence items need loc and quote")
        for loc in [e.get("location", "")] + e.get("locations", []):
            if not LOC_RE.match(str(loc)):
                errs.append(f"{eid}: location {loc!r} is not file:line")
    for section in ("rejected", "mask_candidates", "related_bugs"):
        for j, r in enumerate(cat.get(section, [])):
            for k in ("what", "where"):
                if not r.get(k):
                    errs.append(f"{section}[{j}]: missing {k!r}")
    return errs


def _file_lines(path: str, rev: str, cache: dict) -> list[str] | None:
    if path in cache:
        return cache[path]
    lines = None
    if path.startswith("DLC:"):
        p = GAME_LEKMOD_DLC / path[4:]
        if p.exists():
            lines = p.read_text(encoding="latin-1").split("\n")
    else:
        r = subprocess.run(["git", "show", f"{rev}:{path}"], cwd=REPO, capture_output=True)
        if r.returncode == 0:
            lines = r.stdout.decode("latin-1").split("\n")
    cache[path] = lines
    return lines


def _norm(s: str) -> str:
    return re.sub(r"\s+", " ", s).strip()


def check_lines(cat: dict) -> tuple[list[str], list[str]]:
    """file:line refs resolve at base_rev; evidence quotes appear within +-3 lines and C++ refs are not compiled out
    under the golden defines (warnings only)."""
    from . import scan
    errs, warns = [], []
    rev = cat.get("base_rev", "HEAD")
    cache: dict = {}
    pp_cache: dict = {}
    scan._REV, scan._GLOBAL_DEFINES = rev, None

    def compiled_out(path: str, line: int) -> bool:
        if not re.search(r"\.(cpp|h)$", path) or path.startswith("DLC:"):
            return False
        if path not in pp_cache:
            raw = "\n".join(cache[path])
            pp_cache[path] = scan.preprocess(scan.strip_cpp_comments(raw), scan.golden_defines()).split("\n")
        raw_line = cache[path][line - 1].strip()
        if not raw_line or raw_line.startswith(("//", "#")):     # comments and directives are always blank after pp
            return False
        return not pp_cache[path][line - 1].strip()

    def resolve(owner: str, loc: str) -> list[str] | None:
        m = LOC_RE.match(loc)
        if not m:
            return None
        lines = _file_lines(m.group("path"), rev, cache)
        if lines is None:
            if m.group("path").startswith("DLC:") and not GAME_LEKMOD_DLC.exists():
                return None                         # install not present on this machine: skip
            errs.append(f"{owner}: {loc}: file not found at {rev}")
            return None
        if not 1 <= int(m.group("line")) <= len(lines):
            errs.append(f"{owner}: {loc}: line out of range ({len(lines)} lines)")
            return None
        if owner.startswith("DS-") and compiled_out(m.group("path"), int(m.group("line"))):
            warns.append(f"{owner}: {loc}: line is compiled out under the golden defines")
        return lines

    for e in cat.get("entries", []):
        for loc in [e["location"]] + e.get("locations", []):
            resolve(e["id"], loc)
        for ev in e.get("evidence", []):
            loc = ev.get("loc", "")
            if loc == "scanner" or not LOC_RE.match(loc):
                continue
            lines = resolve(e["id"], loc)
            if lines is None:
                continue
            line = int(LOC_RE.match(loc).group("line"))
            window = _norm(" ".join(lines[max(0, line - 4):line + 3]))
            head = re.split(r"\s{2}\(|\s\.\.\.|\s\(|;\s", ev["quote"])[0]
            head = _norm(head)[:28]
            if head and head not in window:
                warns.append(f"{e['id']}: quote not found near {loc}: {head!r}")
    for section in ("rejected", "mask_candidates", "related_bugs"):
        for r in cat.get(section, []):
            if LOC_RE.match(r.get("where", "")):
                resolve(section, r["where"])
    return list(dict.fromkeys(errs)), list(dict.fromkeys(warns))


# --------------------------------------------------------------------------------------------------------------- render
def _cell(s) -> str:
    return str(s).replace("|", "\\|").replace("\n", " ")


def _code(s: str) -> str:
    s = re.sub(r"\s+", " ", str(s))
    tick = "``" if "`" in s else "`"
    pad = " " if tick == "``" else ""
    return f"{tick}{pad}{s}{pad}{tick}"


def _classes(e: dict) -> str:
    return "/".join([e["class"]] + list(e.get("secondary_classes", [])))


def _trap_short(e: dict) -> str:
    t = e.get("trap") or {}
    axes = ", ".join(t.get("axis", []))
    return f"{t['id']} ({axes})" if t.get("id") else axes


def render(cat: dict) -> str:
    entries = ranked(cat)
    out: list[str] = []
    w = out.append
    w("# Desync catalog")
    w("")
    w("<!-- Generated by `python -m civsim.detlab catalog render` from headless/determinism/catalog.json. Do not edit by hand. -->")
    w("")
    caveat = cat.get("generated_by", "").rpartition("; ")[2]
    caveat = (caveat[:1].upper() + caveat[1:] + ".") if caveat else ""
    w(f"Candidate desync sources in Lekmod {', '.join(cat.get('versions_audited', []))} (DLL and mod Lua), found by static audit "
      f"(`python -m civsim.detlab scan all --rev HEAD`) and manual triage. Classes follow "
      f"the determinism-lab taxonomy. {caveat}".rstrip())
    w("")
    w(f"- Line references are at git rev `{cat.get('base_rev')}`. {cat.get('line_refs', '').split('. ', 1)[-1]}")
    w("- `likely` means the mechanism was verified by reading the code; `confirmed` needs a trap or a perturbation axis to catch it.")
    w("- Rank = confidence × impact (impact scale below). `rng` detection means the exe OOS check would fire "
      "(RNG or FAutoVariable delta); `silent` means the state is outside the sync check until it cascades.")
    w("- Full resync note: on a full resync every peer, the host included, reloads the host's save, so C1 state "
      "does not split peers after a full resync. It does split a hot-joining or reconnecting peer from the others, "
      "and it makes \"after load\" differ from continuous play (the lab's `continue` test).")
    w("")
    w("## Top 10 to confirm first")
    w("")
    for i, e in enumerate(entries[:10], 1):
        w(f"{i}. **{e['id']}** {e['title']} ({_classes(e)}, {e['confidence']:.2f} × {e['impact']['score']} = "
          f"{rank(e):.2f}). Trap: {_trap_short(e)}.")
    w("")
    w("## Summary")
    w("")
    w("| # | id | class | status | conf | impact | rank | detection | behaviour change | fix | title | location |")
    w("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for i, e in enumerate(entries, 1):
        w(f"| {i} | [{e['id']}](#{e['id'].lower()}) | {_classes(e)} | {e['status']} | {e['confidence']:.2f} | "
          f"{e['impact']['score']} | {rank(e):.2f} | {e['impact']['detection']} | {e['behavior_change']} | "
          f"{', '.join(e.get('fix_refs', [])) or '-'} | {_cell(e['title'])} | {_code(e['location'])} |")
    w("")
    w("## Entries")
    for e in entries:
        w("")
        w(f"### {e['id']}")
        w("")
        w(f"**{e['title']}**")
        w("")
        w(f"- Class: {_classes(e)}. Status: {e['status']}. Confidence: {e['confidence']:.2f}. "
          f"Impact: {e['impact']['score']} ({e['impact']['detection']}). Rank: {rank(e):.2f}. "
          f"Behaviour change of the fix: {e['behavior_change']}.")
        locs = ", ".join(_code(l) for l in e.get("locations", []))
        w(f"- Location: {_code(e['location'])}" + (f"; also {locs}" if locs else ""))
        w(f"- Mechanism: {e['mechanism']}")
        w(f"- Trigger: {e['trigger']}")
        if e.get("symptom"):
            w(f"- Symptom: {e['symptom']}")
        if e["impact"].get("note"):
            w(f"- Impact note: {e['impact']['note']}")
        w("- Evidence:")
        for ev in e["evidence"]:
            where = "scanner" if ev["loc"] == "scanner" else _code(ev["loc"])
            w(f"  - {where}: {_code(ev['quote'])}")
        if e.get("community"):
            w("- Community (same class fixed upstream):")
            for c in e["community"]:
                w(f"  - {c}")
        w(f"- Proposed fix: {e['proposed_fix']}")
        t = e["trap"]
        w(f"- Trap: {('**' + t['id'] + '**, ') if t.get('id') else ''}axis {', '.join(t['axis'])}. {t['idea']}")
        if e.get("fix_refs"):
            w(f"- Fix journal: {', '.join(e['fix_refs'])} ")
        if e.get("scanner_rules"):
            w(f"- Scanner rules: {', '.join(_code(r) for r in e['scanner_rules'])}")
    for section, title, intro in (
            ("rejected", "Rejected candidates",
             "Scanner hits and suspicions that were checked and are not desync sources."),
            ("mask_candidates", "Mask candidates",
             "State that differs between peers or between continuous play and a load by design. "
             "Digest comparisons should mask it."),
            ("related_bugs", "Related deterministic bugs",
             "Found during the audit; they behave the same on every peer, so they are not desyncs.")):
        rows = cat.get(section, [])
        if not rows:
            continue
        w("")
        w(f"## {title}")
        w("")
        w(intro)
        w("")
        if section == "related_bugs":
            w("| what | where | fix | note |")
            w("|---|---|---|---|")
            for r in rows:
                w(f"| {_cell(r['what'])} | {_code(r['where'])} | {', '.join(r.get('fix_refs', [])) or '-'} | "
                  f"{_cell(r.get('note', ''))} |")
        else:
            w("| what | where | why |")
            w("|---|---|---|")
            for r in rows:
                w(f"| {_cell(r['what'])} | {_code(r['where'])} | {_cell(r.get('why', ''))} |")
    w("")
    w("## Impact scale")
    w("")
    for k, v in sorted(cat.get("impact_scale", {}).items()):
        w(f"- {k}: {v}")
    w("")
    return "\n".join(out)


# ------------------------------------------------------------------------------------------------------------------ cli
def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(prog="civsim.detlab catalog")
    ap.add_argument("cmd", choices=["list", "show", "check", "render"])
    ap.add_argument("id", nargs="?")
    ap.add_argument("--catalog", type=Path, default=CATALOG)
    ap.add_argument("--out", type=Path, default=OUT_MD)
    ap.add_argument("--no-lines", action="store_true", help="check: skip resolving file:line refs at base_rev")
    a = ap.parse_args(argv)
    cat = load(a.catalog)

    if a.cmd == "list":
        for e in ranked(cat):
            print(f"{e['id']}  {rank(e):4.2f}  {_classes(e):8} {e['status']:9} {e['title']}")
        return 0
    if a.cmd == "show":
        hit = [e for e in cat.get("entries", []) if e.get("id") == a.id]
        if not hit:
            print(f"no entry {a.id!r}", file=sys.stderr)
            return 1
        print(json.dumps(hit[0], indent=2, ensure_ascii=False))
        return 0
    if a.cmd == "check":
        errs = check_schema(cat)
        warns: list[str] = []
        if not a.no_lines and not errs:
            e2, warns = check_lines(cat)
            errs += e2
        for w in warns:
            print(f"warning: {w}")
        for e in errs:
            print(f"error: {e}")
        print(f"{len(cat.get('entries', []))} entries, {len(errs)} error(s), {len(warns)} warning(s)", file=sys.stderr)
        return 1 if errs else 0
    if a.cmd == "render":
        errs = check_schema(cat)
        if errs:
            for e in errs:
                print(f"error: {e}", file=sys.stderr)
            return 1
        a.out.parent.mkdir(parents=True, exist_ok=True)
        a.out.write_text(render(cat), encoding="utf-8", newline="\n")
        print(f"wrote {a.out}")
        return 0
    return 2
