"""civsim.detlab oos - compare the desync evidence of several machines of one multiplayer game.

Usage:
  python -m civsim.detlab oos <report> <report> [<report> ...] [--json OUT] [--context N]

A <report> is a LekmodReport-*.zip (a zip of a player's Logs and autosaves), a directory (an unpacked report, a Civ5 Logs folder or a
civhost run dir), or a LekmodNG-turns.log file. Nothing is extracted to disk; zips are read in memory.

What it does:
  1. parses every machine's Logs/LekmodNG-turns.log (HYG-0010, lines "LTL1 t=<turn> key=value ...") and prints the first turn at
     which any machine differs: the differing keys (g game counters, m plots, v visibility, Txx team, Pxx player with the
     sub-field: econ / cities / units / policies), the machines grouped by value, and the jon / map RNG seed and call-count deltas;
  2. compares RandCalls.csv (RandLog=1) of every machine with the first one and prints the first differing RNG call with context
     (the Instance column is a pointer and is ignored);
  3. summarises the exe's own OOS lines in net_message_debug.log (RNG out of sync, City/Unit/Plot out of sync, resync);
  4. lists the autosaves of each report next to the divergent turn (the byte-level save diff is a follow-up, not done here).
Exit: 0 all turn logs equal, 6 divergence found, 2 usage / fewer than two turn logs.
"""
from __future__ import annotations

import io
import json
import re
import sys
import zipfile
from dataclasses import dataclass, field
from pathlib import Path

TURNLOG = "lekmodng-turns.log"
IGNORED_KEYS = {"t", "ap", "us", "kc"}      # differ between machines by design (active player, wall-clock cost)
PLAYER_FIELDS = ("state", "cities", "units", "gold", "econ", "cities_hash", "units_hash", "policies")
OOS_PATTERNS = [
    ("rng", re.compile(r"Random Number Generators are out of sync", re.I)),
    ("city", re.compile(r"City out of sync", re.I)),
    ("unit", re.compile(r"Unit out of sync", re.I)),
    ("plot", re.compile(r"Plot out of sync", re.I)),
    ("player", re.compile(r"Player out of sync", re.I)),
    ("variable", re.compile(r"Variable Out Of Sync", re.I)),
    ("resync", re.compile(r"NetForceResync|resync", re.I)),
    ("not-local", re.compile(r"does not exist locally", re.I)),
]


# ---------------------------------------------------------------------------------------------------------------- inputs
@dataclass
class Source:
    label: str
    files: dict[str, bytes] = field(default_factory=dict)      # lower-case base name -> bytes (the newest/biggest wins)
    paths: dict[str, str] = field(default_factory=dict)        # lower-case base name -> original path inside the source
    saves: list[str] = field(default_factory=list)
    meta: dict = field(default_factory=dict)


WANTED = (TURNLOG, "net_message_debug.log", "randcalls.csv", "report.json", "state.json")


def _take(src: Source, name: str, data_fn, size: int) -> None:
    base = name.replace("\\", "/").rsplit("/", 1)[-1]
    low = base.lower()
    if low.endswith(".civ5save"):
        src.saves.append(base)
        return
    if low in WANTED and (low not in src.files or size > len(src.files[low])):
        src.files[low] = data_fn()
        src.paths[low] = name


def load_source(arg: str) -> Source:
    p = Path(arg)
    if p.is_file() and zipfile.is_zipfile(p):
        src = Source(label=p.stem)
        with zipfile.ZipFile(p) as z:
            for info in z.infolist():
                if not info.is_dir():
                    _take(src, info.filename, lambda i=info: z.read(i), info.file_size)
    elif p.is_dir():
        src = Source(label=p.name)
        for f in sorted(p.rglob("*")):
            if f.is_file():
                _take(src, str(f.relative_to(p)), f.read_bytes, f.stat().st_size)
    elif p.is_file():
        src = Source(label=p.parent.name or p.stem)
        _take(src, TURNLOG if p.name.lower() != TURNLOG else p.name, p.read_bytes, p.stat().st_size)
        if TURNLOG not in src.files:                       # any file name given directly is taken as a turn log
            src.files[TURNLOG] = p.read_bytes()
            src.paths[TURNLOG] = str(p)
    else:
        raise FileNotFoundError(arg)
    if "report.json" in src.files:
        try:
            src.meta = json.loads(src.files["report.json"].decode("utf-8-sig"))
            name = src.meta.get("computer_name") or src.meta.get("computer")
            if name:
                src.label = str(name)
        except (ValueError, UnicodeDecodeError):
            pass
    src.saves.sort()
    return src


