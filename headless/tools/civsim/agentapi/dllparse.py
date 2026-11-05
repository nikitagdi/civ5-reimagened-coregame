"""Read-only parsers for the wire-level vocabulary: ICvEngineUtility send*, ICvNetMessageHandler Response*, Lua Network.Send*.

Nothing here executes or links anything; it only reads headers and scripts.
"""
from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

UTILITY_H = "CvGameCoreDLLUtil/include/ICvDLLUtility.h"
HANDLER_HS = ("CvGameCoreDLLUtil/include/CvDllInterfaces.h", "CvGameCoreDLLUtil/include/CvDllInterfaces2.h")
HANDLER_CPP = "CvDllNetMessageHandler.cpp"


@dataclass
class Decl:
    name: str
    iface: str                       # ICvEngineUtility1 / ICvNetMessageHandler2 ...
    params: list[tuple[str, str]]    # (c++ type, name)
    const: bool
    file: str
    line: int


def read(path: Path) -> str:
    return path.read_text(encoding="latin-1")


def blank_comments(s: str) -> str:
    """Replace comments by blanks, keeping every newline so line numbers stay valid."""
    def blank(m: re.Match) -> str:
        return re.sub(r"[^\n]", " ", m.group(0))
    s = re.sub(r"/\*.*?\*/", blank, s, flags=re.S)
    return re.sub(r"//[^\n]*", blank, s)


def _split_top(s: str) -> list[str]:
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in "<([":
            depth += 1
        elif ch in ">)]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur)
    return out


_SAL = re.compile(r"\b_(?:In|Out|Inout|Ret|Deref)[A-Za-z0-9_]*_(?:\s*\([^()]*\))?")


def parse_params(s: str) -> list[tuple[str, str]]:
    res: list[tuple[str, str]] = []
    for idx, raw in enumerate(_split_top(s)):
        p = " ".join(_SAL.sub(" ", raw.split("=")[0] if "=" in raw else raw).split())
        if p in ("", "void"):
            continue
        m = re.match(r"^(.*?[\s*&>])(\w+)$", p)
        words = [w for w in re.findall(r"\w+", p) if w not in ("const", "unsigned", "signed", "volatile")]
        if p.endswith(("*", "&", ">")) or not m or len(words) < 2:
            res.append((p, f"p{idx}"))          # unnamed parameter
        else:
            res.append((m.group(1).strip(), m.group(2)))
    return res


def _class_spans(text: str, pattern: str) -> list[tuple[int, str]]:
    return [(m.start(), m.group(1)) for m in re.finditer(pattern, text)]


def _decls(path: Path, name_re: str, class_re: str, rel: str) -> list[Decl]:
    raw = read(path)
    s = blank_comments(raw)
    spans = _class_spans(s, class_re)
    out: list[Decl] = []
    rx = re.compile(r"\bvirtual\b[^;{}]*?\b(" + name_re + r")\s*\(([^;{}]*?)\)\s*(const)?\s*=\s*0")
    for m in rx.finditer(s):
        cls = [n for p, n in spans if p < m.start()]
        out.append(Decl(m.group(1), cls[-1] if cls else "?", parse_params(m.group(2)), bool(m.group(3)),
                        rel, s.count("\n", 0, m.start(1)) + 1))
    return out


def parse_sends(dll_src: Path) -> list[Decl]:
    return _decls(dll_src / UTILITY_H, r"[sS]end[A-Z]\w*", r"\bclass\s+(ICvEngineUtility\d)\s*:", "ICvDLLUtility.h")


def parse_responses(dll_src: Path) -> list[Decl]:
    out: list[Decl] = []
    for rel in HANDLER_HS:
        out += _decls(dll_src / rel, r"Response\w+", r"\bclass\s+(ICvNetMessageHandler\d)\s*:", Path(rel).name)
    return out


def handler_lines(dll_src: Path) -> dict[str, int]:
    """Line of the implementation in CvDllNetMessageHandler.cpp (the longest overload when overloaded)."""
    s = blank_comments(read(dll_src / HANDLER_CPP))
    found: dict[str, list[tuple[int, int]]] = {}
    for m in re.finditer(r"^void\s+CvDllNetMessageHandler::(Response\w+)\s*\(([^)]*)\)", s, flags=re.M):
        found.setdefault(m.group(1), []).append((len(m.group(2)), s.count("\n", 0, m.start()) + 1))
    return {n: max(v, key=lambda t: (t[0], -t[1]))[1] for n, v in found.items()}


def sync_lines(decls: list[Decl]) -> dict[str, str]:
    """name -> 'File.h:line' (first declaration)."""
    out: dict[str, str] = {}
    for d in decls:
        out.setdefault(d.name, f"{d.file}:{d.line}")
    return out


# ---- Lua ---------------------------------------------------------------------------------------------------------
_LUA_LONG = re.compile(r"--\[(=*)\[.*?\]\1\]", flags=re.S)
_LUA_CALL = re.compile(r"\bNetwork\.(Send\w+)")


def scan_lua(roots: list[Path]) -> dict[str, dict]:
    """Network.Send* calls outside comments: name -> {count, example 'path:line'}."""
    found: dict[str, dict] = {}
    for root in roots:
        for f in sorted(root.rglob("*.lua")):
            try:
                text = f.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            if "Network." not in text:
                continue
            text = _LUA_LONG.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), text)
            for ln, line in enumerate(text.split("\n"), 1):
                code = line.split("--", 1)[0]
                for m in _LUA_CALL.finditer(code):
                    e = found.setdefault(m.group(1), {"count": 0, "example": f"{f.name}:{ln}"})
                    e["count"] += 1
    return found
