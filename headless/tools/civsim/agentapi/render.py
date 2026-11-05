"""Command reference (headless/generated-docs/AGENT-COMMANDS.md), generated from headless/agent-api/commands.json (do not edit by hand)."""
from __future__ import annotations

from pathlib import Path

from . import canon

OUT = canon.DOCS_DIR / "AGENT-COMMANDS.md"


def _wire(w: dict) -> str:
    parts = []
    for key, label in (("send", "send"), ("response", "resp"), ("lua", "lua")):
        v = w[key]
        if v:
            parts.append(f"{label}: " + ", ".join(v if isinstance(v, list) else [v]))
    if w.get("mux"):
        parts.append("mux: " + ", ".join(f"{k}={v}" for k, v in w["mux"].items()))
    if w.get("exe"):
        parts.append("exe")
    return "<br>".join(parts) or "-"


def _arg(a: dict) -> str:
    s = f"{a['name']}:{a['type']}"
    if a.get("own"):
        s += " (own)"
    if a.get("none") or a.get("optional"):
        s += "?"
    return s


def _src(rule: str) -> str:
    return rule.split(":", 1)[0].split("/")[1]


def _esc(s: str) -> str:
    return s.replace("|", "\\|")


def render(doc: dict, dhash: str) -> str:
    cmds = doc["commands"]
    L = ["# Agent API: command dictionary", "",
         "Generated from `headless/agent-api/commands.json` by `python -m civsim.agentapi render`; do not edit.", "",
         f"- api: {doc['api']}  DLL: {doc['dll']}  commands: {len(cmds)}",
         f"- dict_hash (SHA-256 of the canonical commands.json): `{dhash}`",
         "- The acting player always comes from the session, never from the payload (`wire.actor` is filled by the session).",
         "- `commands.json` is written by `python -m civsim.agentapi generate` from the DLL headers, `civsim/agentapi/overlay.py` (hand rules) and "
         "`headless/improvements/fixes.json`: edit the overlay, not the json. Source line numbers are those of the working tree at generation time; `coverage` warns when `wire.src` drifts.",
         "- Rules are `step/src: text`. `auto` = mechanically derived from class and argument types; `hand` = written after reading the handler. "
         "`[gap:ID]` marks the rule that closes a DLL validation gap. Rejected commands never reach the DLL or `commands.bin`.", ""]
    L += ["## Classes", "", "| class | commands | meaning |", "|---|---|---|"]
    for k, v in doc["classes"].items():
        L.append(f"| {k} | {sum(1 for c in cmds if c['class'] == k)} | {_esc(v)} |")
    L += ["", "## Validation order", ""] + [f"- {v}" for v in doc["steps"]]
    L += ["", "## Reserved raw values (Lekmod multiplexers)", "",
          "| wire | field | raw condition | raw command rejects with `reserved_value` | dedicated commands |", "|---|---|---|---|---|"]
    for r in doc["reserved_values"]:
        L.append(f"| {r['wire']} | {r['field']} | {r['condition']} | {r['raw_command']} | {', '.join(r['commands'])} |")

    groups: dict[str, list[dict]] = {}
    for c in cmds:
        groups.setdefault(c["name"].split(".")[0], []).append(c)
    L += ["", "## Commands"]
    for g, cs in sorted(groups.items()):
        L += ["", f"### {g}", "", "| command | class | wire | args | phase | rate | rules |", "|---|---|---|---|---|---|---|"]
        for c in cs:
            auto = sum(1 for r in c["validate"] if _src(r) == "auto")
            hand = len(c["validate"]) - auto
            flags = ("bot " if "bot" in c.get("tags", []) else "") + ("gap " if c["dll_gaps"] else "")
            L.append(f"| **{c['name']}** | {c['class']} | {_wire(c['wire'])} | {_esc(', '.join(_arg(a) for a in c['args']) or '-')} | "
                     f"{_esc(', '.join(c['turn_phase']) or '-')} | {c['rate']} | {auto} auto, {hand} hand {flags}|")
        notes = [c for c in cs if c.get("note") or any(_src(r) == "hand" for r in c["validate"]) or c.get("expands_to")]
        if notes:
            L += ["", "Hand-written rules and notes:", ""]
            for c in notes:
                L.append(f"- **{c['name']}**" + (f" (expands to {', '.join(c['expands_to'])})" if c.get("expands_to") else "")
                         + (f": {c['note']}" if c.get("note") else ""))
                for r in c["validate"]:
                    if _src(r) == "hand":
                        L.append(f"  - {r}")

    L += ["", "## DLL validation gaps", "",
          "`FIX-*` rows are FIX-VALIDATION records (flag, status from `headless/improvements/fixes.json`); `AUD-*` rows were found by the V1 audit of the other "
          "handlers and are closed by the host validator only. Honest commands are bit-for-bit unchanged either way.", "",
          "| id | flag | DLL status | commands | gap | DLL rejects | not done in the DLL, and why |", "|---|---|---|---|---|---|---|"]
    gaps: dict[str, dict] = {}
    for c in cmds:
        for g in c["dll_gaps"]:
            e = gaps.setdefault(g["id"], {**g, "cmds": []})
            e["cmds"].append(c["name"])
    for gid in sorted(gaps):
        g = gaps[gid]
        L.append(f"| {gid} | {g['flag'] or '-'} | {g['dll_status']} | {', '.join(g['cmds'])} | {_esc(g['gap'])} | {_esc(g['dll_scope'] or '-')} | {_esc(g['dll_note'] or '-')} |")

    L += ["", "## Bot-player commands", "", "| decision | command | wire |", "|---|---|---|"]
    for c in cmds:
        if "bot" in c.get("tags", []):
            L.append(f"| {c['name']} | {c['class']} | {_wire(c['wire'])} |")
    L.append("")
    return "\n".join(L) + "\n"


def write(doc: dict, dhash: str, check_only: bool = False, path: Path | None = None) -> bool:
    """True when the file was (or, with check_only, would be) changed."""
    p = path or OUT
    data = render(doc, dhash).encode("utf-8")
    if p.exists() and p.read_bytes() == data:
        return False
    if not check_only:
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
    return True