# ---------------------------------------------------------------------------------------------------------------- turn log
@dataclass
class TurnLog:
    records: dict[int, dict[str, str]] = field(default_factory=dict)   # turn -> key -> value (first record of a turn)
    duplicates: dict[int, int] = field(default_factory=dict)           # turn -> extra records seen (reloads in one session)
    active_player: str | None = None
    lines: int = 0


def parse_turnlog(text: str) -> TurnLog:
    """LTL1 lines; an optional timestamp prefix ("[12.345] ") is ignored. A line with 'jon=' starts a new record for its turn."""
    log = TurnLog()
    cur_turn: int | None = None
    skipping = False
    for raw in text.splitlines():
        i = raw.find("LTL1 ")
        if i < 0:
            continue
        toks = raw[i + 5:].split()
        kv = {}
        for tok in toks:
            k, sep, v = tok.partition("=")
            if sep:
                kv[k] = v
        if "t" not in kv:
            continue
        try:
            turn = int(kv["t"])
        except ValueError:
            continue
        log.lines += 1
        if "jon" in kv:                                   # header line: a new record
            if turn in log.records:
                log.duplicates[turn] = log.duplicates.get(turn, 0) + 1
                skipping = True
            else:
                log.records[turn] = {}
                skipping = False
            cur_turn = turn
            if log.active_player is None and "ap" in kv:
                log.active_player = kv["ap"]
        elif turn != cur_turn:                            # continuation line of a record whose header is missing
            if turn in log.records:
                skipping = True
            else:
                log.records[turn] = {}
                skipping = False
            cur_turn = turn
        if not skipping:
            log.records[turn].update(kv)
    return log


def explain_key(key: str, values: dict[str, str]) -> str:
    """Human-readable detail of one differing key across machines."""
    if key in ("jon", "map"):
        parts = {m: v.split(",") for m, v in values.items()}
        calls = {m: int(p[1]) for m, p in parts.items() if len(p) > 1 and p[1].isdigit()}
        base = min(calls.values()) if calls else 0
        desc = ", ".join(f"{m}: seed {p[0]} calls {p[1] if len(p) > 1 else '?'}"
                         + (f" (+{calls[m] - base})" if m in calls and calls[m] != base else "")
                         + (f" resets {p[2]}" if len(p) > 2 else "") for m, p in parts.items())
        return f"{key} RNG: {desc}"
    if key.startswith("P") and key[1:].isdigit():
        split = {m: v.split(",") for m, v in values.items()}
        diff = []
        for idx, name in enumerate(PLAYER_FIELDS):
            vals = {m: (s[idx] if idx < len(s) else None) for m, s in split.items()}
            if len(set(vals.values())) > 1:
                diff.append(name + (" " + "/".join(str(v) for v in vals.values()) if name in ("state", "cities", "units", "gold") else ""))
        return f"player {key[1:]}: " + ", ".join(diff)
    if key.startswith("T") and key[1:].isdigit():
        split = {m: v.split(",") for m, v in values.items()}
        techs = {m: s[0] for m, s in split.items()}
        what = "techs known " + "/".join(techs.values()) if len(set(techs.values())) > 1 else "tech progress or war state"
        return f"team {key[1:]}: {what}"
    names = {"g": "game counters (turn, cities, population, alive flags)", "m": "plots (owner, terrain, feature, resource, improvement, route, units per plot)",
             "v": "plot visibility per team"}
    return names.get(key, key)


