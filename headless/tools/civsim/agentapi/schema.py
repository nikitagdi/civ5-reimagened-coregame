"""Own tiny schema validator (no jsonschema package) + semantic checks of the dictionary.

Schema language (a dict): t = type name or list of names (obj arr str int bool null any), p = {prop: schema},
req = [required props], extra = allow unknown props (default False), of = item schema (arr) / value schema (obj
without p), min/max = int bounds (int) or length bounds (str, arr), enum, re = regex a str must match.
"""
from __future__ import annotations

import re

MAX_PLAYERS, MAX_TEAMS, MAX_MAJOR_CIVS, MAX_CIV_PLAYERS = 64, 64, 22, 63
CLIENT_CLASSES = ("player", "turn", "meta", "lekmod_mp")
FORBIDDEN_CLASSES = ("server", "pregame", "unsupported")
STEPS = ("range", "class", "player", "ownership", "phase", "pending", "applicability", "rate")
RULE_RE = r"^(" + "|".join(STEPS) + r")/(auto|hand): .+"
PHASE_RE = r"^(turn_active|any|pending:[A-Za-z0-9_]+)$"

_PY = {"obj": dict, "arr": list, "str": str, "bool": bool, "null": type(None)}


def _type_ok(v: object, t: str) -> bool:
    if t == "any":
        return True
    if t == "int":
        return isinstance(v, int) and not isinstance(v, bool)
    return isinstance(v, _PY[t])


def validate(v: object, sc: dict, path: str = "$") -> list[str]:
    errs: list[str] = []
    ts = sc.get("t", "any")
    ts = [ts] if isinstance(ts, str) else ts
    if not any(_type_ok(v, t) for t in ts):
        return [f"{path}: expected {'/'.join(ts)}, got {type(v).__name__}"]
    if "enum" in sc and v not in sc["enum"]:
        errs.append(f"{path}: {v!r} not in {sc['enum']}")
    if isinstance(v, str):
        if "re" in sc and not re.match(sc["re"], v):
            errs.append(f"{path}: {v!r} does not match {sc['re']}")
        if "min" in sc and len(v) < sc["min"]:
            errs.append(f"{path}: shorter than {sc['min']}")
    elif isinstance(v, bool):
        pass
    elif isinstance(v, int):
        if "min" in sc and v < sc["min"]:
            errs.append(f"{path}: {v} < {sc['min']}")
        if "max" in sc and v > sc["max"]:
            errs.append(f"{path}: {v} > {sc['max']}")
    elif isinstance(v, list):
        if "min" in sc and len(v) < sc["min"]:
            errs.append(f"{path}: fewer than {sc['min']} items")
        for i, it in enumerate(v):
            errs += validate(it, sc.get("of", {}), f"{path}[{i}]")
    elif isinstance(v, dict):
        props = sc.get("p")
        for r in sc.get("req", []):
            if r not in v:
                errs.append(f"{path}: missing '{r}'")
        if props is not None:
            for k, x in v.items():
                if k in props:
                    errs += validate(x, props[k], f"{path}.{k}")
                elif not sc.get("extra", False):
                    errs.append(f"{path}: unknown property '{k}'")
        elif "of" in sc:
            for k, x in v.items():
                errs += validate(x, sc["of"], f"{path}.{k}")
    return errs


# ---- schemas of the four documents ---------------------------------------------------------------------------------
STR, INT, BOOL, ANY = {"t": "str"}, {"t": "int"}, {"t": "bool"}, {"t": "any"}
NSTR = {"t": ["str", "null"]}
STRS = {"t": "arr", "of": STR}
NAME = {"t": "str", "re": r"^[a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)+$"}
SEMVER = {"t": "str", "re": r"^\d+\.\d+$"}
ARG_TYPE = r"^(int|bool|string|player|team|unit_ref|city_ref|plot|league_id|(bitset|enum):\w+|list:[\w:]+)$"

ARG = {"t": "obj", "req": ["name", "type"], "p": {
    "name": {"t": "str", "re": r"^[a-z][a-z0-9_]*$"}, "type": {"t": "str", "re": ARG_TYPE}, "own": BOOL, "wire_name": STR,
    "none": BOOL, "optional": BOOL, "pkind": {"t": "str", "enum": ["major", "minor"]}, "min": INT, "max": INT,
    "max_len": INT, "reserved_below": INT, "reserved_above": INT, "typed_by": ANY, "note": STR}}
GAP = {"t": "obj", "req": ["id", "fix", "flag", "dll_status", "gap", "dll_scope", "dll_note"], "p": {
    "id": {"t": "str", "re": r"^(FIX|AUD)-\d{4}$|^AUD-\d{2}$"}, "fix": NSTR, "flag": NSTR, "dll_status": STR, "gap": STR,
    "dll_scope": NSTR, "dll_note": NSTR}}
