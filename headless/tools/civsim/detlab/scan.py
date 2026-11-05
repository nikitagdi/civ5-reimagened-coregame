"""civsim.detlab.scan — static triage scanners for desync candidates.

Heuristic, regex-based, stdlib only. A finding is a lead for a human or agent to read, never proof.

Scanners (rule ids in brackets):
  nonser        C1  class members absent from Write [nonser.member], Read/Write asymmetry [nonser.asym],
                    function-level statics [static.local], mutable globals [static.global], static data members
                    [static.member]
  uninit        C2  POD members not set in ctor / reset / init [uninit.member]
  ptrorder      C3  pointer-keyed associative containers [ptr.container], hashed-container iteration [hash.iter],
                    sort of pointer vectors without comparator [ptr.sort], pointer relational compare [ptr.compare],
                    pointer-to-int casts [ptr.asint]
  activeplayer  C5  sim mutations gated by active player / host [ap.gate], net sends outside player-gated code
                    [net.send], async RNG uses [async.rand]
  lua-local     C5  Lua: mutators gated by GetActivePlayer [lua.activeplayer], mutators in Events/UI handlers
                    [lua.events-mutator], Game.Rand in UI code [lua.rand-ui], math.random near state [lua.math-random],
                    GameEvents handler self-removal [lua.selfremove], ipairs(Players) [lua.ipairs-players],
                    player object compared with a number [lua.obj-eq-num], module-level caches [lua.cache],
                    pairs() over object-keyed tables [lua.pairs-objkey]

The C++ side is preprocessed with the golden define set (_Defines.h, _FruityDefines.h and the vcproj defines), so
inactive #if branches are ignored and brace matching stays sane.
"""
from __future__ import annotations

import bisect
import json
import os
import re
import sys
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parents[4]
DLL_DIR = REPO / "LEKMOD_DLL" / "CvGameCoreDLL_Expansion2"
LUA_DIRS = [REPO / "LEKMOD" / "Lua", REPO / "LEKMOD" / "Art", REPO / "Lekmap"]
GAME_ASSETS = Path(os.environ.get("CIV5_GAME_DIR") or r"C:\Program Files (x86)\Steam\steamapps\common\Sid Meier's Civilization V") / "Assets"   # CIV5_GAME_DIR: game install
GAME_LEKMOD_DLC = GAME_ASSETS / "DLC" / "LEKMOD_v34.15"
DLL_EXCLUDE = {"ThirdPartyLibs", "FirePlace", "CvGameCoreDLLUtil", "CvGameDatabase", "CvLocalization"}
PROJECT_DEFINES = {"FXS_IS_DLL": "1", "WIN32": "1", "NDEBUG": "1", "_WINDOWS": "1", "_USRDLL": "1",
                   "CVGAMECOREDLL_EXPORTS": "1", "FINAL_RELEASE": "1", "_CRT_SECURE_NO_WARNINGS": "1",
                   "_MSC_VER": "1500"}

SCANNERS = ("nonser", "uninit", "ptrorder", "activeplayer", "lua-local")


@dataclass
class Finding:
    rule: str
    cls: str
    file: str
    line: int
    score: int          # 3 = read this first, 2 = plausible, 1 = informational
    msg: str
    snippet: str = ""
    func: str = ""

    def loc(self) -> str:
        return f"{self.file}:{self.line}"


def _rel(p: Path) -> str:
    try:
        return p.resolve().relative_to(REPO).as_posix()
    except ValueError:
        return str(p)


# ----------------------------------------------------------------------------------------------------------------
# C++ preprocessing
# ----------------------------------------------------------------------------------------------------------------
_CPP_TOKENS = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', re.S)


def strip_cpp_comments(src: str) -> str:
    """Blank comments and string/char literal contents, keeping offsets and newlines."""
    def rep(m: re.Match) -> str:
        s = m.group(0)
        if s[0] in "\"'":
            return s[0] + re.sub(r"[^\n]", " ", s[1:-1]) + s[-1]
        return re.sub(r"[^\n]", " ", s)
    return _CPP_TOKENS.sub(rep, src)


def _eval_pp(expr: str, defines: dict[str, str]) -> bool:
    expr = re.sub(r"\bdefined\s*\(\s*(\w+)\s*\)", lambda m: " 1 " if m.group(1) in defines else " 0 ", expr)
    expr = re.sub(r"\bdefined\s+(\w+)", lambda m: " 1 " if m.group(1) in defines else " 0 ", expr)

    def ident(m: re.Match) -> str:
        v = defines.get(m.group(0))
        if v is None:
            return " 0 "
        v = v.strip()
        return f" {int(v, 0)} " if re.fullmatch(r"-?(0x[0-9a-fA-F]+|\d+)", v) else " 1 "
    expr = re.sub(r"\b[A-Za-z_]\w*\b", ident, expr)
    expr = re.sub(r"(\d+)[uUlL]+\b", r"\1", expr)
    expr = expr.replace("&&", " and ").replace("||", " or ")
    expr = re.sub(r"!(?!=)", " not ", expr)
    try:
        return bool(eval(expr, {"__builtins__": {}}, {}))  # noqa: S307 - digits and operators only
    except Exception:
        return False


def preprocess(code: str, defines: dict[str, str]) -> str:
    """Evaluate #if/#ifdef/#else/#endif with `defines` (updated in place by #define/#undef); blank inactive lines."""
    out = []
    stack: list[tuple[bool, bool]] = []    # (active, taken)
    active = True
    lines = code.split("\n")
    i = 0
    while i < len(lines):
        line = lines[i]
        s = line.lstrip()
        if s.startswith("#"):
            full, n = line, 1
            while full.rstrip().endswith("\\") and i + n < len(lines):
                full = full.rstrip()[:-1] + " " + lines[i + n]
                n += 1
            m = re.match(r"\s*#\s*(\w+)\s*(.*)", full)
            d, rest = (m.group(1), m.group(2).strip()) if m else ("", "")
            if d in ("if", "ifdef", "ifndef"):
                if d == "if":
                    cond = _eval_pp(rest, defines)
                else:
                    name = rest.split()[0] if rest else ""
                    cond = (name in defines) == (d == "ifdef")
                stack.append((active, active and cond))
                active = active and cond
            elif d == "elif":
                parent, taken = stack[-1] if stack else (True, True)
                cond = parent and not taken and _eval_pp(rest, defines)
                stack[-1] = (parent, taken or cond)
                active = cond
            elif d == "else":
                parent, taken = stack[-1] if stack else (True, True)
                stack[-1] = (parent, True)
                active = parent and not taken
            elif d == "endif":
                if stack:
                    active = stack.pop()[0]
            elif active and d == "define":
                mm = re.match(r"(\w+)(\([^)]*\))?\s*(.*)", rest)
                if mm:
                    defines[mm.group(1)] = mm.group(3) if not mm.group(2) else "1"
            elif active and d == "undef":
                defines.pop(rest.split()[0] if rest else "", None)
            out.extend([""] * n)
            i += n
            continue
        out.append(line if active else "")
        i += 1
    return "\n".join(out)


_GLOBAL_DEFINES: dict[str, str] | None = None


def golden_defines() -> dict[str, str]:
    global _GLOBAL_DEFINES
    if _GLOBAL_DEFINES is None:
        d = dict(PROJECT_DEFINES)
        for name in ("_Defines.h", "_FruityDefines.h"):
            p = DLL_DIR / name
            if _REV:
                import subprocess
                rel = p.relative_to(REPO).as_posix()
                txt = subprocess.run(["git", "-C", str(REPO), "show", f"{_REV}:{rel}"], capture_output=True, check=True).stdout.decode("latin-1")
                preprocess(strip_cpp_comments(txt), d)
            elif p.exists():
                preprocess(strip_cpp_comments(p.read_text(encoding="latin-1")), d)
        _GLOBAL_DEFINES = d
    return dict(_GLOBAL_DEFINES)