def compare_turnlogs(logs: dict[str, TurnLog]) -> dict:
    machines = list(logs)
    common = sorted(set.intersection(*(set(l.records) for l in logs.values()))) if logs else []
    out: dict = {"machines": machines, "turns_common": [common[0], common[-1]] if common else None,
                 "turns_per_machine": {m: [min(l.records), max(l.records), len(l.records)] if l.records else None for m, l in logs.items()},
                 "active_player": {m: l.active_player for m, l in logs.items()},
                 "duplicates": {m: l.duplicates for m, l in logs.items() if l.duplicates},
                 "first_divergence": None, "last_equal_turn": None, "divergent_turns": 0}
    for turn in common:
        recs = {m: logs[m].records[turn] for m in machines}
        keys = sorted(set().union(*recs.values()) - IGNORED_KEYS)
        diffs = []
        for k in keys:
            vals = {m: recs[m].get(k, "-") for m in machines}
            if len(set(vals.values())) > 1:
                groups: dict[str, list[str]] = {}
                for m, v in vals.items():
                    groups.setdefault(v, []).append(m)
                diffs.append({"key": k, "values": vals, "groups": list(groups.values()), "detail": explain_key(k, vals)})
        if diffs:
            out["divergent_turns"] += 1
            if out["first_divergence"] is None:
                out["first_divergence"] = {"turn": turn, "diffs": diffs}
        elif out["first_divergence"] is None:
            out["last_equal_turn"] = turn
    return out


# ---------------------------------------------------------------------------------------------------------------- exe logs
def summarize_netlog(data: bytes, limit: int = 6) -> dict:
    counts: dict[str, int] = {}
    first: list[str] = []
    for line in data.decode("latin-1").splitlines():
        for name, rx in OOS_PATTERNS:
            if rx.search(line):
                counts[name] = counts.get(name, 0) + 1
                if len(first) < limit:
                    first.append(line.strip()[:300])
                break
    return {"counts": counts, "first_lines": first}


def _randcalls_rows(data: bytes) -> tuple[list[str], list[tuple]]:
    lines = data.decode("latin-1").splitlines()
    header = [h.strip().lower() for h in lines[0].split(",")] if lines else []
    skip = header.index("instance") if "instance" in header else -1
    rows = []
    for ln in lines[1:]:
        cols = [c.strip() for c in ln.split(",")]
        if skip >= 0 and len(cols) > skip:
            cols = cols[:skip] + cols[skip + 1:]
        rows.append(tuple(cols))
    return header, rows


def compare_randcalls(files: dict[str, bytes], context: int = 3) -> dict | None:
    if len(files) < 2:
        return None
    names = list(files)
    ref_name = names[0]
    _, ref = _randcalls_rows(files[ref_name])
    result = {"reference": ref_name, "rows": {ref_name: len(ref)}, "first_difference": {}}
    for other in names[1:]:
        _, rows = _randcalls_rows(files[other])
        result["rows"][other] = len(rows)
        n = min(len(ref), len(rows))
        idx = next((i for i in range(n) if ref[i] != rows[i]), None)
        if idx is None:
            result["first_difference"][other] = None if len(ref) == len(rows) else {"index": n, "note": "one log is a prefix of the other"}
            continue
        lo, hi = max(0, idx - context), idx + context + 1
        result["first_difference"][other] = {"index": idx, ref_name: [",".join(r) for r in ref[lo:hi]], other: [",".join(r) for r in rows[lo:hi]]}
    return result


# ---------------------------------------------------------------------------------------------------------------- report
def _save_hint(saves: list[str], turn: int | None) -> list[str]:
    if turn is None:
        return saves[-6:]
    want = {f"{t:04d}" for t in (turn - 1, turn)}
    return [s for s in saves if any(w in s for w in want) or "resync" in s.lower()]