WIRE = {"t": "obj", "req": ["send", "response", "lua", "iface", "net_msg", "src"], "p": {
    "send": {"t": ["str", "arr", "null"], "of": STR}, "response": {"t": ["str", "arr", "null"], "of": STR},
    "lua": {"t": ["str", "arr", "null"], "of": STR}, "iface": NSTR, "net_msg": {"t": ["int", "null"]}, "src": NSTR,
    "actor": NSTR, "mux": {"t": ["obj", "null"], "of": ANY}, "derived": {"t": ["obj", "null"], "of": ANY}, "exe": NSTR}}
COMMAND = {"t": "obj", "req": ["name", "since", "class", "wire", "args", "validate", "dll_gaps", "turn_phase", "rate"], "p": {
    "name": NAME, "since": SEMVER, "class": {"t": "str", "enum": list(CLIENT_CLASSES + FORBIDDEN_CLASSES + ("helper",))},
    "wire": WIRE, "args": {"t": "arr", "of": ARG}, "validate": {"t": "arr", "of": {"t": "str", "re": RULE_RE}},
    "dll_gaps": {"t": "arr", "of": GAP}, "turn_phase": {"t": "arr", "of": {"t": "str", "re": PHASE_RE}}, "rate": STR,
    "expands_to": STRS, "tags": STRS, "note": STR}}
S_COMMANDS = {"t": "obj", "req": ["schema", "api", "dll", "classes", "steps", "rates", "reserved_values", "commands"], "p": {
    "schema": INT, "api": SEMVER, "dll": STR, "classes": {"t": "obj", "of": STR}, "steps": STRS,
    "rates": {"t": "obj", "of": STR},
    "reserved_values": {"t": "arr", "of": {"t": "obj", "req": ["wire", "field", "condition", "raw_command", "commands"], "p": {
        "wire": STR, "field": STR, "condition": STR, "raw_command": STR, "commands": STRS}}},
    "commands": {"t": "arr", "of": COMMAND}}}

FIELD = {"t": "obj", "req": ["name", "type", "bin", "source", "visible"], "p": {
    "name": STR, "type": STR, "bin": STR, "source": STR, "visible": STR, "note": STR}}
PENDING = {"t": "obj", "req": ["type", "kind", "source", "non_active", "args", "open"], "p": {
    "type": {"t": "str", "re": r"^ENDTURN_BLOCKING_[A-Z_]+$"}, "kind": {"t": "str", "enum": ["notification", "unit_check", "unused"]},
    "source": STR, "non_active": STR, "args": STR, "open": NSTR}}
S_OBSERVATION = {"t": "obj", "req": ["schema", "api", "views", "parts", "pending", "delta"], "p": {
    "schema": INT, "api": SEMVER, "views": {"t": "obj", "of": STR}, "delta": {"t": "obj", "of": STR},
    "parts": {"t": "arr", "of": {"t": "obj", "req": ["name", "view", "source", "fields"], "p": {
        "name": STR, "view": STR, "source": STR, "fields": {"t": "arr", "of": FIELD}, "note": STR}}},
    "pending": {"t": "arr", "of": PENDING}}}
S_EVENTS = {"t": "obj", "req": ["schema", "api", "envelope", "sources", "events"], "p": {
    "schema": INT, "api": SEMVER, "envelope": {"t": "obj", "of": STR}, "sources": {"t": "obj", "of": STR},
    "events": {"t": "arr", "of": {"t": "obj", "req": ["kind", "source", "subject", "data", "visible_if"], "p": {
        "kind": {"t": "str", "re": r"^[a-z_]+(\.[a-z_]+)*$"}, "source": STR, "subject": STR, "data": STRS, "visible_if": STR}}}}}
S_ERRORS = {"t": "obj", "req": ["schema", "api", "format", "codes"], "p": {
    "schema": INT, "api": SEMVER, "format": {"t": "obj", "of": STR},
    "codes": {"t": "arr", "of": {"t": "obj", "req": ["code", "step", "fatal", "reaches_dll", "meaning"], "p": {
        "code": {"t": "str", "re": r"^[a-z_]+$"}, "step": STR, "fatal": BOOL, "reaches_dll": BOOL, "meaning": STR}}}}}
SCHEMAS = {"commands": S_COMMANDS, "observation": S_OBSERVATION, "events": S_EVENTS, "errors": S_ERRORS}


# ---- semantic checks of commands.json ------------------------------------------------------------------------------
_GAP_TAG = re.compile(r"\[gap:([A-Z]+-\d+)\]")


def by_name(doc: dict) -> dict[str, dict]:
    return {c["name"]: c for c in doc["commands"]}