# ----------------------------------------------------------------------------------------------------------------
# C++ structure: functions, classes, members
# ----------------------------------------------------------------------------------------------------------------
@dataclass
class Func:
    qual: str
    cls: str
    name: str
    header: str
    body_start: int
    body_end: int
    line: int
    end_line: int
    src: "SrcFile" = field(repr=False, default=None)  # type: ignore[assignment]

    @property
    def body(self) -> str:
        return self.src.code[self.body_start:self.body_end + 1]


@dataclass
class Member:
    name: str
    type: str
    line: int
    static: bool
    file: str


@dataclass
class ClassDecl:
    name: str
    file: str
    line: int
    members: dict[str, Member] = field(default_factory=dict)


class SrcFile:
    def __init__(self, path: Path, code: str, raw: str):
        self.path = path
        self.rel = _rel(path)
        self.code = code
        self.raw_lines = raw.split("\n")
        self._nl = [m.start() for m in re.finditer("\n", code)]
        self.funcs: list[Func] = []
        self.classes: list[ClassDecl] = []
        self.globals: list[tuple[str, str, int]] = []     # (name, decl, line)

    def line_of(self, off: int) -> int:
        return bisect.bisect_left(self._nl, off) + 1

    def raw(self, line: int) -> str:
        return self.raw_lines[line - 1].strip() if 0 < line <= len(self.raw_lines) else ""

    def func_at(self, off: int) -> Func | None:
        best = None
        for f in self.funcs:
            if f.body_start <= off <= f.body_end and (best is None or f.body_start > best.body_start):
                best = f
        return best


_FUNC_NAME = re.compile(r"(operator\s*(?:\(\)|[^\s(]+)|~?[A-Za-z_]\w*(?:\s*::\s*~?[A-Za-z_]\w*)*)\s*\(")


def _split_top(s: str, sep: str = ",") -> list[str]:
    parts, depth, cur = [], 0, []
    for ch in s:
        if ch in "<([":
            depth += 1
        elif ch in ">)]":
            depth -= 1
        if ch == sep and depth == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    parts.append("".join(cur))
    return parts


def _parse_member(stmt: str, line: int, file: str) -> list[Member]:
    s = stmt.strip()
    while True:
        m = re.match(r"(public|private|protected)\s*:\s*", s)
        if not m:
            break
        s = s[m.end():]
    if not s or re.match(r"(typedef|friend|using|enum|class|struct|union|template|operator)\b", s):
        return []
    static = bool(re.match(r"static\b", s))
    s2 = re.sub(r"^(static|mutable|volatile)\s+", "", s)
    if re.match(r"const\b", s2) and static:
        return []
    if "(" in s2 and not re.match(r"FAutoVariable\b", s2):
        # function declaration (or function pointer member, ignored)
        return []
    decls = _split_top(s2)
    first = decls[0]
    m = re.search(r"([A-Za-z_]\w*)\s*((?:\[[^\]]*\]\s*)*)(=.*)?$", first.strip())
    if not m:
        return []
    typ = first[:m.start()].strip() if m.start() else first.strip()
    out = [Member(m.group(1), typ + (" []" if m.group(2) else ""), line, static, file)]
    for d in decls[1:]:
        mm = re.search(r"([A-Za-z_]\w*)\s*((?:\[[^\]]*\]\s*)*)(=.*)?$", d.strip())
        if mm:
            ptr = "*" if d.strip().startswith("*") else ""
            out.append(Member(mm.group(1), typ + ptr + (" []" if mm.group(2) else ""), line, static, file))
    return out


def parse_structure(sf: SrcFile) -> None:
    code = sf.code
    stack: list[tuple[str, int, object]] = []   # (kind, open_offset, payload)
    boundary = 0
    for m in re.finditer(r"[{};]", code):
        ch, pos = m.group(0), m.start()
        in_func = any(k in ("func", "block", "init") for k, _, _ in stack)
        if ch == "{":
            if in_func:
                stack.append(("block", pos, None))
            else:
                header = code[boundary:pos]
                hs = header.strip()
                kind, payload = "other", None
                if re.search(r"\bnamespace\b[^;{}]*$", hs) or re.search(r'\bextern\s*"', hs):
                    kind = "ns"
                elif hs.endswith("="):
                    kind = "init"
                else:
                    cm = re.search(r"\b(class|struct|union|enum)\b\s*(?:[A-Z_][A-Z0-9_]*\s+)*([A-Za-z_]\w*)?\s*(?::[^(){};]*)?$", hs)
                    if cm and "(" not in hs[cm.start():]:
                        kind = "class"
                        payload = ClassDecl(cm.group(2) or "", sf.rel, sf.line_of(pos))
                        if cm.group(1) != "enum" and cm.group(2):
                            sf.classes.append(payload)
                    elif "(" in hs:
                        fm = _FUNC_NAME.search(hs)
                        if fm:
                            qual = re.sub(r"\s+", "", fm.group(1))
                            enclosing = [p.name for k, _, p in stack if k == "class" and isinstance(p, ClassDecl) and p.name]
                            if "::" not in qual and enclosing:
                                qual = enclosing[-1] + "::" + qual
                            cls, _, name = qual.rpartition("::")
                            kind = "func"
                            payload = Func(qual, cls, name, hs, pos, -1, sf.line_of(boundary + len(header) - len(header.lstrip())), -1, sf)
                stack.append((kind, pos, payload))
            boundary = pos + 1
        elif ch == "}":
            if stack:
                kind, start, payload = stack.pop()
                if kind == "func" and isinstance(payload, Func):
                    payload.body_end = pos
                    payload.end_line = sf.line_of(pos)
                    sf.funcs.append(payload)
            boundary = pos + 1
        else:  # ';'
            if not in_func:
                stmt = code[boundary:pos]
                top = stack[-1] if stack else None
                if top and top[0] == "class" and isinstance(top[2], ClassDecl):
                    off = boundary + len(stmt) - len(stmt.lstrip())
                    for mem in _parse_member(stmt, sf.line_of(off), sf.rel):
                        top[2].members[mem.name] = mem
                elif top is None or top[0] == "ns":
                    st = stmt.strip()
                    if st and "(" not in st.split("=")[0] and not re.match(
                            r"(typedef|extern|using|template|class|struct|enum|union|friend|namespace|const\b|static\s+const\b)", st) \
                            and re.search(r"[A-Za-z_]\w*\s*(\[[^\]]*\])?\s*(=.*)?$", st):
                        nm = re.search(r"([A-Za-z_]\w*)\s*(\[[^\]]*\])?\s*(=.*)?$", st.split("=")[0].strip() if "=" in st else st)
                        if nm and not re.search(r"\bconst\b", st.split("=")[0]):
                            off = boundary + len(stmt) - len(stmt.lstrip())
                            sf.globals.append((nm.group(1), " ".join(st.split()), sf.line_of(off)))
            boundary = pos + 1


def git_tree_texts(rev: str, root: Path, exts: tuple[str, ...]) -> dict[Path, str]:
    """{path: text} of every file under `root` with one of `exts` at git revision `rev` (one `git cat-file` process)."""
    import subprocess
    rel = root.resolve().relative_to(REPO).as_posix()
    names = subprocess.run(["git", "-C", str(REPO), "ls-tree", "-r", "--name-only", rev, "--", rel],
                           capture_output=True, text=True, check=True).stdout.split("\n")
    names = [n for n in names if n and n.lower().endswith(exts)]
    if not names:
        return {}
    proc = subprocess.run(["git", "-C", str(REPO), "cat-file", "--batch"], input="\n".join(f"{rev}:{n}" for n in names).encode() + b"\n",
                          capture_output=True, check=True)
    out, data, pos = {}, proc.stdout, 0
    for n in names:
        nl = data.index(b"\n", pos)
        header = data[pos:nl].split()
        size = int(header[2]) if len(header) >= 3 and header[1] != b"missing" else 0
        out[REPO / n] = data[nl + 1:nl + 1 + size].decode("latin-1")
        pos = nl + 1 + size + 1
    return out