def analyse(args: list[str], context: int = 3) -> tuple[dict, str]:
    sources = [load_source(a) for a in args]
    seen: dict[str, int] = {}
    for s in sources:                                     # unique labels
        if s.label in seen:
            seen[s.label] += 1
            s.label = f"{s.label}#{seen[s.label]}"
        else:
            seen[s.label] = 1
    logs = {s.label: parse_turnlog(s.files[TURNLOG].decode("latin-1")) for s in sources if TURNLOG in s.files}
    res: dict = {"sources": [{"label": s.label, "turnlog": s.paths.get(TURNLOG), "files": sorted(s.paths.values()), "saves": s.saves,
                              "release": (s.meta.get("release") if s.meta else None)} for s in sources]}
    res["turnlogs"] = compare_turnlogs(logs) if len(logs) >= 2 else None
    res["netlog"] = {s.label: summarize_netlog(s.files["net_message_debug.log"]) for s in sources if "net_message_debug.log" in s.files}
    res["randcalls"] = compare_randcalls({s.label: s.files["randcalls.csv"] for s in sources if "randcalls.csv" in s.files}, context)

    L: list[str] = []
    L.append(f"oos: {len(sources)} report(s): " + ", ".join(s.label for s in sources))
    for s in sources:
        tl = logs.get(s.label)
        extra = f", release {res['sources'][sources.index(s)]['release']}" if s.meta.get("release") else ""
        L.append(f"  {s.label}: turn log {'%d turns' % len(tl.records) if tl else 'MISSING'}"
                 + (f" (active player {tl.active_player})" if tl and tl.active_player is not None else "")
                 + f", net_message_debug.log {'yes' if 'net_message_debug.log' in s.files else 'no'}, RandCalls.csv {'yes' if 'randcalls.csv' in s.files else 'no'}"
                 + f", {len(s.saves)} save(s){extra}")
    first_turn = None
    tc = res["turnlogs"]
    if tc is None:
        L.append("turn logs: fewer than two machines have Logs\\LekmodNG-turns.log (LoggingEnabled=1 and the release DLL are needed)")
    else:
        if tc["duplicates"]:
            L.append(f"note: turns logged twice (game reloaded in one session; the first record is used): {tc['duplicates']}")
        fd = tc["first_divergence"]
        rng = tc["turns_common"]
        if fd is None:
            L.append(f"turn logs: EQUAL on all common turns {rng[0]}..{rng[1]}" if rng else "turn logs: no common turns")
        else:
            first_turn = fd["turn"]
            L.append(f"turn logs: FIRST DIVERGENT TURN {fd['turn']} (last equal turn: {tc['last_equal_turn']}; divergent turns: {tc['divergent_turns']} of the common {rng[0]}..{rng[1]})")
            for d in fd["diffs"]:
                groups = " | ".join("+".join(g) for g in d["groups"])
                L.append(f"  {d['key']:>4}: {d['detail']}   [{groups}]")
            L.append("  (the line of turn T is written at the start of doTurn: the cause is in turn T's commands / AI moves, or in turn T-1's processing)")
    for label, nl in res["netlog"].items():
        if nl["counts"]:
            L.append(f"net_message_debug.log {label}: " + ", ".join(f"{k} {v}" for k, v in nl["counts"].items()))
            for ln in nl["first_lines"]:
                L.append(f"    {ln}")
    rc = res["randcalls"]
    if rc:
        for other, fdiff in rc["first_difference"].items():
            if fdiff is None:
                L.append(f"RandCalls.csv {rc['reference']} vs {other}: identical ({rc['rows'][other]} calls)")
            elif "note" in fdiff:
                L.append(f"RandCalls.csv {rc['reference']} vs {other}: equal for {fdiff['index']} calls, {fdiff['note']}")
            else:
                L.append(f"RandCalls.csv {rc['reference']} vs {other}: first differing RNG call #{fdiff['index']} (Turn, Slice, Range, Value, Seed, Type, Location):")
                for side in (rc["reference"], other):
                    L.append(f"  {side}:")
                    for r in fdiff[side]:
                        L.append(f"    {r}")
    for s in sources:
        hint = _save_hint(s.saves, first_turn)
        if hint:
            L.append(f"saves {s.label}: " + ", ".join(hint))
    if first_turn is not None:
        L.append("next step (manual, not automated yet): extract two machines' autosaves of that turn with python -m civsim.saveio extract, import with "
                 "civhost --import-save and diff with --dump-state-at / civsim.enginehost.digest_diff (see headless\\tools\\README.md)")
    return res, "\n".join(L)


def main(argv: list[str]) -> int:
    import argparse
    ap = argparse.ArgumentParser(prog="civsim.detlab oos", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("reports", nargs="+")
    ap.add_argument("--json", help="write the full result as JSON")
    ap.add_argument("--context", type=int, default=3, help="RandCalls.csv rows around the first difference")
    a = ap.parse_args(argv)
    if len(a.reports) < 2:
        print("oos: give at least two reports (one per machine)", file=sys.stderr)
        return 2
    try:
        res, text = analyse(a.reports, a.context)
    except FileNotFoundError as e:
        print(f"oos: not found: {e}", file=sys.stderr)
        return 2
    print(text)
    if a.json:
        Path(a.json).write_text(json.dumps(res, indent=2), encoding="utf-8")
    tc = res["turnlogs"]
    if tc is None:
        return 2
    return 6 if tc["first_divergence"] else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