def check_commands(doc: dict, fixes: list[dict] | None = None) -> list[str]:
    """Safety rules that a plain schema cannot say."""
    errs: list[str] = []
    cmds = doc["commands"]
    names = [c["name"] for c in cmds]
    if names != sorted(names):
        errs.append("commands are not sorted by name")
    if len(set(names)) != len(names):
        errs.append("duplicate command names")
    index = by_name(doc)
    selectors: dict[str, str] = {}
    for c in cmds:
        n, cls, w = c["name"], c["class"], c["wire"]
        rules = c["validate"]
        steps = {r.split("/", 1)[0] for r in rules}
        if cls not in doc["classes"]:
            errs.append(f"{n}: class '{cls}' missing from classes")
        if c["rate"] not in doc["rates"]:
            errs.append(f"{n}: unknown rate '{c['rate']}'")
        argnames = [a["name"] for a in c["args"]]
        if len(set(argnames)) != len(argnames):
            errs.append(f"{n}: duplicate argument names")
        for a in c["args"]:
            tb = a.get("typed_by")
            if tb and tb["arg"] not in argnames:
                errs.append(f"{n}: typed_by refers to unknown argument '{tb['arg']}'")
        if cls in CLIENT_CLASSES:
            for need in ("ownership", "phase", "applicability"):
                if need not in steps:
                    errs.append(f"{n}: no '{need}' rule (write 'not required: <reason>' if it does not apply)")
            if "range" not in steps or "class" not in steps or "rate" not in steps:
                errs.append(f"{n}: range/class/rate rules missing")
            if w["response"] is None and not w.get("exe"):
                errs.append(f"{n}: no Response* and no 'exe' note")
            if not c["turn_phase"]:
                errs.append(f"{n}: empty turn_phase")
        elif cls in FORBIDDEN_CLASSES:
            if "class" not in steps:
                errs.append(f"{n}: forbidden class without a class rule")
            if c["args"]:
                errs.append(f"{n}: forbidden class lists client arguments")
        elif cls == "helper":
            if not c.get("expands_to"):
                errs.append(f"{n}: helper without expands_to")
            for t in c.get("expands_to", []):
                if t not in index or index[t]["class"] == "helper":
                    errs.append(f"{n}: expands_to '{t}' is not a wire command")
        if cls != "helper" and not (w["send"] or w["response"] or w["lua"]):
            errs.append(f"{n}: no wire name")
        # gaps <-> [gap:ID] tags
        tagged = {m for r in rules for m in _GAP_TAG.findall(r)}
        gaps = {g["id"] for g in c["dll_gaps"]}
        if tagged != gaps:
            errs.append(f"{n}: gap ids {sorted(gaps)} differ from rule tags {sorted(tagged)}")
        sel = repr((w["send"], w["response"], w["lua"], sorted((w.get("mux") or {}).items(), key=str)))
        if cls != "helper" and sel in selectors:
            errs.append(f"{n}: same wire selector as {selectors[sel]}")
        selectors[sel] = n
    for rv in doc["reserved_values"]:
        for n in [rv["raw_command"]] + rv["commands"]:
            if n not in index:
                errs.append(f"reserved_values: unknown command '{n}'")
        raw = index.get(rv["raw_command"])
        if raw and not any(k in a for a in raw["args"] for k in ("reserved_below", "reserved_above")) \
                and not any("reserved_value" in r for r in raw["validate"]):
            errs.append(f"reserved_values: {rv['raw_command']} does not reject the reserved range")
    if fixes is not None:
        errs += check_fix_alignment(doc, fixes)
    return errs


# validation-class records that fence a Lua getter, not a Response* handler: there is no command to put them on
NO_COMMAND_FIXES = {"FIX-0113": "Lua Game.GetFounder (read-only getter, CvLuaGame.cpp), not a net command"}


def check_fix_alignment(doc: dict, fixes: list[dict]) -> list[str]:
    """Every FIX-VALIDATION record sits on a command with the same flag/status; nothing refers to a missing record."""
    errs: list[str] = []
    rec = {r["id"]: r for r in fixes if r.get("kind") == "validation" and r["id"] not in NO_COMMAND_FIXES}
    seen: set[str] = set()
    for c in doc["commands"]:
        for g in c["dll_gaps"]:
            if g["id"].startswith("FIX-"):
                seen.add(g["id"])
                r = rec.get(g["id"])
                if r is None:
                    errs.append(f"{c['name']}: gap {g['id']} has no validation record in fixes.json")
                elif (g["flag"], g["dll_status"]) != (r["flag"], r["status"]):
                    errs.append(f"{c['name']}: gap {g['id']} says {g['flag']}/{g['dll_status']}, fixes.json says "
                                f"{r['flag']}/{r['status']} (run 'generate')")
    for rid in sorted(set(rec) - seen):
        errs.append(f"fixes.json {rid} is on no command")
    return errs