_REV: str | None = None     # set by --rev: scan a git revision instead of the working tree (stable line numbers)


def _read_sources(root: Path, exts: tuple[str, ...]) -> list[tuple[Path, str]]:
    if _REV:
        return sorted(git_tree_texts(_REV, root, exts).items())
    return [(p, p.read_text(encoding="latin-1")) for p in sorted(root.rglob("*")) if p.suffix.lower() in exts and p.is_file()]


class CppIndex:
    def __init__(self, root: Path = DLL_DIR):
        t0 = time.time()
        base = golden_defines()
        self.files: list[SrcFile] = []
        for p, raw in _read_sources(root, (".cpp", ".h", ".inl")):
            if any(x in DLL_EXCLUDE for x in p.relative_to(root).parts):
                continue
            code = preprocess(strip_cpp_comments(raw), dict(base))
            sf = SrcFile(p, code, raw)
            parse_structure(sf)
            self.files.append(sf)
        self.classes: dict[str, ClassDecl] = {}
        for sf in self.files:
            for c in sf.classes:
                if c.name and (c.name not in self.classes or len(c.members) > len(self.classes[c.name].members)):
                    self.classes[c.name] = c
        self.funcs_by_cls: dict[str, list[Func]] = {}
        for sf in self.files:
            for f in sf.funcs:
                self.funcs_by_cls.setdefault(f.cls, []).append(f)
        self.elapsed = time.time() - t0

    def cpp_files(self):
        return [f for f in self.files if f.path.suffix.lower() == ".cpp"]


# ----------------------------------------------------------------------------------------------------------------
# helpers shared by C++ rules
# ----------------------------------------------------------------------------------------------------------------
SER_NAMES = {"Read", "Write", "read", "write", "Serialize", "ReadSupportingClassData", "WriteSupportingClassData"}
INIT_NAMES = {"reset", "Reset", "init", "Init", "Initialize", "uninit", "Uninit", "Uninitialize", "Init2", "InitMap",
              "reinit", "ResetData", "InitData", "Clear", "clear", "Shutdown", "PostInit", "LaterInit", "initFreeState",
              "initFreeUnits", "Reinit", "ResetValues", "Setup", "CacheResults", "cacheGlobals", "fracInit",
              "fracInitInternal", "fracInitHinted", "fracInitRifts"}
POD_TYPE = re.compile(r"^(?:const\s+)?(?:unsigned\s+|signed\s+)?(bool|int|uint|short|char|long|float|double|BYTE|WORD|DWORD|"
                      r"UINT|ULONG|size_t|__int64|[A-Z]\w*Types|PlayerTypes|TeamTypes|LeaderHeadTypes|\w+Type)\b")


def _assign_re(name: str) -> re.Pattern:
    n = re.escape(name)
    return re.compile(
        rf"(?:\b{n}\b\s*(?:\[[^\]\n]*\]\s*)*(?:=(?!=)|\+=|-=|\*=|/=|\|=|&=|\^=|\+\+|--)"
        rf"|(?:\+\+|--)\s*\b{n}\b"
        rf"|\b{n}\s*(?:\.|->)\s*(?:push_back|push_front|insert|erase|clear|resize|pop_back|pop_front|assign|swap|setAt|"
        rf"Add|Remove|Clear|Init|init|reset|Reset|set|push|pop|sort|remove)\s*\("
        rf"|(?:memset|memcpy|ZeroMemory|std::fill|fill|std::swap|swap|SAFE_DELETE|SAFE_DELETE_ARRAY|delete\s*\[\s*\]|delete)\s*\(?\s*&?\s*\b{n}\b)")


def _idents(text: str, prefix: str = "m_") -> set[str]:
    return set(re.findall(rf"\b{prefix}\w+", text))


def _ser_funcs(idx: CppIndex, cls: str) -> tuple[list[Func], list[Func]]:
    reads, writes = [], []
    for f in idx.funcs_by_cls.get(cls, []):
        if f.name in ("Read", "read", "ReadSupportingClassData"):
            reads.append(f)
        elif f.name in ("Write", "write", "WriteSupportingClassData"):
            writes.append(f)
    for f in idx.funcs_by_cls.get("", []):
        if f.name.startswith("operator") and re.search(rf"\b{re.escape(cls)}\s*&", f.header):
            if ">>" in f.name:
                reads.append(f)
            elif "<<" in f.name:
                writes.append(f)
    return reads, writes


def _with_helpers(idx: CppIndex, cls: str, funcs: list[Func]) -> str:
    """Bodies of `funcs` plus one level of same-class helpers that take the stream."""
    text = "".join(f.body for f in funcs)
    names = set(re.findall(r"\b([A-Za-z_]\w*)\s*\(\s*(?:kStream|saveTo|loadFrom|stream|kSaveStream|kLoadStream)\b", text))
    if any(f.cls == "" for f in funcs):
        # free operator<< / operator>> usually go through accessors: map getter/setter calls to the members they touch
        names |= set(re.findall(r"(?:\.|->)\s*([A-Za-z_]\w*)\s*\(", text))
    for f in idx.funcs_by_cls.get(cls, []):
        if f.name in names and f not in funcs:
            text += f.body
    return text


# ----------------------------------------------------------------------------------------------------------------
# nonser (C1)
# ----------------------------------------------------------------------------------------------------------------
def scan_nonser(idx: CppIndex) -> list[Finding]:
    out: list[Finding] = []
    for cls, decl in sorted(idx.classes.items()):
        reads, writes = _ser_funcs(idx, cls)
        if not writes:
            continue
        wtext = _with_helpers(idx, cls, writes)
        rtext = _with_helpers(idx, cls, reads)
        w_ids, r_ids = _idents(wtext), _idents(rtext)
        archive_streamed = "m_syncArchive" in w_ids
        logic = [f for f in idx.funcs_by_cls.get(cls, [])
                 if f.name not in SER_NAMES and f.name not in INIT_NAMES and f.name != cls and f.name != "~" + cls]
        logic_text = [(f, f.body) for f in logic]
        for mem in decl.members.values():
            n = mem.name
            if not n.startswith("m_") or mem.static or "&" in mem.type.split("<")[0]:
                continue
            if mem.type.startswith("FAutoVariable") and archive_streamed:
                continue
            in_w, in_r = n in w_ids, n in r_ids
            if in_w and in_r:
                continue
            if in_w != in_r and reads:
                out.append(Finding("nonser.asym", "C1", mem.file, mem.line, 2,
                                   f"{cls}::{n} is {'written but not read' if in_w else 'read but not written'} by serialization",
                                   f"{mem.type} {n}"))
                continue
            if in_w:
                continue
            wre = _assign_re(n)
            nre = re.compile(rf"\b{re.escape(n)}\b")
            writers = [f for f, b in logic_text if wre.search(b)]
            readers = [f for f, b in logic_text if nre.search(b)]
            readers_only = [f for f in readers if f not in writers]
            if writers and readers:
                score = 3 if not in_r else 2
            elif writers or readers_only:
                score = 1
            else:
                continue
            w_names = ", ".join(sorted({f.name for f in writers})[:4])
            r_names = ", ".join(sorted({f.name for f in readers})[:4])
            out.append(Finding("nonser.member", "C1", mem.file, mem.line, score,
                               f"{cls}::{n} not in Write ({'recomputed in Read' if in_r else 'not in Read'}); "
                               f"mutated in [{w_names}] read in [{r_names}]",
                               f"{mem.type} {n}"))
    out += scan_statics(idx)
    return out


_LOG_HINT = re.compile(r"(^|_|::)[Ll]og|Log[A-Z]|[Pp]erf|bFirstRun|szTemp|strTemp|[Bb]uffer|kTimer|[Dd]ebug|^buf$")


