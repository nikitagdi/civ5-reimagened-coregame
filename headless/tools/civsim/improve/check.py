"""check-registry: the json registry, the flag sets and the sources must agree."""
from __future__ import annotations

import re
from pathlib import Path

from . import registry as R

FLAG_RE = re.compile(r"LEKMOD_HEADLESS_(?:OPT|FIX|HYG|DIAG)_[A-Z0-9_]+")
# not improvement flags: the shadow-check switches of optimized-diag builds (OPT-0090 has its shadow switch in its record)
NOT_IMPROVEMENT_FLAGS = {"LEKMOD_HEADLESS_OPT_VERIFY", "LEKMOD_HEADLESS_OPT_ASTAR_UNREACHABLE_STOP_SHADOW", "LEKMOD_HEADLESS_DIAG_TURN_LOG_EVERY"}  # + HYG-0010's every-N-turns knob
PP_IF_RE = re.compile(r"^\s*#\s*(?:if|ifdef|ifndef|elif)\b(.*)$")
HEADER_FLAG_RE = re.compile(r"^//\s*(LEKMOD_HEADLESS_[A-Z0-9_]+)\s")
LUA_PATH_RE = re.compile(r"^([^\s(]+\.lua)\b")


def scan_sources(src_dir: Path) -> dict[str, list[str]]:
    """flag -> ['file:line', ...] for every preprocessor conditional (#if/#ifdef/#elif) that mentions an improvement flag."""
    found: dict[str, list[str]] = {}
    for path in sorted(src_dir.rglob("*")):
        if path.suffix.lower() not in (".cpp", ".h", ".inl") or not path.is_file():
            continue
        try:
            text = path.read_bytes().decode("latin-1")
        except OSError:
            continue
        for n, line in enumerate(text.splitlines(), 1):
            m = PP_IF_RE.match(line)
            if not m:
                continue
            for flag in FLAG_RE.findall(m.group(1)):
                if flag not in NOT_IMPROVEMENT_FLAGS:
                    found.setdefault(flag, []).append(f"{path.name}:{n}")
    return found


def header_flags(header: Path) -> set[str]:
    """Flags listed as one-line comments in HeadlessImprovements.h."""
    out: set[str] = set()
    for line in header.read_text(encoding="latin-1").splitlines():
        m = HEADER_FLAG_RE.match(line)
        if m:
            out.add(m.group(1))
    return out


def _in_tree(rec: dict) -> bool:
    """The entry's code is in the sources behind its flag (OPT: prototyped or later; FIX/HYG: implemented or later)."""
    if rec.get("kind") == "opt":
        return rec.get("status") in R.OPT_IN_TREE
    return rec.get("status") in R.IMPLEMENTED


def check(reg: R.Registry, src_dir: Path = R.SRC_DIR, repo: Path = R.REPO) -> tuple[list[str], list[str]]:
    """Returns (errors, notes)."""
    errs = R.validate(reg)
    notes: list[str] = []
    by_id = reg.by_id()
    flag_of = {r["id"]: r.get("flag") for r in reg.records()}

    # 1. every entry of every flag set exists in a record with the same flag
    set_flags: dict[str, set[str]] = {}
    for setname, entries in reg.flagsets.items():
        flags = set()
        for e in entries:
            rid, flag = e.get("id"), e.get("flag")
            if rid not in by_id:
                errs.append(f"flagsets[{setname}]: {rid} has no record")
                continue
            if flag != flag_of[rid] or not flag:
                errs.append(f"flagsets[{setname}]: {rid} flag {flag!r} != record flag {flag_of[rid]!r}")
            if flag in flags:
                errs.append(f"flagsets[{setname}]: duplicate flag {flag}")
            flags.add(flag)
        set_flags[setname] = flags

    # 2. every flag in a set is referenced by an #if in the sources
    used = scan_sources(src_dir)
    for setname, flags in set_flags.items():
        for flag in sorted(flags):
            if flag not in used:
                errs.append(f"flagsets[{setname}]: {flag} is not referenced by any #if/#ifdef in {src_dir.name}")

    # 3. release subset of approved, and release == opt + approved fixes
    approved = {r["flag"] for r in reg.fixes + reg.hyg + reg.opts
                if r.get("flag") and r.get("release") and _in_tree(r)
                and r.get("approval") != "awaiting-user"}
    for flag in sorted(set_flags.get("release", set()) - approved):
        errs.append(f"release set contains {flag}, which is not an approved implemented entry (release=true)")
    expected_release = set_flags.get("opt", set()) | {f for f in set_flags.get("fix", set()) if f in approved}
    if set_flags.get("release", set()) != expected_release:
        errs.append("release set != opt set + approved fix set: "
                    f"missing {sorted(expected_release - set_flags.get('release', set()))}, "
                    f"extra {sorted(set_flags.get('release', set()) - expected_release)}")
    for rec in reg.fixes + reg.hyg + reg.opts:
        if rec.get("release") and rec.get("flag") and rec["flag"] not in set_flags.get("release", set()):
            errs.append(f"{rec['id']}: release=true but flag {rec['flag']} is not in the release set")
        if not rec.get("release") and rec.get("flag") and rec["flag"] in set_flags.get("release", set()):
            errs.append(f"{rec['id']}: release=false but flag {rec['flag']} is in the release set")

    # 4. every improvement flag used in the sources is registered, and every registered flag is used
    registered = {r["flag"] for r in reg.records() if r.get("flag")}
    for flag in sorted(set(used) - registered):
        errs.append(f"{flag} is used in {used[flag][0]} but has no registry record")
    for rec in reg.records():
        flag = rec.get("flag")
        if flag and _in_tree(rec) and flag not in used:
            errs.append(f"{rec['id']}: implemented but {flag} is not referenced in the sources")
    # an OPT in a flag set must have its code in the tree (prototyped or later)
    for rec in reg.opts:
        for setname, flags in set_flags.items():
            if rec.get("flag") in flags and rec.get("status") not in R.OPT_IN_TREE:
                errs.append(f"{rec['id']}: in flag set '{setname}' but status is {rec.get('status')!r}")

    # 5. HeadlessImprovements.h lists exactly the registered flags (one-line comments)
    header = src_dir / "HeadlessImprovements.h"
    if header.exists():
        listed = header_flags(header)
        for flag in sorted(registered - listed):
            errs.append(f"{flag} is not listed in HeadlessImprovements.h")
        for flag in sorted(listed - registered):
            errs.append(f"HeadlessImprovements.h lists {flag}, which has no registry record")
    else:
        errs.append(f"missing {header}")

    # 6. Lua records: the FIX-xxxx marker must be present in the listed Lua file
    for rec in reg.fixes:
        if not rec.get("lua") or rec.get("status") == "not-applicable":
            continue
        paths = [m.group(1) for f in rec.get("files", []) if (m := LUA_PATH_RE.match(f))]
        if not paths:
            errs.append(f"{rec['id']}: lua record without a .lua file in 'files'")
        for p in paths:
            lua = repo / p
            if not lua.exists():
                errs.append(f"{rec['id']}: {p} not found")
            elif rec["id"] not in lua.read_text(encoding="utf-8", errors="replace"):
                errs.append(f"{rec['id']}: marker '{rec['id']}' not found in {p}")

    notes.append(f"records: {len(reg.fixes)} fix/validation, {len(reg.hyg)} hyg, {len(reg.opts)} opt; "
                 f"flag sets: " + ", ".join(f"{k}={len(v)}" for k, v in sorted(set_flags.items())))
    return errs, notes
