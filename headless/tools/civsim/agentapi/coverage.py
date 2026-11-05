"""Coverage: every wire-level name in the sources is classified by a dictionary entry; an unknown name fails (exit 2).

Wire names: send*/Send* of ICvEngineUtility1-4, Response* of ICvNetMessageHandler1-3, Lua Network.Send* (outside comments).
"""
from __future__ import annotations

import os
import re
from dataclasses import dataclass, field
from pathlib import Path

from . import canon, dllparse, gen

DEFAULT_GAME = r"C:\Program Files (x86)\Steam\steamapps\common\Sid Meier's Civilization V"   # override with CIV5_GAME_DIR


def default_lua_roots() -> list[Path]:
    game = Path(os.environ.get("CIV5_GAME_DIR", DEFAULT_GAME)) / "Assets"
    roots = [game, canon.REPO / "LEKMOD" / "Lua"]
    return [r for r in roots if r.is_dir()]


@dataclass
class Wires:
    send: dict[str, str] = field(default_factory=dict)       # name -> 'File.h:line'
    response: dict[str, str] = field(default_factory=dict)
    lua: dict[str, str] = field(default_factory=dict)        # name -> 'example (count calls)'
    send_decls: int = 0
    response_decls: int = 0
    lua_scanned: bool = False


def collect(dll_src: Path, lua_roots: list[Path]) -> Wires:
    sends, resps = dllparse.parse_sends(dll_src), dllparse.parse_responses(dll_src)
    w = Wires(send=dllparse.sync_lines(sends), response=dllparse.sync_lines(resps),
              send_decls=len(sends), response_decls=len(resps))
    if lua_roots:
        w.lua_scanned = True
        w.lua = {k: f"{v['example']} ({v['count']} calls)" for k, v in dllparse.scan_lua(lua_roots).items()}
    return w


def _names(x: object) -> list[str]:
    return [] if not x else ([x] if isinstance(x, str) else list(x))


@dataclass
class Report:
    unknown: list[tuple[str, str]] = field(default_factory=list)   # (kind, name)
    stale: list[tuple[str, str]] = field(default_factory=list)
    by_kind: dict[str, dict[str, list[str]]] = field(default_factory=dict)  # kind -> name -> [command]
    classes: dict[str, dict[str, int]] = field(default_factory=dict)       # kind -> class -> names
    drift: list[str] = field(default_factory=list)

    @property
    def code(self) -> int:
        return 2 if (self.unknown or [s for s in self.stale if s[0] != "lua"]) else 0


def classify(doc: dict, w: Wires) -> Report:
    r = Report()
    refs: dict[str, dict[str, list[str]]] = {"send": {}, "response": {}, "lua": {}}
    for c in doc["commands"]:
        for kind, key in (("send", "send"), ("response", "response"), ("lua", "lua")):
            for n in _names(c["wire"][key]):
                refs[kind].setdefault(n.removeprefix("Network."), []).append(c["name"])
    cls_of = {c["name"]: c["class"] for c in doc["commands"]}
    src = {"send": w.send, "response": w.response, "lua": w.lua}
    for kind in ("send", "response", "lua"):
        if kind == "lua" and not w.lua_scanned:
            continue
        r.by_kind[kind] = {}
        r.classes[kind] = {}
        for n in sorted(src[kind]):
            cmds = refs[kind].get(n)
            if not cmds:
                r.unknown.append((kind, n))
                continue
            r.by_kind[kind][n] = cmds
            cl = sorted({cls_of[c] for c in cmds})
            key = "/".join(cl)
            r.classes[kind][key] = r.classes[kind].get(key, 0) + 1
        r.stale += [(kind, n) for n in sorted(refs[kind]) if n not in src[kind]]
    return r


def drift(doc: dict, dll_src: Path) -> list[str]:
    """Commands whose wire.src no longer names the current declaration/implementation lines."""
    hl, sl = dllparse.handler_lines(dll_src), dllparse.sync_lines(dllparse.parse_sends(dll_src))
    out = []
    for c in doc["commands"]:
        s = c["wire"]["src"] or ""
        exp = [sl[n] for n in _names(c["wire"]["send"]) if n in sl]
        exp += [f"CvDllNetMessageHandler.cpp:{hl[n]}" for n in _names(c["wire"]["response"]) if n in hl]
        if any(e not in s for e in exp):
            out.append(c["name"])
    return out


def markdown(doc: dict, w: Wires, rep: Report) -> str:
    L = ["# Agent API coverage", "",
         f"- send* declarations: {w.send_decls} ({len(w.send)} names)",
         f"- Response* declarations: {w.response_decls} ({len(w.response)} names)",
         f"- Network.Send* names called outside comments: {len(w.lua)}" if w.lua_scanned else "- Network.Send*: Lua roots not scanned", ""]
    for kind, names in rep.by_kind.items():
        L += [f"## {kind}", "", "| name | commands |", "|---|---|"]
        L += [f"| {n} | {', '.join(c)} |" for n, c in names.items()]
        L.append("")
    if rep.unknown:
        L += ["## unknown (not classified)", ""] + [f"- {k}: {n}" for k, n in rep.unknown] + [""]
    return "\n".join(L) + "\n"


def fix_val_guards(dll_src: Path) -> dict[str, set[str]]:
    """LEKMOD_HEADLESS_FIX_VAL_* flag -> Response* handlers that contain a block fenced by it."""
    s = dllparse.blank_comments(dllparse.read(dll_src / dllparse.HANDLER_CPP))
    funcs = [(m.start(), m.group(1)) for m in re.finditer(r"^void\s+CvDllNetMessageHandler::(Response\w+)\s*\(", s, flags=re.M)]
    out: dict[str, set[str]] = {}
    for m in re.finditer(r"\bLEKMOD_HEADLESS_FIX_VAL_\w+", s):
        owner = [n for p, n in funcs if p < m.start()]
        if owner:
            out.setdefault(m.group(0), set()).add(owner[-1])
    return out


def check_guards(doc: dict, dll_src: Path) -> list[str]:
    """Every handler guarded by a FIX-VALIDATION flag is the Response* of a command whose dll_gaps carry that flag."""
    on_flag: dict[str, set[str]] = {}
    for c in doc["commands"]:
        for g in c["dll_gaps"]:
            if g["flag"]:
                on_flag.setdefault(g["flag"], set()).update(_names(c["wire"]["response"]))
    errs = []
    for flag, handlers in sorted(fix_val_guards(dll_src).items()):
        if flag not in on_flag:
            errs.append(f"{flag} fences code in the handler but is on no command gap")
        elif handlers - on_flag[flag]:
            errs.append(f"{flag} fences {sorted(handlers - on_flag[flag])} but no command with that gap uses them")
    return errs