def scan_statics(idx: CppIndex) -> list[Finding]:
    out: list[Finding] = []
    for sf in idx.cpp_files():
        for f in sf.funcs:
            body = f.body
            for m in re.finditer(r"(?:(?<=[;{}])|(?<=\n)|(?<=^))\s*static\s+(?!const\b)(?!_cast)([^;(=]*?)\b([A-Za-z_]\w*)\s*(=[^;]*|\[[^\]]*\]\s*(=[^;]*)?|\([^;]*\))?;", body):
                decl = " ".join(m.group(0).split())
                if re.search(r"\bconst\b", m.group(1)):
                    continue
                name = m.group(2)
                line = sf.line_of(f.body_start + m.start(2))
                typ = m.group(1)
                score = 2
                if _LOG_HINT.search(name) or _LOG_HINT.search(f.name):
                    score = 1
                elif re.search(r"std::|vector|map|set|list|FStaticVector|CvWeightedVector", typ) and not re.search(r"(?i)temp|scratch|buffer|list$", name):
                    score = 2
                uses = len(re.findall(rf"\b{re.escape(name)}\b", body))
                # a static only assigned before every use is a scratch buffer; persistence matters when read first
                if re.search(r"(?i)^(s_|b|i|e|m_)", name) and score == 2 and not re.search(r"std::|vector|FStaticVector|CvWeightedVector", typ):
                    score = 3
                out.append(Finding("static.local", "C1", sf.rel, line, score,
                                   f"function-level static '{name}' in {f.qual} persists across turns/games, not saved ({uses} uses)",
                                   decl, f.qual))
        for name, decl, line in sf.globals:
            if re.match(r"(?i)g_?log|s_?log", name):
                continue
            if re.search(r"\b(GUID|IID|LPCSTR|const)\b", decl) or name.isupper():
                continue
            # DB-id caches (CvTypes s_eMISSION_* etc.) are refilled on every database load: C8 only with changing data profiles
            score = 1 if re.match(r"s_(e|ui)[A-Z_]", name) else 2
            out.append(Finding("static.global", "C8" if score == 1 else "C1", sf.rel, line, score,
                               f"mutable file-scope variable '{name}' (not serialized, survives between games)", decl))
    for cls, decl in sorted(idx.classes.items()):
        for mem in decl.members.values():
            if mem.static and not re.search(r"\bconst\b", mem.type):
                out.append(Finding("static.member", "C1", mem.file, mem.line, 2,
                                   f"static data member {cls}::{mem.name} (not serialized, shared by all instances/games)",
                                   f"static {mem.type} {mem.name}"))
    return out


# ----------------------------------------------------------------------------------------------------------------
# uninit (C2)
# ----------------------------------------------------------------------------------------------------------------
def scan_uninit(idx: CppIndex) -> list[Finding]:
    out: list[Finding] = []
    for cls, decl in sorted(idx.classes.items()):
        funcs = idx.funcs_by_cls.get(cls, [])
        ctors = [f for f in funcs if f.name == cls]
        if not ctors:
            continue
        inits = ctors + [f for f in funcs if f.name in INIT_NAMES]
        # one level of same-class calls from ctor / init functions
        called = set()
        for f in inits:
            called |= set(re.findall(r"\b([A-Za-z_]\w*)\s*\(", f.body))
            called |= set(re.findall(r"\b([A-Za-z_]\w*)\s*\(", f.header.split(")", 1)[-1]))  # init list
        inits += [f for f in funcs if f.name in called and f not in inits and f.name not in SER_NAMES]
        init_text = "".join(f.header + f.body for f in inits)
        reads, _ = _ser_funcs(idx, cls)
        r_ids = _idents("".join(f.body for f in reads))
        logic = [f for f in funcs if f not in inits and f.name not in SER_NAMES]
        for mem in decl.members.values():
            n = mem.name
            if mem.static or not POD_TYPE.match(mem.type) and "*" not in mem.type:
                continue
            if mem.type.startswith("FAutoVariable") or "<" in mem.type:
                continue
            if re.search(rf"\b{re.escape(n)}\b", init_text):
                continue
            readers = [f for f in logic if re.search(rf"\b{re.escape(n)}\b", f.body)]
            if not readers:
                continue
            score = 3 if "*" not in mem.type else 2
            out.append(Finding("uninit.member", "C2", mem.file, mem.line, score,
                               f"{cls}::{n} ({mem.type}) never set in ctor/reset/init"
                               f"{'; serialized, so garbage only until first load' if n in r_ids else '; not serialized'}; "
                               f"used in [{', '.join(sorted({f.name for f in readers})[:5])}]",
                               f"{mem.type} {n}"))
    return out


# ----------------------------------------------------------------------------------------------------------------
# ptrorder (C3)
# ----------------------------------------------------------------------------------------------------------------
_PTR_CONT = re.compile(r"\b(?:std::|stdext::|std::tr1::)?(set|map|multiset|multimap|hash_map|hash_set|unordered_set|unordered_map)\s*<\s*(?:const\s+)?([A-Za-z_]\w*)\s*\*")
_HASH_CONT = re.compile(r"\b(?:stdext::|std::tr1::|tr1::)?(hash_map|hash_set|unordered_set|unordered_map|unordered_multimap)\s*<")


def scan_ptrorder(idx: CppIndex) -> list[Finding]:
    out: list[Finding] = []
    for sf in idx.files:
        code = sf.code
        for rx, rule in ((_PTR_CONT, "ptr.container"), (_HASH_CONT, "hash.iter")):
            for m in rx.finditer(code):
                tail = code[m.end():m.end() + 300]
                nm = re.search(r">\s*(::\s*\w+\s*)?&?\s*([A-Za-z_]\w*)\s*[;=(\[,)]", tail)
                if not nm or nm.group(1):        # Container<...>::iterator it; -- not a declaration of a container
                    continue
                var = nm.group(2)
                vars_ = [var]
                line_txt = code[code.rfind("\n", 0, m.start()) + 1:m.start()]
                if re.search(r"\btypedef\b", line_txt):     # typedef std::map<CvCity*, ...> CityMap; -> variables of CityMap
                    vars_ = re.findall(rf"\b{re.escape(var)}\s+([A-Za-z_]\w*)\s*[;=(]", code) or [var]
                iterated = any(re.search(rf"\b{re.escape(v)}\s*(?:\.|->)\s*(?:begin|rbegin)\s*\(", code) for v in vars_)
                var = "/".join(vars_)
                key_t = m.group(2) if rule == "ptr.container" else "hash"
                if rule == "hash.iter" and not iterated:
                    continue
                score = 3 if iterated else 1
                if rule == "ptr.container" and re.match(r"(FAutoVariableBase|NotificationTarget|ObjectType)", key_t):
                    score = 1
                if rule == "ptr.container" and var.endswith("ToCheck"):
                    score = 1   # FSerialization sync-check registries: order only affects sync-check report order
                line = sf.line_of(m.start())
                f = sf.func_at(m.start())
                out.append(Finding(rule, "C3", sf.rel, line, score,
                                   f"{m.group(1)} keyed by {key_t}{'*' if rule == 'ptr.container' else ''} '{var}'"
                                   f"{' is iterated: order = heap addresses' if iterated else ' (membership only?)'}",
                                   sf.raw(line), f.qual if f else ""))
        for f in sf.funcs:
            body = f.body
            ptr_vars = set(re.findall(r"\b(?:Cv\w+|FAStarNode|CvAStarNode)\s*\*\s*(?:const\s+)?([A-Za-z_]\w*)", body)) | \
                set(re.findall(r"\b(?:Cv\w+|FAStarNode|CvAStarNode)\s*\*\s*(?:const\s+)?([A-Za-z_]\w*)", f.header))
            vec_ptr = set(re.findall(r"(?:vector|FStaticVector|FFastVector)\s*<\s*(?:const\s+)?\w+\s*\*[^>]*>\s*&?\s*([A-Za-z_]\w*)", body + f.header))
            for m in re.finditer(r"\b(?:std::)?(sort|stable_sort)\s*\(\s*([A-Za-z_]\w*)\s*\.\s*begin\s*\(\s*\)\s*,\s*\2\s*\.\s*end\s*\(\s*\)\s*\)", body):
                if m.group(2) in vec_ptr:
                    line = sf.line_of(f.body_start + m.start())
                    out.append(Finding("ptr.sort", "C3", sf.rel, line, 3, f"{m.group(1)} of pointer vector '{m.group(2)}' without comparator sorts by address", sf.raw(line), f.qual))
            if ptr_vars:
                pv = "|".join(sorted(map(re.escape, ptr_vars)))
                for m in re.finditer(rf"\b({pv})\s*(<=|>=|<(?![<=])|>(?![>=]))\s*({pv})\b", body):
                    line = sf.line_of(f.body_start + m.start())
                    out.append(Finding("ptr.compare", "C3", sf.rel, line, 2, f"relational compare of pointers {m.group(1)} {m.group(2)} {m.group(3)}", sf.raw(line), f.qual))
            for m in re.finditer(r"(?:\(\s*(?:int|uint|unsigned\s+int|long|unsigned\s+long|size_t|DWORD|intptr_t|uintptr_t|UINT_PTR)\s*\)\s*(?:this|p[A-Z]\w*|&\w+)\b(?!\s*(?:->|\.|\[|\())"
                                 r"|reinterpret_cast\s*<\s*(?:int|uint|unsigned\s+int|long|size_t|DWORD|intptr_t|uintptr_t|UINT_PTR)\s*>)", body):
                line = sf.line_of(f.body_start + m.start())
                out.append(Finding("ptr.asint", "C3", sf.rel, line, 2, "pointer converted to integer (address-dependent value)", sf.raw(line), f.qual))
    return out