# ---- validation of one command call (scenario line {turn, player, cmd, args}) --------------------------------------
def _is_int(v: object) -> bool:
    return isinstance(v, int) and not isinstance(v, bool)


def _check_arg(a: dict, v: object, session_player: int | None) -> tuple[str, str] | None:
    t, n = a["type"], a["name"]
    if v is None:
        return None if (a.get("none") or a.get("optional")) else ("bad_args", f"{n}: null not allowed")
    if t == "bool":
        return None if isinstance(v, bool) else ("bad_args", f"{n}: expected bool")
    if t == "string":
        if not isinstance(v, str):
            return ("bad_args", f"{n}: expected string")
        if any(ord(ch) < 32 for ch in v) or len(v.encode("utf-8")) > a.get("max_len", 127):
            return ("string_invalid", f"{n}: control character or longer than {a.get('max_len', 127)} bytes")
        return None
    if t in ("int", "league_id", "player", "team"):
        if not _is_int(v):
            return ("bad_args", f"{n}: expected integer")
        lo, hi = {"player": (0, MAX_PLAYERS - 1), "team": (0, MAX_TEAMS - 1), "league_id": (0, 1 << 30)}.get(t, (None, None))
        if a.get("pkind") == "major":
            lo, hi = 0, MAX_MAJOR_CIVS - 1
        elif a.get("pkind") == "minor":
            lo, hi = MAX_MAJOR_CIVS, MAX_CIV_PLAYERS - 1
        return _check_int(a, v, lo, hi)
    if t in ("unit_ref", "city_ref"):
        ref = v if isinstance(v, dict) else {"owner": session_player, "id": v}
        if not _is_int(ref.get("id")) or not (ref.get("owner") is None or _is_int(ref.get("owner"))):
            return ("bad_args", f"{n}: expected id or {{owner, id}}")
        if a.get("own") and session_player is not None and ref.get("owner") not in (None, session_player):
            return ("not_owner", f"{n}: owner {ref.get('owner')} is not the session player")
        return _check_int(a, ref["id"], 0, None)
    if t == "plot":
        if not isinstance(v, dict) or not (_is_int(v.get("x")) and _is_int(v.get("y"))):
            return ("bad_args", f"{n}: expected {{x, y}}")
        if (v["x"], v["y"]) == (-1, -1) and a.get("none"):
            return None
        return None if v["x"] >= 0 and v["y"] >= 0 else ("out_of_range", f"{n}: negative plot coordinate")
    if t.startswith("enum:"):
        return None if isinstance(v, str) and re.match(r"^[A-Z][A-Z0-9_]*$", v) else ("bad_args", f"{n}: expected a Type string")
    if t.startswith("bitset:") or t.startswith("list:"):
        return None if isinstance(v, list) else ("bad_args", f"{n}: expected a list")
    return ("bad_args", f"{n}: unknown argument type {t}")


def _check_int(a: dict, v: int, lo: int | None, hi: int | None) -> tuple[str, str] | None:
    n = a["name"]
    if "reserved_below" in a and v < a["reserved_below"]:
        return ("reserved_value", f"{n}: {v} is a reserved multiplexer value (use the dedicated command)")
    if "reserved_above" in a and v > a["reserved_above"]:
        return ("reserved_value", f"{n}: {v} is a reserved multiplexer value (use the dedicated command)")
    lo = a.get("min", lo)
    hi = a.get("max", hi)
    if (lo is not None and v < lo) or (hi is not None and v > hi):
        return ("out_of_range", f"{n}: {v} outside [{lo}, {hi}]")
    return None


def check_call(doc: dict, name: str, args: dict, session_player: int | None = None) -> list[tuple[str, str]]:
    """Static part of the validation order (parse, class, ranges, ownership shape). Returns [(code, detail)], [] = ok."""
    cmd = by_name(doc).get(name)
    if cmd is None:
        return [("unknown_command", name)]
    if cmd["class"] not in CLIENT_CLASSES:
        return [("forbidden", f"{name}: class {cmd['class']}")]
    if not isinstance(args, dict):
        return [("bad_args", "args must be an object")]
    known = {a["name"]: a for a in cmd["args"]}
    errs: list[tuple[str, str]] = [("bad_args", f"unknown argument '{k}'") for k in args if k not in known]
    for a in cmd["args"]:
        if a["name"] not in args:
            if not (a.get("optional") or a.get("none")):
                errs.append(("bad_args", f"missing argument '{a['name']}'"))
            continue
        e = _check_arg(a, args[a["name"]], session_player)
        if e:
            errs.append(e)
    return errs