# ----------------------------------------------------------------------------------------------------------------
# activeplayer (C5)
# ----------------------------------------------------------------------------------------------------------------
AP_TOKEN = r"(?:getActivePlayer|getActiveTeam|GetActivePlayer|GetActiveTeam|isLocalPlayer|IsHost|isActivePlayer|getActiveCivilizationType|GetActivePlayerID)\s*\(|\bbIsActiveTeam\b|\bbIsActivePlayer\b"
UI_HINT = re.compile(r"gDLL->|DLLUI|GetEngineUserInterface|\bNotification|Notifications\(\)|AddNotification|CvPopupInfo|addPopup|AddPopup|"
                     r"Localization::|GetLocalizedText|\.Format\s*\(|strBuffer|strText|strSummary|strMessage|szBuffer|"
                     r"[Ss]etDirty|[Ss]etLayoutDirty|setInfoPane|setFocus|PublishQueuedVisualizationMoves|lookAt|"
                     r"Achievement|SteamStat|CvMessage|AddMessage|GameplayUnit|GameplayCity|UpdateTurnTimer|"
                     r"pLog|LOG|[Ll]ogMsg|SoundTrigger|PlayScriptedSound|ClearHexHighlights|ShowPlotHighlight|"
                     r"SetCityBanner|DoUpdateTileHighlight|Events\b|setSelect|selectUnit|selectCity|AddToCityAdvisor|"
                     r"m_kUnitSelection|CvInterfaceModeInfo|InterfaceMode|setInterfaceMode|setLastSelected|CycleUnits")
MUT_STRONG = re.compile(r"\b(?:set|change|Set|Change|do|Do|init|Init|kill|Kill|create|Create|found|Found|acquire|Acquire|"
                        r"declare|Declare|make|Make|convert|Convert|spawn|Spawn|finish|Finish|grant|Grant|give|Give|"
                        r"receive|Receive|join|Join|pick|Pick|choose|Choose|adopt|Adopt|learn|Learn)[A-Z_]\w*\s*\(|"
                        r"\b(?:setHasTech|setRevealed|setVisible|kill|getJonRandNum|getJonRandNumVA|getJonRand)\s*\(|"
                        r"\bm_\w+\s*(?:\[[^\]]*\]\s*)*(?:=(?!=)|\+=|-=|\+\+|--)")
MUT_WEAK = re.compile(r"\b(?:add|Add|remove|Remove|push|Push|increment|Increment|decrement|update|Update|clear|Clear|reset|Reset|process|Process)[A-Z_]\w*\s*\(|"
                      r"\b(?:push_back|insert|erase)\s*\(")


def _match_paren(s: str, i: int) -> int:
    """index of the ')' matching '(' at i, or -1."""
    depth = 0
    for j in range(i, len(s)):
        c = s[j]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return j
    return -1


def _match_brace(s: str, i: int) -> int:
    depth = 0
    for j in range(i, len(s)):
        c = s[j]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return j
    return len(s) - 1


def _stmt_end(s: str, i: int) -> int:
    """end offset (inclusive) of the statement starting at i (block or simple statement incl. nested if/else)."""
    while i < len(s) and s[i].isspace():
        i += 1
    if i >= len(s):
        return len(s) - 1
    if s[i] == "{":
        return _match_brace(s, i)
    m = re.match(r"(if|while|for)\s*\(", s[i:])
    if m:
        close = _match_paren(s, i + m.end() - 1)
        end = _stmt_end(s, close + 1)
        if m.group(1) == "if":
            em = re.match(r"\s*else\b", s[end + 1:])
            if em:
                end = _stmt_end(s, end + 1 + em.end())
        return end
    depth = 0
    for j in range(i, len(s)):
        c = s[j]
        if c in "([":
            depth += 1
        elif c in ")]":
            depth -= 1
        elif c == ";" and depth <= 0:
            return j
    return len(s) - 1


def _classify_region(text: str) -> tuple[list[str], list[str]]:
    strong, weak = [], []
    for st in re.split(r"[;{}]", text):
        st1 = " ".join(st.split())
        if not st1 or UI_HINT.search(st1):
            continue
        if MUT_STRONG.search(st1):
            strong.append(st1[:140])
        elif MUT_WEAK.search(st1):
            weak.append(st1[:140])
    return strong, weak


def scan_activeplayer(idx: CppIndex) -> list[Finding]:
    out: list[Finding] = []
    for sf in idx.cpp_files():
        for f in sf.funcs:
            body = f.body
            if not re.search(AP_TOKEN, body) and "gDLL->send" not in body and "gDLL->Send" not in body \
                    and "AsyncRand" not in body and "ASyncRand" not in body:
                continue
            aliases = set(re.findall(rf"\b([A-Za-z_]\w*)\s*=\s*[^;=]*(?:{AP_TOKEN})", body))
            aliases -= {"if", "return"}
            pred = re.compile(AP_TOKEN + ("|" + "|".join(rf"\b{re.escape(a)}\b" for a in aliases) if aliases else ""))
            gated: list[tuple[int, int]] = []
            for m in re.finditer(r"\b(if|while)\s*\(", body):
                op = m.end() - 1
                cp = _match_paren(body, op)
                if cp < 0:
                    continue
                cond = body[op:cp + 1]
                if not pred.search(cond):
                    continue
                stmt_end = _stmt_end(body, cp + 1)
                region = body[cp + 1:stmt_end + 1]
                rstrip = " ".join(region.split())
                if re.fullmatch(r"\{?\s*return\b[^;]*;\s*\}?", rstrip):
                    region = body[stmt_end + 1:]          # early-return guard: rest of the function is gated
                    stmt_end = len(body) - 1
                gated.append((cp + 1, stmt_end))
                strong, weak = _classify_region(region)
                em = re.match(r"\s*else\b", body[stmt_end + 1:]) if stmt_end + 1 < len(body) else None
                if em:
                    e_end = _stmt_end(body, stmt_end + 1 + em.end())
                    s2, w2 = _classify_region(body[stmt_end + 1 + em.end():e_end + 1])
                    strong += s2
                    weak += w2
                    gated.append((stmt_end + 1, e_end))
                if not strong and not weak:
                    continue
                score = 3 if strong else 2
                line = sf.line_of(f.body_start + m.start())
                ex = "; ".join((strong or weak)[:3])
                out.append(Finding("ap.gate", "C5", sf.rel, line, score,
                                   f"{len(strong)} sim mutation(s), {len(weak)} weak, under active-player/host condition in {f.qual}: {ex}",
                                   " ".join(cond.split())[:160], f.qual))
            has_pred = bool(pred.search(body))
            for m in re.finditer(r"gDLL->\s*[sS]end(\w+)\s*\(", body):
                off = m.start()
                if any(a <= off <= b for a, b in gated) or has_pred:
                    continue
                if re.search(r"isHuman\s*\(|isLocal|GetActive", body):
                    continue
                line = sf.line_of(f.body_start + off)
                out.append(Finding("net.send", "C5", sf.rel, line, 2,
                                   f"gDLL->send{m.group(1)} in {f.qual} without active-player/human gating: if reached from "
                                   f"simulation on every peer, the command is sent N times", sf.raw(line), f.qual))
            for m in re.finditer(r"getAsyncRandNum\s*\(|getASyncRand\s*\(", body):
                line = sf.line_of(f.body_start + m.start())
                out.append(Finding("async.rand", "C5", sf.rel, line, 1,
                                   f"async RNG in {f.qual}: must not feed synchronous state", sf.raw(line), f.qual))
    return out


# ----------------------------------------------------------------------------------------------------------------
# Lua (C5)
# ----------------------------------------------------------------------------------------------------------------
_LUA_TOKENS = re.compile(r"--\[(=*)\[.*?\]\1\]|--[^\n]*|\[(=*)\[.*?\]\2\]|\"(?:\\.|[^\"\\\n])*\"|'(?:\\.|[^'\\\n])*'", re.S)


def strip_lua(src: str) -> str:
    def rep(m: re.Match) -> str:
        s = m.group(0)
        if s.startswith("--"):
            return re.sub(r"[^\n]", " ", s)
        if s[0] in "\"'":
            return s[0] + re.sub(r"[^\n]", " ", s[1:-1]) + s[-1]
        return re.sub(r"[^\n]", " ", s)
    return _LUA_TOKENS.sub(rep, src)


@dataclass
class LuaFunc:
    name: str
    start: int      # offset of 'function'
    end: int        # offset after 'end'
    line: int
    end_line: int


_LUA_KW = re.compile(r"\b(function|if|for|while|do|repeat|until|end)\b")


def lua_functions(code: str, line_of) -> list[LuaFunc]:
    funcs, stack = [], []        # stack items: (kind, start, name)
    pending_loop = 0
    for m in _LUA_KW.finditer(code):
        kw = m.group(1)
        if m.start() > 0 and code[m.start() - 1] in ".:":
            continue
        if kw == "function":
            pre = code[max(0, m.start() - 120):m.start()]
            post = code[m.end():m.end() + 120]
            nm = re.match(r"\s*([\w.:]+)\s*\(", post)
            if nm:
                name = nm.group(1)
            else:
                am = re.search(r"([\w.\[\]\"']+)\s*=\s*$", pre)
                cm = re.search(r"([\w.:]+)\s*\(\s*(?:[^()]*,\s*)?$", pre)
                name = am.group(1) if am else (f"<anon@{cm.group(1)}>" if cm else "<anon>")
            stack.append(("function", m.start(), name))
        elif kw in ("if",):
            stack.append(("if", m.start(), ""))
        elif kw in ("for", "while"):
            stack.append((kw, m.start(), ""))
            pending_loop += 1
        elif kw == "do":
            if pending_loop:
                pending_loop -= 1
            else:
                stack.append(("do", m.start(), ""))
        elif kw == "repeat":
            stack.append(("repeat", m.start(), ""))
        elif kw == "until":
            if stack and stack[-1][0] == "repeat":
                stack.pop()
        elif kw == "end":
            if stack:
                kind, start, name = stack.pop()
                if kind == "function":
                    funcs.append(LuaFunc(name, start, m.end(), line_of(start), line_of(m.end())))
    return funcs


LUA_MUT_STRONG = re.compile(r":(?:Change|Set|Init|Kill|Found|Create|Do|Disband|Convert|Grant|Give|Acquire|Adopt|Push|Pop|Join|"
                            r"Raze|Gift|Receive|Make|Declare|Spawn|Finish|JumpToNearestValidPlot|SetHasPromotion|AddFreeUnit|"
                            r"AddTemporaryDominanceZone|SetNumRealBuilding|SetNumFreeBuilding|ChangeNumResource)\w*\s*\(|"
                            r"\bGame\.(?:Rand|SetWinner|ChangeNumVotesForTeam|DoSpawn|SetPlotExtraYield|ChangeTurnTimer|SetCombatWarned)\w*\s*\(|"
                            r"\bMap\.(?:Rand|Change|Set)\w*\s*\(|\bSetPlotType|\bSetTerrainType|\bSetFeatureType|\bSetResourceType|\bSetImprovementType")
LUA_UI = re.compile(r"\b(?:Events|UI|Controls|ContextPtr|LuaEvents|Locale|print|Network)\b|:AddNotification|GameplayAlertMessage|IconHookup|SetToolTip")


def _lua_line_of(code: str):
    nl = [m.start() for m in re.finditer("\n", code)]
    return lambda off: bisect.bisect_left(nl, off) + 1


LUA_GAME_RECV = re.compile(r"(?i)(player|city|unit|plot|team|capital|owner|Players\s*\[|Teams\s*\[|GetPlot|GetCity|GetUnit|"
                           r"GetCapital|pLoop|kLoop|religion|league|^p[A-Z]\w*$)")
LUA_UI_RECV = re.compile(r"(?i)^(Controls|UIManager|ContextPtr|UI|instance|inst|controlTable|button|label|\w*Instance\w*|\w*Button\w*|"
                         r"\w*Label\w*|\w*Stack\w*|\w*Box\w*|\w*Text\w*|\w*Icon\w*|\w*Anim\w*|\w*Panel\w*|\w*Pulldown\w*|\w*Bar|\w*TT|\w*Image\w*)$")
LUA_UI_METHOD = re.compile(r"^(SetText|SetHide|SetDisabled|SetToolTip|SetColor|SetOffset|SetSize|SetTexture|SetAlpha|SetVoid|SetString|"
                           r"SetCheck|SetSelected|SetPercent|SetValue|SetFont|SetAnchor|SetWrapWidth|SetMaxSize|SetHeight|SetWidth|"
                           r"SetLabel|SetTitle|SetEnabled|SetFocus|SetPosition|SetInputHandler|SetUpdate|SetShowHide|SetCallback|"
                           r"SetSpeed|SetPause|SetToBeginning|SetEndVal|SetBeginVal|SetHexHighlight|SetSelection|SetChecked|"
                           r"AddNotification|AddMessage|DoUpdate\w*Panel|DoBeginDiploWithHuman\w*|Does[A-Z]\w*|Domain\w*|"
                           r"DoAutoSize|ChangeParent)")
_LUA_CALL = re.compile(r"([\w\.\[\]\(\)]+?)\s*:\s*([A-Z]\w*)\s*\(")


def _lua_mutators(text: str) -> list[str]:
    """Statements that change synchronous game state: game-object method calls (player:/city:/unit:/plot:/team:) with a
    mutating verb, plus Game./Map. mutators and the sync RNG. UI control calls and Network.Send* are excluded."""
    out = []
    for ln in text.split("\n"):
        s = " ".join(ln.split())
        if not s or re.match(r"(Events|LuaEvents)\.", s) or re.search(r"\bNetwork\.Send", s):
            continue
        hit = bool(re.search(r"\bGame\.(?:Rand|SetWinner|ChangeNumVotesForTeam|DoSpawn|SetPlotExtraYield|ChangeTurnTimer)\w*\s*\(|"
                             r"\bMap\.(?:Rand|Change|Set)\w*\s*\(", s))
        if not hit:
            for m in _LUA_CALL.finditer(s):
                recv = m.group(1).split(".")[-1] if not m.group(1).startswith(("Players", "Teams")) else m.group(1)
                meth = m.group(2)
                if LUA_UI_METHOD.match(meth) or LUA_UI_RECV.match(recv) or m.group(1).startswith(("Controls", "UI.", "UIManager")):
                    continue
                if not LUA_MUT_STRONG.search(":" + meth + "(") and not re.match(
                        r"(Change|Set|Init|Kill|Found|Create|Disband|Convert|Grant|Give|Acquire|Adopt|Raze|Gift|Receive|"
                        r"Declare|Spawn|Finish|Add|Remove|Push|Do)[A-Z]", meth):
                    continue
                if LUA_GAME_RECV.search(recv):
                    hit = True
                    break
        if hit:
            out.append(s[:120])
    return out


def _lua_block_end(code: str, then_off: int) -> int:
    """offset of the 'end'/'else'/'elseif' closing the if-block whose 'then' ends at then_off."""
    depth = 0
    pending = 0
    for m in re.finditer(r"\b(function|if|for|while|do|repeat|until|end|else|elseif)\b", code[then_off:]):
        kw = m.group(1)
        pos = then_off + m.start()
        if pos > 0 and code[pos - 1] in ".:":
            continue
        if kw in ("function", "if", "repeat"):
            depth += 1
        elif kw in ("for", "while"):
            depth += 1
            pending += 1
        elif kw == "do":
            if pending:
                pending -= 1
            else:
                depth += 1
        elif kw == "until":
            depth -= 1
        elif kw == "end":
            if depth == 0:
                return pos
            depth -= 1
        elif kw in ("else", "elseif") and depth == 0:
            return pos
    return len(code)


def lua_roots(with_game_ui: bool = False) -> list[Path]:
    roots = [p for p in LUA_DIRS if p.exists()]
    if GAME_LEKMOD_DLC.exists():
        roots.append(GAME_LEKMOD_DLC / "Lua")
    if with_game_ui and GAME_ASSETS.exists():
        roots += [GAME_ASSETS / "UI", GAME_ASSETS / "DLC" / "Expansion2" / "UI", GAME_ASSETS / "DLC" / "Expansion" / "UI"]
    return roots


def scan_lua(roots: list[Path] | None = None) -> list[Finding]:
    out: list[Finding] = []
    seen_hashes: dict[str, str] = {}
    for root in roots or lua_roots():
        in_repo = str(root.resolve()).lower().startswith(str(REPO).lower())
        files = _read_sources(root, (".lua",)) if in_repo else \
            [(p, p.read_text(encoding="latin-1")) for p in sorted(root.rglob("*.lua"))]
        for p, raw in files:
            key = f"{p.name}:{hash(raw.replace(chr(13), ''))}"
            if key in seen_hashes:      # same file shipped in repo and install: report once
                continue
            seen_hashes[key] = _rel(p)
            out += _scan_lua_file(p, raw)
    return out


def _scan_lua_file(p: Path, raw: str) -> list[Finding]:
    out: list[Finding] = []
    code = strip_lua(raw)
    line_of = _lua_line_of(code)
    raw_lines = raw.split("\n")
    rel = _rel(p)
    is_ui = bool(re.search(r"\bContextPtr\b|\bControls\.", code)) or "/UI/" in rel.replace("\\", "/")

    def snip(line: int) -> str:
        return raw_lines[line - 1].strip()[:160] if 0 < line <= len(raw_lines) else ""

    funcs = lua_functions(code, line_of)
    sync_handlers, local_handlers = set(), set()
    for m in re.finditer(r"\bGameEvents\.(\w+)\.Add\s*\(\s*([\w.:]+)?", code):
        if m.group(2) and m.group(2) != "function":
            sync_handlers.add(m.group(2))
    for m in re.finditer(r"\b(?:Events|LuaEvents)\.(\w+)\.Add\s*\(\s*([\w.:]+)?", code):
        if m.group(2) and m.group(2) != "function":
            local_handlers.add(m.group(2))
    for m in re.finditer(r":RegisterCallback\s*\(\s*[\w.]+\s*,\s*([\w.:]+)|ContextPtr:Set\w+Handler\s*\(\s*([\w.:]+)|ContextPtr:SetUpdate\s*\(\s*([\w.:]+)", code):
        local_handlers.add(next(g for g in m.groups() if g))

    def kind_of(fn: LuaFunc) -> str:
        if fn.name in sync_handlers or "GameEvents" in fn.name:
            return "sync"
        if fn.name in local_handlers or re.match(r"<anon@(Events|LuaEvents)\.", fn.name) or "RegisterCallback" in fn.name:
            return "local"
        return "ui" if is_ui else "unknown"

    def enclosing(off: int) -> LuaFunc | None:
        best = None
        for fn in funcs:
            if fn.start <= off < fn.end and (best is None or fn.start > best.start):
                best = fn
        return best

    # lua.activeplayer
    for m in re.finditer(r"\b(if|elseif)\b(.*?)\bthen\b", code, re.S):
        cond = m.group(2)
        ap = re.search(r"GetActivePlayer\s*\(|GetActiveTeam\s*\(", cond)
        if not ap:
            continue
        fn = enclosing(m.start())
        then_end = m.end()
        block_end = _lua_block_end(code, then_end)
        block = code[then_end:block_end]
        if re.fullmatch(r"\s*return\b[^\n]*\s*", block) and fn:
            region = code[block_end:fn.end]
            how = "early-return guard"
        else:
            region = block
            how = "if-block"
        muts = _lua_mutators(region)
        if not muts:
            continue
        line = line_of(m.start(2) + ap.start())         # the line with the active-player test, not the 'if'
        k = kind_of(fn) if fn else "unknown"
        score = 3 if k in ("sync", "unknown") else 2
        out.append(Finding("lua.activeplayer", "C5", rel, line, score,
                           f"{len(muts)} state mutation(s) behind GetActivePlayer ({how}) in {fn.name if fn else '<chunk>'} "
                           f"[{k} handler]: {'; '.join(muts[:3])}", snip(line), fn.name if fn else ""))

    def own_body(fn: LuaFunc) -> str:
        """function text with nested function bodies blanked (EUI wraps whole files in one Events handler)."""
        chars = list(code[fn.start:fn.end])
        for other in funcs:
            if other is not fn and fn.start < other.start and other.end <= fn.end:
                for i in range(other.start - fn.start, other.end - fn.start):
                    if chars[i] != "\n":
                        chars[i] = " "
        return "".join(chars)

    # reachability inside this file: names referenced from chunk-level code, transitively through live functions
    def _short(n: str) -> str:
        return n.split(".")[-1].split(":")[-1]

    def _refs(text: str) -> set[str]:
        return {_short(f.name) for f in funcs
                if not f.name.startswith("<") and re.search(rf"(?<!function )(?<![\w.:]){re.escape(_short(f.name))}\b", text)}

    top = list(code)
    for f in funcs:
        for i in range(f.start, f.end):
            if top[i] != "\n":
                top[i] = " "
    live_short = _refs("".join(top))
    # anonymous functions and table/method members (callable from other files or by the engine) count as live roots
    root = [f for f in funcs if f.name.startswith("<") or re.search(r"[.:\[]", f.name) or _short(f.name) in live_short]
    live = {f.name for f in funcs if f.name.startswith("<")}
    todo = root
    while todo:
        f = todo.pop()
        if f.name in live and not f.name.startswith("<"):
            continue
        live.add(f.name)
        for n in _refs(own_body(f)):
            todo += [g for g in funcs if _short(g.name) == n and g.name not in live]

    for fn in funcs:
        body = own_body(fn)
        k = kind_of(fn)
        if k in ("local", "ui"):
            muts = _lua_mutators(body)
            if muts and k == "local" or (muts and is_ui):
                reach = k == "local" or fn.name in live
                out.append(Finding("lua.events-mutator", "C5", rel, fn.line, (3 if k == "local" else 2) if reach else 1,
                                   f"{fn.name} is a {'Events/UI' if k == 'local' else 'UI-context'} function (runs on one client) "
                                   f"and mutates game state: {'; '.join(muts[:3])}"
                                   f"{'' if reach else ' (not reachable from active code in this file)'}", snip(fn.line), fn.name))
        called = k == "local" or fn.name in live
        for m in re.finditer(r"\bGame\.Rand\s*\(", body):
            if k in ("local", "ui"):
                line = line_of(fn.start + m.start())
                out.append(Finding("lua.rand-ui", "C5", rel, line, 3 if called else 1,
                                   f"Game.Rand (synchronous RNG) in {k} function {fn.name}: advances the sync seed on one client only"
                                   f"{'' if called else ' (function is never called in active code)'}",
                                   snip(line), fn.name))
        for m in re.finditer(r"\bmath\.random\s*\(", body):
            muts = _lua_mutators(body)
            line = line_of(fn.start + m.start())
            if k == "sync" or muts:
                out.append(Finding("lua.math-random", "C5", rel, line, 3,
                                   f"math.random (local RNG) in {fn.name} [{k}] which mutates state", snip(line), fn.name))
        for m in re.finditer(r"\bGameEvents\.(\w+)\.Remove\s*\(", body):
            line = line_of(fn.start + m.start())
            out.append(Finding("lua.selfremove", "C1", rel, line, 3,
                               f"GameEvents.{m.group(1)}.Remove inside {fn.name}: handler set differs between continuous play "
                               f"and after load/resync (re-registered on load)", snip(line), fn.name))
    for m in re.finditer(r"\bipairs\s*\(\s*(Players|Teams)\s*\)", code):
        line = line_of(m.start())
        fn = enclosing(m.start())
        out.append(Finding("lua.ipairs-players", "-", rel, line, 2,
                           f"ipairs({m.group(1)}) skips index 0 (Players/Teams are 0-based)", snip(line), fn.name if fn else ""))
    pvars = set(re.findall(r"\blocal\s+(\w+)\s*=\s*Players\s*\[[^\]\n]*\][ \t]*(?:;|\n|$)", code))
    for v in pvars:
        for m in re.finditer(rf"\b{re.escape(v)}\s*(==|~=)\s*([\w.]+)", code):
            rhs = m.group(2)
            if rhs in ("nil", "true", "false") or rhs in pvars or rhs.startswith("Players"):
                continue
            if re.search(r"(?i)id$|^\d+$|^i[A-Z]|player_or|_id", rhs):
                line = line_of(m.start())
                out.append(Finding("lua.obj-eq-num", "-", rel, line, 2,
                                   f"player object '{v}' compared with number-like '{rhs}' (always {'false' if m.group(1) == '==' else 'true'})",
                                   snip(line)))
    # module-level caches written from handlers
    top_tables = set()
    for m in re.finditer(r"^(?:local\s+)?([A-Za-z_]\w*)\s*=\s*\{\s*\}", code, re.M):
        if not enclosing(m.start()):
            top_tables.add((m.group(1), line_of(m.start())))
    for name, line in top_tables:
        writes = [fn for fn in funcs if re.search(rf"\b{re.escape(name)}\s*(\[[^\]]*\]|\.\w+)\s*=", code[fn.start:fn.end])]
        if writes and (sync_handlers or local_handlers):
            out.append(Finding("lua.cache", "C1", rel, line, 2,
                               f"module-level table '{name}' written in [{', '.join(sorted({f.name for f in writes})[:4])}]: "
                               f"lives only in this Lua state (not saved, not rebuilt on resync unless recomputed)", snip(line)))
    objkeyed = set(re.findall(r"\b(\w+)\s*\[\s*(?:p?[Pp]lot|p?[Uu]nit|p?[Cc]ity|p[A-Z]\w*)\s*\]\s*=", code))
    for name in objkeyed:
        for m in re.finditer(rf"\bpairs\s*\(\s*{re.escape(name)}\s*\)", code):
            line = line_of(m.start())
            out.append(Finding("lua.pairs-objkey", "C3", rel, line, 2,
                               f"pairs({name}) over a table keyed by game objects: Lua hashes tables/userdata by address",
                               snip(line)))
    return out


# ----------------------------------------------------------------------------------------------------------------
# driver
# ----------------------------------------------------------------------------------------------------------------
def run(which: str, with_game_ui: bool = False) -> tuple[list[Finding], dict]:
    t0 = time.time()
    names = SCANNERS if which == "all" else (which,)
    out: list[Finding] = []
    stats: dict = {}
    idx = None
    if any(n != "lua-local" for n in names):
        idx = CppIndex()
        stats["cpp_files"] = len(idx.files)
        stats["cpp_parse_s"] = round(idx.elapsed, 2)
    for n in names:
        t = time.time()
        if n == "nonser":
            got = scan_nonser(idx)
        elif n == "uninit":
            got = scan_uninit(idx)
        elif n == "ptrorder":
            got = scan_ptrorder(idx)
        elif n == "activeplayer":
            got = scan_activeplayer(idx)
        elif n == "lua-local":
            got = scan_lua(lua_roots(with_game_ui))
        else:
            raise ValueError(n)
        stats[f"{n}_s"] = round(time.time() - t, 2)
        stats[f"{n}_findings"] = len(got)
        out += got
    out.sort(key=lambda f: (-f.score, f.rule, f.file, f.line))
    stats["total_s"] = round(time.time() - t0, 2)
    return out, stats


def main(argv: list[str]) -> int:
    import argparse
    ap = argparse.ArgumentParser(prog="python -m civsim.detlab scan")
    ap.add_argument("which", choices=SCANNERS + ("all",))
    ap.add_argument("--json", metavar="OUT", help="write findings as JSON to OUT")
    ap.add_argument("--min-score", type=int, default=2, help="hide findings below this score in text output (default 2)")
    ap.add_argument("--rule", help="only rules starting with this prefix")
    ap.add_argument("--with-game-ui", action="store_true", help="lua-local: also scan base-game UI Lua in the game install")
    ap.add_argument("--limit", type=int, default=0, help="max text lines (0 = all)")
    ap.add_argument("--rev", help="scan repo sources at this git revision (e.g. HEAD) instead of the working tree; "
                                  "line numbers then stay stable while other agents edit files")
    a = ap.parse_args(argv)
    global _REV, _GLOBAL_DEFINES
    if a.rev:
        _REV, _GLOBAL_DEFINES = a.rev, None
    findings, stats = run(a.which, a.with_game_ui)
    stats["rev"] = a.rev or "worktree"
    if a.rule:
        findings = [f for f in findings if f.rule.startswith(a.rule)]
    if a.json:
        Path(a.json).parent.mkdir(parents=True, exist_ok=True)
        Path(a.json).write_text(json.dumps({"schema_version": 1, "stats": stats, "findings": [asdict(f) for f in findings]},
                                           indent=1), encoding="utf-8")
    shown = [f for f in findings if f.score >= a.min_score]
    if a.limit:
        shown = shown[:a.limit]
    for f in shown:
        print(f"[{f.score}] {f.rule:<20} {f.loc()}  {f.msg}")
    by_rule: dict[str, int] = {}
    for f in findings:
        by_rule[f.rule] = by_rule.get(f.rule, 0) + 1
    print(f"-- {len(findings)} findings ({len(shown)} shown, score>={a.min_score}); "
          + ", ".join(f"{k}={v}" for k, v in sorted(by_rule.items())) + f"; {stats['total_s']} s", file=sys.stderr)
    return 0
