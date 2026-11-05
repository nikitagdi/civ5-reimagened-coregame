"""EUI vs standard UI: which UI Lua/XML files does a player run, and which differences can reach the simulation?

Lekmod ships two sets of UI files (Lua\\tmp\\ui and Lua\\tmp\\eui) and copies one of them into Lua\\UI (ui_check.bat, or
LekmodInstaller\\ui_manager.py). Players with EUI (Assets\\DLC\\UI_bc1) and without it therefore run different UI Lua. This module
reproduces the selection, resolves every UI file name the way the exe's UI skin lookup does (highest package priority wins),
lists what differs between the two player profiles and scans the differing Lua for anything that can touch synchronous state.

  python -m civsim.detlab.euiui diff   [--game <Civ5 root>] [--out headless/out/eui-vs-ui] [--reference]
  python -m civsim.detlab.euiui ai     [--turns 50]        AI-only civhost runs, EUI-profile Lua vs standard-profile Lua (digest.bin compare)
  python -m civsim.detlab.euiui mixed  [--turns 60]        lockstep session with a human slot: EUI notification handler on one peer

`diff --reference` writes headless/reference/profiles-eui-vs-ui.md . Everything under the game
folder is read-only; outputs go to headless/out/eui-vs-ui (staged copies of Lua, runs, analysis.json) and headless/reference.
civhost is run from a private copy (default headless/out/host-eui/civhost.exe): pass --host to use another one.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

from .. import common as C

GAME_DEFAULT = Path(os.environ.get("CIV5_GAME_DIR") or r"C:\Program Files (x86)\Steam\steamapps\common\Sid Meier's Civilization V")
LEKMOD_DEFAULT = "LEKMOD_v34.15"


# ----------------------------------------------------------------------------------------------------------------
# ui_check.bat emulation
# ----------------------------------------------------------------------------------------------------------------
@dataclass
class Cond:
    kind: str            # "exist" | "find"
    path: str            # with %vars%
    neg: bool = False
    text: str = ""


@dataclass
class Node:
    op: str              # "copy" | "set" | "if"
    a: str = ""
    b: str = ""
    cond: Cond | None = None
    then: list = field(default_factory=list)
    els: list | None = None
    line: int = 0


_IF_EXIST = re.compile(r'(?i)^IF\s+(NOT\s+)?EXIST\s+"([^"]+)"\s*\($')
_FIND = re.compile(r'(?i)^FIND\s+(%text%|"[^"]*")\s+"([^"]+)"\s*>\s*nul\s+2>&1\s+&&\s*\($')
_COPY = re.compile(r'(?i)^copy\s+/y\s+"([^"]+)"\s+"([^"]+)"\s*>\s*nul$')
_SET = re.compile(r'(?i)^set\s+(\w+)=(.*)$')
_CLOSE_ELSE = re.compile(r'(?i)^\)\s*(ELSE|\|\|)\s*\($')


def parse_bat(text: str) -> list[Node]:
    lines = [ln.rstrip("\r") for ln in text.split("\n")]

    def block(i: int) -> tuple[list[Node], int]:
        out: list[Node] = []
        while i < len(lines):
            ln = lines[i].strip()
            if not ln or ln.upper().startswith("REM") or ln.startswith("@"):
                i += 1
                continue
            if ln.startswith(")"):
                return out, i
            m = _IF_EXIST.match(ln)
            f = _FIND.match(ln) if not m else None
            if m or f:
                cond = Cond("exist", m.group(2), neg=bool(m.group(1))) if m else Cond("find", f.group(2), text=f.group(1))
                then, i = block(i + 1)
                els = None
                if _CLOSE_ELSE.match(lines[i].strip()):
                    els, i = block(i + 1)
                i += 1
                out.append(Node("if", cond=cond, then=then, els=els, line=i))
                continue
            c = _COPY.match(ln)
            if c:
                out.append(Node("copy", c.group(1), c.group(2), line=i + 1))
            else:
                s = _SET.match(ln)
                if s:
                    out.append(Node("set", s.group(1).lower(), s.group(2).strip(), line=i + 1))
            i += 1
        return out, i

    return block(0)[0]


class BatRun:
    """Executes the parsed script against the real folder layout without copying anything: records (dest, src, ok, line)."""

    def __init__(self, lekmod_dir: Path, dlc_dir: Path, eui_present: bool, eui_dir: Path | None):
        self.lekmod = lekmod_dir
        self.dlc = dlc_dir
        self.eui_present = eui_present
        self.eui_dir = eui_dir
        self.env = {"patchfolder": str(lekmod_dir), "euifolder": "UI_bc1_xits", "text": ""}
        self.copies: list[tuple[str, str, bool, int]] = []

    def sub(self, s: str) -> str:
        return re.sub(r"%(\w+)%", lambda m: self.env.get(m.group(1).lower(), m.group(0)), s)

    def resolve(self, p: str) -> Path:
        p = self.sub(p)
        pp = Path(p)
        if pp.is_absolute():
            return pp
        rel = p.replace("\\", "/")        # relative to the DLC folder (the .bat does "cd .." from the Lekmod folder)
        head = rel.split("/")[0].lower()
        if head == "ui_bc1":
            if not self.eui_present or self.eui_dir is None:
                return self.dlc / "__no_such_eui__" / "/".join(rel.split("/")[1:])
            return self.eui_dir / "/".join(rel.split("/")[1:])
        return self.dlc / rel

    def run(self, nodes: list[Node]) -> bool:
        ok = True
        for n in nodes:
            if n.op == "set":
                if "%cd%" in n.b.lower():
                    continue                    # set patchfolder=%cd%: already seeded with the Lekmod folder
                self.env[n.a] = n.b
            elif n.op == "copy":
                src = self.resolve(n.a)
                dst = self.sub(n.b)
                good = src.is_file()
                rel_dst = os.path.relpath(dst, str(self.lekmod / "Lua" / "UI")).replace("\\", "/")
                try:
                    rel_src = os.path.relpath(str(src), str(self.lekmod / "Lua" / "tmp")).replace("\\", "/")
                except ValueError:
                    rel_src = str(src)
                self.copies.append((rel_dst, rel_src, good, n.line))
                ok = good
            else:
                c = n.cond
                if c.kind == "exist":
                    truth = self.eui_present if c.path.lower() == "ui_bc1" else self.resolve(c.path).exists()
                    if c.neg:
                        truth = not truth
                    ok = self.run(n.then if truth else (n.els or []))
                else:       # FIND text file ... && (then) || (else)
                    f = self.resolve(c.path)
                    text = (self.env["text"] if c.text == "%text%" else c.text).strip('"')
                    found = False
                    if f.is_file():
                        try:
                            found = text in f.read_text(encoding="latin-1")
                        except OSError:
                            found = False
                    if found:
                        ok_then = self.run(n.then)
                        # cmd: "A && (B) || (C)" also runs C when B failed (a copy whose source is missing)
                        ok = self.run(n.els) if (not ok_then and n.els is not None) else ok_then
                    else:
                        ok = self.run(n.els or [])
        return ok


def emulate_bat_text(bat_text: str, lekmod: Path, dlc: Path, eui_present: bool, eui_dir: Path | None) -> dict[str, dict]:
    """Run a ui_check.bat text against a folder layout: `lekmod` holds Lua\\tmp (only the existence of the sources matters), `dlc` is the DLC
    folder next to it, `eui_dir` the EUI folder UI_bc1 (anywhere: `UI_bc1\\...` paths are redirected to it). Used by emulate_bat."""
    r = BatRun(lekmod, dlc, eui_present and eui_dir is not None and eui_dir.is_dir(), eui_dir)
    r.run(parse_bat(bat_text))
    return {dst: {"src": src, "line": line} for dst, src, ok, line in r.copies if ok}     # a failed copy leaves nothing


def emulate_bat(game: Path, lekmod_name: str, eui_present: bool) -> dict[str, dict]:
    """{dest name relative to Lua\\UI: {src (relative to Lua\\tmp), line}} that ui_check.bat leaves in Lua\\UI."""
    dlc = game / "Assets" / "DLC"
    lekmod = dlc / lekmod_name
    bat = (lekmod / "ui_check.bat").read_text(encoding="latin-1")
    return emulate_bat_text(bat, lekmod, dlc, eui_present, dlc / "UI_bc1")


def emulate_installer(game: Path, lekmod_name: str, is_eui: bool, stage: Path) -> dict[str, dict]:
    """Run the repo's LekmodInstaller UIManager.configure_ui_files on a staged copy of Lua\\tmp (is_eui = the choice made in
    the installer's UI dialog, not a detection)."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("lekmod_ui_manager", C.REPO / "LekmodInstaller" / "ui_manager.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    root = stage / lekmod_name
    if root.exists():
        shutil.rmtree(root)
    shutil.copytree(game / "Assets" / "DLC" / lekmod_name / "Lua" / "tmp", root / "Lua" / "tmp")
    (root / "Lua" / "UI").mkdir(parents=True)
    mod.UIManager().configure_ui_files(str(stage), "Enhanced UI" if is_eui else "Standard UI", lambda *_: None)
    out: dict[str, dict] = {}
    for p in sorted((root / "Lua" / "UI").rglob("*")):
        if p.is_file():
            out[p.relative_to(root / "Lua" / "UI").as_posix()] = {"src": "(installer)", "path": str(p)}
    return out


# ----------------------------------------------------------------------------------------------------------------
# UI skin resolution: file name -> winning file, highest package priority wins
# ----------------------------------------------------------------------------------------------------------------
def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes().replace(b"\r\n", b"\n")).hexdigest()[:16]


@dataclass
class Layer:
    name: str
    priority: int
    root: Path
    files: dict[str, Path] = field(default_factory=dict)      # lower-case file name -> path


UI_EXT = (".lua", ".xml")


def _scan(root: Path, layer: Layer) -> Layer:
    for p in sorted(root.rglob("*")):
        if p.is_file() and p.suffix.lower() in UI_EXT:
            layer.files[p.name.lower()] = p
    return layer


def build_layers(game: Path, lekmod_name: str, eui_present: bool, lekmod_ui: dict[str, Path]) -> list[Layer]:
    """Layers from low to high priority. lekmod_ui = {dest name (Lua\\UI relative) -> real file} for this profile.
    Priorities: base game 0, Expansion 100, Expansion2 200, EUI_2 250 (EUI_0/1 are 050/150, same directories), Lekmod 300."""
    a = game / "Assets"
    layers = [_scan(a / "UI", Layer("base UI", 0, a / "UI")),
              _scan(a / "DLC" / "Expansion" / "UI", Layer("Expansion (G&K) UI", 100, a / "DLC" / "Expansion" / "UI")),
              _scan(a / "DLC" / "Expansion2" / "UI", Layer("Expansion2 (BNW) UI", 200, a / "DLC" / "Expansion2" / "UI"))]
    eui = a / "DLC" / "UI_bc1"
    if eui_present and eui.is_dir():
        lay = Layer("EUI (UI_bc1)", 250, eui)
        for d in sorted(eui.iterdir()):
            if d.is_dir() and not d.name.lower().startswith("art"):
                _scan(d, lay)
        layers.append(lay)
    lk = a / "DLC" / lekmod_name / "Lua"
    lay = Layer("Lekmod (GameplaySkin Lua)", 300, lk)
    # The GameplaySkin directory "Lua" is searched recursively: every .lua/.xml counts, *.ignore / *.IGNORE does not (that is what
    # Lua\tmp relies on). Lua\UI is replaced by the file set this profile's ui_check run produced.
    for p in sorted(lk.rglob("*")):
        if p.is_file() and p.suffix.lower() in UI_EXT and p.relative_to(lk).parts[0].lower() != "ui":
            lay.files[p.name.lower()] = p
    for name, real in lekmod_ui.items():
        lay.files[Path(name).name.lower()] = real
    layers.append(lay)
    return layers


def resolve_all(layers: list[Layer]) -> dict[str, tuple[Layer, Path]]:
    win: dict[str, tuple[Layer, Path]] = {}
    for lay in sorted(layers, key=lambda x: x.priority):
        for k, p in lay.files.items():
            win[k] = (lay, p)
    return win


def lekmod_ui_files(game: Path, lekmod_name: str, copies: dict[str, dict]) -> dict[str, Path]:
    tmp = game / "Assets" / "DLC" / lekmod_name / "Lua" / "tmp"
    out: dict[str, Path] = {}
    for dest, info in copies.items():
        p = Path(info["path"]) if "path" in info else tmp / info["src"]
        if p.is_file():
            out[dest] = p
    return out


@dataclass
class Profile:
    label: str
    eui_present: bool
    copies: dict[str, dict]
    lk_ui: dict[str, Path]
    win: dict[str, tuple[Layer, Path]]


def make_profile(game: Path, lekmod_name: str, label: str, eui_present: bool, how: str = "bat", stage: Path | None = None,
                 installer_is_eui: bool | None = None) -> Profile:
    if how == "bat":
        cp = emulate_bat(game, lekmod_name, eui_present)
    else:
        cp = emulate_installer(game, lekmod_name, eui_present if installer_is_eui is None else installer_is_eui,
                               stage or (C.OUT / "eui-vs-ui" / "stage-installer" / label))
    lk = lekmod_ui_files(game, lekmod_name, cp)
    return Profile(label, eui_present, cp, lk, resolve_all(build_layers(game, lekmod_name, eui_present, lk)))


# ----------------------------------------------------------------------------------------------------------------
# context graph: which UI Lua files actually run (InGame.xml -> LuaContext -> include / LoadNewContext)
# ----------------------------------------------------------------------------------------------------------------
_LUACTX = re.compile(r'(?is)<LuaContext\b[^>]*?FileName\s*=\s*"([^"]+)"')
_XMLINC = re.compile(r'(?is)<Include\b[^>]*?FileName\s*=\s*"([^"]+)"')
_LUAINC = re.compile(r'(?<![\w.:])include\s*\(?\s*["\']([^"\']+)["\']')
_LOADCTX = re.compile(r'LoadNewContext\s*\(\s*["\']([^"\']+)["\']')


def stem(name: str) -> str:
    n = name.replace("\\", "/").rsplit("/", 1)[-1]
    return re.sub(r"(?i)\.(lua|xml)$", "", n).lower()


def read_text(p: Path) -> str:
    return p.read_text(encoding="latin-1")


def strip_lua_comments(txt: str) -> str:
    return re.sub(r"--[^\n]*", "", re.sub(r"--\[(=*)\[.*?\]\1\]", "", txt, flags=re.S))


def live_graph(win: dict[str, tuple[Layer, Path]], roots: list[str]) -> dict[str, dict]:
    """{stem: {"lua": Path|None, "xml": Path|None, "kinds": {...}, "via": {...}}} for every context / include reachable from the
    root names (kinds: root, ctx = <LuaContext> in XML, include = Lua include(), loadnewcontext, xmlinclude)."""
    out: dict[str, dict] = {}
    todo: list[tuple[str, str, str]] = [(r, "root", "") for r in roots]
    while todo:
        name, kind, parent = todo.pop()
        st = stem(name)
        ent = out.get(st)
        if ent is None:
            lua, xml = win.get(st + ".lua"), win.get(st + ".xml")
            ent = out[st] = {"lua": lua[1] if lua else None, "xml": xml[1] if xml else None, "kinds": set(), "via": set()}
            if ent["xml"] is not None:
                txt = read_text(ent["xml"])
                todo += [(m.group(1), "ctx", st) for m in _LUACTX.finditer(txt)]
                todo += [(m.group(1), "xmlinclude", st) for m in _XMLINC.finditer(txt)]
            if ent["lua"] is not None:
                txt = strip_lua_comments(read_text(ent["lua"]))
                todo += [(m.group(1), "include", st) for m in _LUAINC.finditer(txt)]
                todo += [(m.group(1), "loadnewcontext", st) for m in _LOADCTX.finditer(txt)]
        ent["kinds"].add(kind)
        if parent:
            ent["via"].add(parent)
    return out


# ----------------------------------------------------------------------------------------------------------------
# static facts about a UI Lua file
# ----------------------------------------------------------------------------------------------------------------
_MUT_PREFIX = re.compile(r"^(Set|Change|Add|Do|Init|Kill|Found|Create|Disband|Convert|Push|Pop|Raze|Acquire|Receive|Hurry|Conscript|"
                         r"Embark|Promote|Lead|Declare|Make|Meet|Clear|Alter|Choose|Enhance|Give|Rotate|Specialize|Grant|Remove|"
                         r"Erase|Nuke|Jump|Finish|Reveal|Cycle|Select|Adopt|Cancel|Spread|Pillage|Rebase|Unload|Update)")
# names the prefix test flags although they only touch UI state
_NOT_SIM = {"SetHotKeyNumber", "SetEmbarkedGraphicOverride", "SetAdvisorMessageHasBeenSeen", "SetAdvisorRecommenderCity",
            "SetAdvisorRecommenderTech", "SetAdvisorBadAttackInterrupt", "SetAdvisorCityAttackInterrupt", "SetEverRightClickMoved",
            "SetStaticTutorialActive", "UpdateFOW", "UpdateFog", "UpdateVisibility", "SetScoreDirty", "UpdateScore", "MouseoverUnit",
            "SelectedUnit_SpeculativePopupTradeRoute_Display", "SelectedUnit_SpeculativePopupTradeRoute_Hide", "DoUpdateProximityToPlayer",
            "DoTradeScreenOpened", "DoTradeScreenClosed", "DoBeginDiploWithHuman", "DoBeginDiploWithHumanEx", "UpdateActions",
            "UpdateDeferredFog", "ExitLeaderScreen", "SetPlotExtraYield"}
# UI.* / Network.* calls that never reach synchronous state (selection, path preview, AI queries on the scratch deal, getters)
_LOCAL_CALL = re.compile(r"^(UI\.(DoSelectCityAtPlot|SendPathfinderUpdate|RequestLeaveLeader|DoWhatDoesAIWant|DoWhatWillAIGive|"
                         r"DoEqualizeDealWithHuman)|Network\.(Get|Is)\w+)$")
_API_CLASSES = ("Player", "City", "Unit", "Plot", "Team", "TeamTech", "Deal")


def lua_api_names() -> dict[str, set[str]]:
    """Method names the DLL binds per Lua class (CvLua*.cpp), read from this repo's DLL sources."""
    lua = C.REPO / "LEKMOD_DLL" / "CvGameCoreDLL_Expansion2" / "Lua"
    files = {"Player": "CvLuaPlayer.cpp", "City": "CvLuaCity.cpp", "Unit": "CvLuaUnit.cpp", "Plot": "CvLuaPlot.cpp", "Team": "CvLuaTeam.cpp",
             "Game": "CvLuaGame.cpp", "Map": "CvLuaMap.cpp", "TeamTech": "CvLuaTeamTech.cpp", "Deal": "CvLuaDeal.cpp"}
    out: dict[str, set[str]] = {}
    for cls, fn in files.items():
        p = lua / fn
        names: set[str] = set()
        if p.is_file():
            t = p.read_text(encoding="latin-1")
            names = set(re.findall(r"\bMethod\((\w+)\)", t)) | set(re.findall(r"\bMethod\(\w+,\s*(\w+)\)", t))
        out[cls] = names
    return out


@dataclass
class Hit:
    kind: str            # gameevents | events | net | game | mutate | rand | savedata | dbwrite | pregame
    what: str
    line: int
    func: str
    ctx: str             # chunk | GameEvents:<name> | Events:<name> | LuaEvents:<name> | callback | function
    cls: str = ""        # API class of a mutate hit (Player/City/Unit/Plot/Team/TeamTech/Deal)


def lua_facts(path: Path, api: dict[str, set[str]]) -> dict:
    from . import scan
    raw = read_text(path)
    code = scan.strip_lua(raw)
    line_of = scan._lua_line_of(code)
    funcs = scan.lua_functions(code, line_of)

    def enclosing(off: int):
        best = None
        for fn in funcs:
            if fn.start <= off < fn.end and (best is None or fn.start > best.start):
                best = fn
        return best

    ev_by_name: dict[str, str] = {}
    for m in re.finditer(r"\b(GameEvents|Events|LuaEvents)\.(\w+)\.Add\s*\(\s*([\w.:]+)", code):
        if m.group(3) != "function":
            ev_by_name[m.group(3)] = f"{m.group(1)}:{m.group(2)}"
    cb_names: set[str] = set()
    for m in re.finditer(r":RegisterCallback\s*\(\s*[\w.]+\s*,\s*([\w.:]+)|ContextPtr:Set\w+Handler\s*\(\s*([\w.:]+)|\.\s*Callback\s*=\s*([\w.:]+)", code):
        cb_names.add(next(g for g in m.groups() if g))

    def ctx_of(fn) -> str:
        if fn is None:
            return "chunk"
        n = fn.name
        mm = re.match(r"<anon@(GameEvents|Events|LuaEvents)\.(\w+)\.Add>", n)
        if mm:
            return f"{mm.group(1)}:{mm.group(2)}"
        if n in ev_by_name:
            return ev_by_name[n]
        if n in cb_names or "RegisterCallback" in n:
            return "callback"
        return "function"

    hits: list[Hit] = []

    def add(kind: str, what: str, off: int, cls: str = "") -> None:
        fn = enclosing(off)
        hits.append(Hit(kind, what, line_of(off), fn.name if fn else "", ctx_of(fn), cls))

    for m in re.finditer(r"\b(GameEvents|Events|LuaEvents)\.(\w+)\.(Add|Remove)\s*\(", code):
        add("gameevents" if m.group(1) == "GameEvents" else "events", f"{m.group(1)}.{m.group(2)}.{m.group(3)}", m.start())
    # calls, and references that are not calls (local aliases such as `local Network_SendResearch = Network.SendResearch`)
    for m in re.finditer(r"\bNetwork\.(\w+)", code):
        add("net", f"Network.{m.group(1)}", m.start())
    for m in re.finditer(r"\bGame\.(\w+)", code):
        n = m.group(1)
        if n in api["Game"] and _MUT_PREFIX.match(n) and n not in _NOT_SIM:
            add("game", f"Game.{n}", m.start())
    for m in re.finditer(r"\bMap\.(\w+)\s*\(", code):
        n = m.group(1)
        if (n in api["Map"] and _MUT_PREFIX.match(n) and n not in _NOT_SIM) or n == "Rand":
            add("game", f"Map.{n}", m.start())
    for m in re.finditer(r"\b(Game|Map)\.Rand\s*\(|\bmath\.random\s*\(", code):
        add("rand", m.group(0).rstrip("( "), m.start())
    allm = set().union(*(api[c] for c in _API_CLASSES))
    for m in re.finditer(r"([\w\]\)\.]+)\s*:\s*(\w+)\s*\(", code):
        n = m.group(2)
        if n in allm and _MUT_PREFIX.match(n) and n not in _NOT_SIM:
            cls = [c for c in _API_CLASSES if n in api[c]]
            add("mutate", f"{m.group(1).split('.')[-1][:24]}:{n}", m.start(), "/".join(cls))
    for m in re.finditer(r"OpenSaveData|\bDB\.Execute|\bDB\.Query\s*\(\s*[\"'](?:insert|update|delete|replace|create|drop|alter)", code, re.I):
        add("savedata" if "SaveData" in m.group(0) else "dbwrite", m.group(0), m.start())
    for m in re.finditer(r"\bPreGame\.(Set\w+)\s*\(", code):
        add("pregame", f"PreGame.{m.group(1)}", m.start())
    for m in re.finditer(r"\bUI\.(Request\w+|Send\w+|Do\w+|Set\w*(?:Option|Auto|Pause)\w*)\s*\(", code):
        add("net", f"UI.{m.group(1)}", m.start())
    inc = sorted({m.group(1) for m in _LUAINC.finditer(strip_lua_comments(raw))})
    return {"hits": hits, "includes": inc, "lines": raw.count("\n") + 1}


# ----------------------------------------------------------------------------------------------------------------
# classification of a differing file
# ----------------------------------------------------------------------------------------------------------------
SIM_AUTO, SIM_CLICK, UI_ONLY = "SIM-AUTO", "SIM-CLICK", "UI-ONLY"

# hand-checked notes (stem -> note), shown next to the automatic classification
MANUAL = {
    "notificationpanel": "EUI variant registers GameEvents.CityBoughtPlot (inside Events.SequenceGameInitComplete); the handler calls "
                         "g_activePlayer:AddNotification(NOTIFICATION_CITY_TILE) only when playerID == the local active player, so the "
                         "EUI owner's CvNotifications list (serialized in the player segment, lookup index and 100-slot ring) differs "
                         "from every other peer. The DLL adds no CITY_TILE notification itself. The SetPopulation handler of EUI's own "
                         "file is commented out in Lekmod's copy (commit defac827).",
    "cityview": "EUI variant: RemoveQueueItem (click on a queue item) adds NOTIFICATION_PRODUCTION to the local player only after "
                "Game.SelectedCitiesGameNetMessage(POP_ORDER); the standard variant does not. Click path, human only.",
    "citybannermanager": "EUI banner sends Network.SendUpdateCityCitizens when the mouse enters the player's own city banner (not a click; MP only while "
                         "the turn is not complete); the standard banner sends it only when the city screen opens. A synced command "
                         "(ResponseUpdateCityCitizens: DoVerifyWorkingPlots + DoReallocateCitizens on every peer), so it changes what is "
                         "simulated for that player, not whether peers agree.",
}
# differences the automatic comparison flags that were read and found to be dead or equivalent
DOWNGRADE = {
    "citystatediplopopup": "the extra Network.SendPledgeMinorProtection of the EUI file sits in the non-G&K branch (dead in BNW); every other command is identical",
}


def _cmd_set(facts: dict) -> set[str]:
    out = set()
    for h in facts["hits"]:
        if h.kind in ("net", "game", "savedata", "dbwrite", "pregame") and not _LOCAL_CALL.match(h.what):
            out.add(h.what)
        elif h.kind == "mutate" and h.cls != "Deal":
            out.add(h.what.split(":")[-1])
    return out


def _auto_state(facts: dict) -> list[Hit]:
    """mutating game-object calls that run without a click: chunk level or inside GameEvents handlers (scratch deals excluded)."""
    return [h for h in facts["hits"] if h.kind == "mutate" and h.cls != "Deal" and (h.ctx == "chunk" or h.ctx.startswith("GameEvents:"))]


def classify(stem_: str, facts_eui: dict | None, facts_ui: dict | None, size_eui: int = 0, size_ui: int = 0) -> tuple[str, str]:
    auto_e = _auto_state(facts_eui) if facts_eui else []
    auto_u = _auto_state(facts_ui) if facts_ui else []
    if {h.what for h in auto_e} != {h.what for h in auto_u}:
        return SIM_AUTO, MANUAL.get(stem_, "differs in automatic state-mutating calls: " + ", ".join(sorted({h.what for h in auto_e} ^ {h.what for h in auto_u})))
    ce = _cmd_set(facts_eui) if facts_eui else set()
    cu = _cmd_set(facts_ui) if facts_ui else set()
    if stem_ in DOWNGRADE:
        return UI_ONLY, DOWNGRADE[stem_]
    if stem_ not in MANUAL and 0 < size_eui < 500 <= size_ui:
        return UI_ONLY, "EUI replaces this screen by a stub (its actions are implemented in other EUI files)"
    if ce != cu or stem_ in MANUAL:
        only_e, only_u = sorted(ce - cu), sorted(cu - ce)
        parts = []
        if only_e:
            parts.append("only EUI: " + ", ".join(only_e))
        if only_u:
            parts.append("only standard: " + ", ".join(only_u))
        base = MANUAL.get(stem_, "; ".join(parts))
        return SIM_CLICK, base
    return UI_ONLY, ""


# ----------------------------------------------------------------------------------------------------------------
# analysis
# ----------------------------------------------------------------------------------------------------------------
def _clean_name(p: Path, prof: Profile | None = None) -> str:
    """File name as the game sees it: Lekmod files take the name of their Lua\\UI destination (CityBannerManager_1.lua.ignore ->
    CityBannerManager.lua)."""
    if prof is not None:
        for k, real in prof.lk_ui.items():
            if real == p:
                return Path(k).name
    return re.sub(r"(?i)\.ignore$", "", p.name)


def _side(prof: Profile, graph_ent: dict | None, ext: str) -> dict | None:
    p = graph_ent[ext] if graph_ent else None
    if p is None:
        return None
    layer = next((lay.name for lay, path in prof.win.values() if path == p), "?")
    return {"layer": layer, "file": str(p), "name": _clean_name(p, prof), "bytes": p.stat().st_size, "sha": sha(p)}


def _layer_of(win: dict, p: Path) -> str:
    for lay, path in win.values():
        if path == p:
            return lay.name
    return "?"


def analyse(game: Path, lekmod_name: str = LEKMOD_DEFAULT) -> dict:
    api = lua_api_names()
    eui = make_profile(game, lekmod_name, "eui", True)
    ui = make_profile(game, lekmod_name, "ui", False)
    inst_eui = make_profile(game, lekmod_name, "installer-eui", True, how="installer", stage=C.OUT / "eui-vs-ui" / "stage-installer" / "eui")
    inst_ui = make_profile(game, lekmod_name, "installer-ui", False, how="installer", stage=C.OUT / "eui-vs-ui" / "stage-installer" / "ui")
    inst_std_with_eui = make_profile(game, lekmod_name, "installer-standard-on-eui-machine", True, how="installer", installer_is_eui=False,
                                     stage=C.OUT / "eui-vs-ui" / "stage-installer" / "std-on-eui")

    res: dict = {"game": str(game), "lekmod": lekmod_name}
    # 1. file selection
    res["bat"] = {"eui_files": len(eui.copies), "ui_files": len(ui.copies),
                  "eui_only": sorted(set(eui.copies) - set(ui.copies)), "ui_only": sorted(set(ui.copies) - set(eui.copies)),
                  "same_dest_different_source": sorted(k for k in set(eui.copies) & set(ui.copies) if eui.copies[k]["src"] != ui.copies[k]["src"])}
    dlc = game / "Assets" / "DLC"
    inst_dir = dlc / lekmod_name / "Lua" / "UI"
    installed = {p.name.lower(): sha(p) for p in inst_dir.iterdir() if p.is_file()} if inst_dir.is_dir() else {}
    emul = {Path(k).name.lower(): sha(eui.lk_ui[k]) for k in eui.lk_ui}
    res["bat_emulation_matches_installed"] = {"installed_files": len(installed), "emulated_files": len(emul), "equal": installed == emul}

    def cmp_profiles(a: Profile, b: Profile) -> list[str]:
        out = []
        for k in sorted(set(a.win) | set(b.win)):
            if (k in a.win) != (k in b.win) or (k in a.win and sha(a.win[k][1]) != sha(b.win[k][1])):
                p, pr = (a.win[k][1], a) if k in a.win else (b.win[k][1], b)
                out.append(_clean_name(p, pr))
        return out

    res["installer_vs_bat"] = {
        "installer_eui_vs_bat_eui": cmp_profiles(inst_eui, eui),
        "installer_ui_vs_bat_ui": cmp_profiles(inst_ui, ui),
        "installer_standard_on_eui_machine_vs_bat_ui": cmp_profiles(inst_std_with_eui, ui),
        "installer_standard_on_eui_machine_vs_bat_eui": cmp_profiles(inst_std_with_eui, eui),
    }
    # 2. context graphs and differing files
    rows = []
    api_facts: dict[str, dict] = {}
    g_eui, g_ui = live_graph(eui.win, ["InGame"]), live_graph(ui.win, ["InGame"])
    f_eui, f_ui = live_graph(eui.win, ["FrontEnd"]), live_graph(ui.win, ["FrontEnd"])
    res["graph"] = {"ingame_eui": len(g_eui), "ingame_ui": len(g_ui), "frontend_eui": len(f_eui), "frontend_ui": len(f_ui)}

    def facts(p: Path | None) -> dict | None:
        if p is None or ".lua" not in p.name.lower():
            return None
        key = str(p)
        if key not in api_facts:
            api_facts[key] = lua_facts(p, api)
        return api_facts[key]

    seen = set()
    for scope, ga, gb in (("in-game", g_eui, g_ui), ("front-end", f_eui, f_ui)):
        for st in sorted(set(ga) | set(gb)):
            for ext in ("lua", "xml"):
                pa = ga.get(st, {}).get(ext) if st in ga else None
                pb = gb.get(st, {}).get(ext) if st in gb else None
                if (pa is None) == (pb is None) and (pa is None or sha(pa) == sha(pb)):
                    continue
                key = (st, ext)
                if key in seen:
                    for r in rows:
                        if (r["stem"], r["ext"]) == key and r["scope"] != scope:
                            r["scope"] = "both"
                    continue
                seen.add(key)
                row = {"stem": st, "ext": ext, "scope": scope,
                       "eui": _side(eui, ga.get(st), ext) if pa else None, "ui": _side(ui, gb.get(st), ext) if pb else None}
                if ext == "lua":
                    fe, fu = facts(pa), facts(pb)
                    cl, note = classify(st, fe, fu, row["eui"]["bytes"] if row["eui"] else 0, row["ui"]["bytes"] if row["ui"] else 0)
                    row["class"], row["note"] = cl, note
                    row["includes_eui"] = fe["includes"] if fe else None
                    row["includes_ui"] = fu["includes"] if fu else None
                    row["hits_eui"] = [h.__dict__ for h in fe["hits"] if h.kind != "events"] if fe else []
                    row["hits_ui"] = [h.__dict__ for h in fu["hits"] if h.kind != "events"] if fu else []
                else:
                    row["class"], row["note"] = UI_ONLY, "UI layout (XML)"
                rows.append(row)
    res["rows"] = rows
    res["counts"] = {c: sum(1 for r in rows if r["ext"] == "lua" and r["class"] == c) for c in (SIM_AUTO, SIM_CLICK, UI_ONLY)}
    res["counts"]["xml"] = sum(1 for r in rows if r["ext"] == "xml")

    # 3. the gameplay closure: contexts InGame.lua loads (plus their includes) resolve to the same bytes in both profiles
    ingame_lua = (ui.lk_ui["InGame.lua"]) if "InGame.lua" in ui.lk_ui else None
    ctx_names = [m.group(1) for m in _LOADCTX.finditer(strip_lua_comments(read_text(ingame_lua)))] if ingame_lua else []
    gcl_eui, gcl_ui = live_graph(eui.win, ctx_names), live_graph(ui.win, ctx_names)
    differing = []
    for st in sorted(set(gcl_eui) | set(gcl_ui)):
        pa, pb = gcl_eui.get(st, {}).get("lua"), gcl_ui.get(st, {}).get("lua")
        if (pa is None) != (pb is None) or (pa and sha(pa) != sha(pb)):
            differing.append(st)
    eui_names = {p.name.lower() for p in (dlc / "UI_bc1").rglob("*") if p.is_file()} if (dlc / "UI_bc1").is_dir() else set()
    unresolved = sorted(st for st in set(gcl_eui) | set(gcl_ui) if gcl_ui.get(st, {}).get("lua") is None and gcl_eui.get(st, {}).get("lua") is None)
    res["gameplay_closure"] = {"contexts_from_InGame_lua": len(ctx_names), "closure_size": len(set(gcl_eui) | set(gcl_ui)), "differing": differing,
                               "unresolved_in_ui_skin": unresolved,
                               "unresolved_names_shipped_by_eui": [st for st in unresolved if (st + ".lua") in eui_names or (st + ".xml") in eui_names],
                               "ingame_lua_same": (sha(eui.lk_ui["InGame.lua"]) == sha(ui.lk_ui["InGame.lua"])) if "InGame.lua" in eui.lk_ui and "InGame.lua" in ui.lk_ui else None,
                               "contexts": ctx_names,
                               "winner_layers": {st: _layer_of(ui.win, gcl_ui[st]["lua"]) for st in sorted(gcl_ui) if gcl_ui[st]["lua"]}}
    # 4. gameplay DB: does EUI ship anything the exe would load into the gameplay or text DB?
    eui_dir = dlc / "UI_bc1"
    pk = {p.name: read_text(p) for p in sorted(eui_dir.glob("*.Civ5Pkg"))} if eui_dir.is_dir() else {}
    xml_roots: dict[str, int] = {}
    n_sql = n_game = 0
    if eui_dir.is_dir():
        for p in eui_dir.rglob("*"):
            if p.is_file() and p.suffix.lower() == ".sql":
                n_sql += 1
            if p.is_file() and p.suffix.lower() == ".xml":
                head = re.sub(r"<\?xml[^>]*\?>|<!--.*?-->", "", read_text(p)[:4000], flags=re.S)
                m = re.search(r"<([A-Za-z_][\w]*)", head)
                root = m.group(1) if m else "?"
                xml_roots[root] = xml_roots.get(root, 0) + 1
                n_game += 1 if root == "GameData" else 0
    res["eui_db"] = {"packages": {n: {"tags": sorted(set(re.findall(r"<(Gameplay|GameplaySkin|UISkin|TextData|GameData|Audio\w*|Language_\w+)\b", t)))} for n, t in pk.items()},
                     "priority": {n: re.findall(r"<Priority>(\d+)</Priority>", t) for n, t in pk.items()},
                     "sql_files": n_sql, "xml_root_elements": xml_roots, "gamedata_xml": n_game,
                     "file_types": {}}
    ft: dict[str, int] = {}
    if eui_dir.is_dir():
        for p in eui_dir.rglob("*"):
            if p.is_file():
                ft[p.suffix.lower()] = ft.get(p.suffix.lower(), 0) + 1
    res["eui_db"]["file_types"] = ft
    txt = dlc / "EUI_text_en_us.xml"
    res["eui_db"]["text_xml"] = {"file": str(txt), "exists": txt.is_file(),
                                 "tables": sorted(set(re.findall(r"<(Language_\w+)>", read_text(txt)))) if txt.is_file() else []}
    return res


# ----------------------------------------------------------------------------------------------------------------
# reference document
# ----------------------------------------------------------------------------------------------------------------
def _kb(side: dict | None) -> str:
    return f"{side['layer'].split(' (')[0]}, {side['bytes']} B" if side else "-"


def render_reference(res: dict) -> str:
    rows = [r for r in res["rows"] if r["ext"] == "lua"]
    xml = [r for r in res["rows"] if r["ext"] == "xml"]
    gc = res["gameplay_closure"]
    L: list[str] = []
    L.append("# Profiles: EUI vs standard UI (`lekmod-v34.15-eui` / `lekmod-v34.15-ui`)")
    L.append("")
    L.append("Generated by `python -m civsim.detlab.euiui diff --reference` . Do not edit by hand; "
             "analysis: see the EUI-vs-UI classification in civsim.detlab.euiui.")
    L.append("")
    L.append(f"Game: `{res['game']}`, Lekmod `{res['lekmod']}`, EUI `UI_bc1` v1.28g. A profile = the UI file set a player runs: "
             "`Lua\\tmp\\ui` or `Lua\\tmp\\eui` copied into `Lua\\UI` by `ui_check.bat`, on top of the base game, Expansion, Expansion2 and (EUI "
             "profile) the EUI package (priority 250); Lekmod (300) wins every name it supplies.")
    L.append("")
    L.append("## Summary")
    L.append("")
    c = res["counts"]
    L.append(f"- Lua file names that resolve to different content in the two profiles (live in the in-game or front-end context graph): "
             f"**{c[SIM_AUTO] + c[SIM_CLICK] + c[UI_ONLY]}** (+ {c['xml']} XML layouts).")
    L.append(f"- Classified **{SIM_AUTO}** (can change synchronous/serialized state without a click): **{c[SIM_AUTO]}**.")
    L.append(f"- Classified **{SIM_CLICK}** (differs only in what a user click sends or does locally): **{c[SIM_CLICK]}**.")
    L.append(f"- Classified {UI_ONLY}: {c[UI_ONLY]}.")
    L.append(f"- Gameplay closure: the {gc['contexts_from_InGame_lua']} contexts that `InGame.lua` loads plus their includes ({gc['closure_size']} names) resolve "
             f"to **{'identical' if not gc['differing'] else 'DIFFERENT'} content** in both profiles"
             f"{'' if not gc['differing'] else ': ' + ', '.join(gc['differing'])}; `InGame.lua` itself is "
             f"{'the same file' if gc['ingame_lua_same'] else 'different'} (always `tmp\\ui\\InGame.lua.ignore`).")
    db = res["eui_db"]
    L.append(f"- Gameplay DB: EUI ships {db['sql_files']} SQL files and {db['gamedata_xml']} `<GameData>` XML files; its three `.Civ5Pkg` declare only "
             f"`UISkin` (tags: {', '.join(sorted({t for v in db['packages'].values() for t in v['tags']}))}). No table or row differs between the profiles.")
    L.append("")
    L.append("## Lua files whose content differs")
    L.append("")
    L.append("Winner = the file the UI skin lookup returns (highest package priority). `Lekmod` = a file ui_check copied from `Lua\\tmp`; "
             "`EUI` = `UI_bc1` file that Lekmod does not override; `-` = not present in that profile.")
    L.append("")
    L.append("| class | file | EUI profile | standard profile | scope | note |")
    L.append("|---|---|---|---|---|---|")
    order = {SIM_AUTO: 0, SIM_CLICK: 1, UI_ONLY: 2}
    for r in sorted(rows, key=lambda r: (order[r["class"]], r["stem"])):
        name = (r["eui"] or r["ui"])["name"].split(".ignore")[0].split(".IGNORE")[0]
        L.append(f"| {r['class']} | `{name}` | {_kb(r['eui'])} | {_kb(r['ui'])} | {r['scope']} | {r['note'].replace('|', '/')} |")
    L.append("")
    L.append("## XML layouts that differ (UI only)")
    L.append("")
    L.append(", ".join(f"`{(r['eui'] or r['ui'])['name'].split('.ignore')[0].split('.IGNORE')[0]}`" for r in sorted(xml, key=lambda r: r["stem"])))
    L.append("")
    L.append("## How the profiles are produced")
    L.append("")
    b = res["bat"]
    L.append(f"`ui_check.bat` (emulated, reproduces the installed `Lua\\UI` of this machine byte for byte: "
             f"{'yes' if res['bat_emulation_matches_installed']['equal'] else 'NO'}, {res['bat_emulation_matches_installed']['installed_files']} files) copies "
             f"{b['eui_files']} files with EUI and {b['ui_files']} without. Always from `tmp\\ui` in both cases: `InGame.lua`, `prophetreplace.lua`, all lobby/"
             "staging scripts, MiniMapPanel, MPTurnPanel, VotingSystem popups, Replays, GPList, UnitList, EnemyUnitPanel, CultureOverview, Highlights.xml, ...")
    L.append("")
    L.append(f"- Only with EUI: {', '.join('`' + x + '`' for x in b['eui_only'])}.")
    L.append(f"- Only without EUI: {', '.join('`' + x + '`' for x in b['ui_only'])}.")
    L.append(f"- Same destination, different source: {', '.join('`' + x + '`' for x in b['same_dest_different_source'])}.")
    L.append("")
    iv = res["installer_vs_bat"]
    L.append("`LekmodInstaller\\ui_manager.py` (`configure_ui_files`) is a port of the .bat but not identical (it takes the EUI/standard choice from its "
             "dialog, not from the files on disk, and always uses `CityBannerManager_2`). Names that resolve to different content than the .bat result:")
    L.append("")
    for k, v in iv.items():
        if len(v) > 12:
            L.append(f"- {k.replace('_', ' ')}: {len(v)} names (EUI files and Lekmod's standard files mixed; see analysis.json)")
        else:
            L.append(f"- {k.replace('_', ' ')}: {', '.join('`' + x + '`' for x in v) if v else 'none'}")
    L.append("")
    return "\n".join(L) + "\n"


# ----------------------------------------------------------------------------------------------------------------
# staging for civhost (--lua-dlc), shims, harness bot
# ----------------------------------------------------------------------------------------------------------------
BOT_LUA = """-- civsim.detlab.euiui harness bot (identical on every peer, so every action here is synced like a command all peers execute).
-- Under civhost a human slot is never turn-activated after turn 0, so its units and cities
-- would idle. The bot stands in for the human's turn: (1) found the capital at the settler's plot once the game has started,
-- (2) on EUIVSUI_LEVEL_TURN make the human's city complete a culture level, exactly what CvCity::doTurn does when the stored
-- culture reaches the threshold: CvCity::DoJONSCultureLevelIncrease -> acquire the next plot -> GameEvents.CityBoughtPlot
-- (isWithGold false, isWithFaithOrCulture true) on every peer.
local EUIVSUI_LEVEL_TURN = 5
local g_leveled = false
local function EuiVsUiBot_Humans(fn)
	for iPlayer = 0, 21 do
		local pPlayer = Players[iPlayer]
		if pPlayer and pPlayer:IsAlive() and pPlayer:IsHuman() then fn(iPlayer, pPlayer) end
	end
end
Events.SequenceGameInitComplete.Add(function()
	EuiVsUiBot_Humans(function(iPlayer, pPlayer)
		if pPlayer:GetNumCities() > 0 then return end
		for pUnit in pPlayer:Units() do
			if pUnit:IsFound() then
				local x, y = pUnit:GetX(), pUnit:GetY()
				pPlayer:InitCity(x, y)
				pUnit:Kill(true, -1)
				print("EuiVsUiBot: founded the capital of player " .. tostring(iPlayer) .. " at " .. x .. "," .. y .. " on turn " .. tostring(Game.GetGameTurn()))
				return
			end
		end
		print("EuiVsUiBot: player " .. tostring(iPlayer) .. " has no settler")
	end)
end)
GameEvents.PlayerDoTurn.Add(function(iPlayer)
	if g_leveled or Game.GetGameTurn() < EUIVSUI_LEVEL_TURN then return end
	g_leveled = true
	EuiVsUiBot_Humans(function(iHuman, pPlayer)
		for pCity in pPlayer:Cities() do
			pCity:ChangeJONSCultureStored(pCity:GetJONSCultureThreshold())
			pCity:DoJONSCultureLevelIncrease()
			print("EuiVsUiBot: culture level of " .. pCity:GetName() .. " (player " .. tostring(iHuman) .. ") increased on turn " .. tostring(Game.GetGameTurn()) ..
				", notifications of the player now " .. tostring(pPlayer:GetNumNotifications()))
			break
		end
	end)
end)
"""


def extract_call_block(raw: str, start_pat: str) -> str | None:
    """Verbatim text of the statement `start_pat ... )` (balanced parentheses, strings and comments ignored) or None."""
    from . import scan
    code = scan.strip_lua(raw)
    m = re.search(start_pat, code)
    if not m:
        return None
    i = code.index("(", m.start())
    depth = 0
    for j in range(i, len(code)):
        if code[j] == "(":
            depth += 1
        elif code[j] == ")":
            depth -= 1
            if depth == 0:
                return raw[m.start():j + 1]
    return None


def make_notification_shim(np_path: Path) -> str | None:
    """The GameEvents.CityBoughtPlot handler of an EUI NotificationPanel.lua, extracted verbatim (the whole panel needs UI engine
    globals civhost does not provide and stops at its first include). Registered inside Events.SequenceGameInitComplete, like the file."""
    raw = read_text(np_path).replace("\r\n", "\n")
    block = extract_call_block(raw, r"\bGameEvents\.CityBoughtPlot\.Add\s*\(")
    if block is None:
        return None
    ups = []
    for pat in (r"^local L = Locale\.ConvertTextKey\s*$", r"^local Map_GetPlot = Map\.GetPlot\s*$", r"^local g_activePlayerID = .*$",
                r"^local g_activePlayer = .*$", r"^local g_activeTeamID = .*$"):
        m = re.search(pat, raw, re.M)
        if not m:
            return None
        ups.append(m.group(0))
    body = "\n".join(ups) + "\n\n" + block
    body = "\n".join("\t" + ln if ln else ln for ln in body.split("\n"))
    return ("-- civsim.detlab.euiui: GameEvents.CityBoughtPlot handler of " + np_path.name + " extracted verbatim (upvalue lines and the call).\n"
            "-- The panel itself cannot run in civhost (missing UI engine globals). Registered at the same moment as in the file.\n"
            "Events.SequenceGameInitComplete.Add(function()\n" + body + "\nend)\n")


def stage_profile(game: Path, lekmod_name: str, label: str, eui_present: bool, out: Path, ui_contexts: bool = False,
                  shim_notification: bool = False, bot: bool = False, extra_contexts: list[str] | None = None) -> dict:
    """Copy of the Lekmod `Lua` folder as a player of this profile has it after ui_check.bat, for civhost (`--lua-dlc <stage>`):
    Lua\\UI = the .bat's output, EUI scripts that win the name lookup next to them, and InGame.lua extended with one
    `ContextPtr:LoadNewContext(...)` per extra context (the host reads the context list from InGame.lua):
      ui_contexts        every UI context the real InGame.xml instantiates (the host runs those that survive its UI stubs),
      shim_notification  the EUI NotificationPanel CityBoughtPlot handler as a context of its own (only if this profile's panel has it),
      bot                the harness city-founding bot for human slots."""
    src = game / "Assets" / "DLC" / lekmod_name
    dst = out / "stage" / label / lekmod_name
    if dst.exists():
        shutil.rmtree(dst)
    (dst / "Lua").mkdir(parents=True)
    for p in sorted((src / "Lua").iterdir()):
        if p.name.lower() in ("tmp", "ui"):
            continue
        if p.is_dir():
            shutil.copytree(p, dst / "Lua" / p.name)
        else:
            shutil.copy2(p, dst / "Lua" / p.name)
    prof = make_profile(game, lekmod_name, label, eui_present)
    (dst / "Lua" / "UI").mkdir()
    for dest_name, real in prof.lk_ui.items():
        tgt = dst / "Lua" / "UI" / dest_name
        tgt.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(real, tgt)
    graph = live_graph(prof.win, ["InGame"])
    for st, ent in graph.items():
        for ext in ("lua", "xml"):
            p = ent[ext]
            if p is not None and "UI_bc1" in str(p):
                shutil.copy2(p, dst / "Lua" / "UI" / p.name)
    ingame = dst / "Lua" / "UI" / "InGame.lua"
    text = read_text(ingame)
    have = {stem(m.group(1)) for m in _LOADCTX.finditer(strip_lua_comments(text))}
    added: list[str] = []
    if ui_contexts:
        added += [st for st, ent in sorted(graph.items()) if "ctx" in ent["kinds"] and ent["lua"] is not None and st not in have and st != "ingame"]
    shim_ok = False
    if shim_notification:
        np_ent = graph.get("notificationpanel")
        shim = make_notification_shim(np_ent["lua"]) if np_ent and np_ent["lua"] else None
        if shim:
            (dst / "Lua" / "EuiVsUiNotificationShim.lua").write_text(shim, encoding="latin-1", newline="")
            added.append("EuiVsUiNotificationShim")
            shim_ok = True
    if bot:
        (dst / "Lua" / "EuiVsUiBot.lua").write_text(BOT_LUA, encoding="latin-1", newline="")
        added.append("EuiVsUiBot")
    added += list(extra_contexts or [])
    if added:
        text += "\r\n-- civsim.detlab.euiui: extra contexts for this profile\r\n" + "".join(f'ContextPtr:LoadNewContext("{n}")\r\n' for n in added)
        ingame.write_text(text, encoding="latin-1", newline="")
    return {"dlc": dst, "added_contexts": added, "graph_size": len(graph), "bat_files": len(prof.copies), "shim": shim_ok}


# ----------------------------------------------------------------------------------------------------------------
# civhost runs
# ----------------------------------------------------------------------------------------------------------------
def default_host() -> Path:
    p = C.OUT / "host-eui" / "civhost.exe"
    return p if p.is_file() else C.resolve_host(None).path


def host_cmd(exe: Path, dll: Path, scenario: Path, out: Path, turns: int, dlc: Path | None, data: Path | None = None) -> list[str]:
    data = data or sorted((C.OUT / "oracle").glob("cache-*"))[-1]
    cmd = [str(exe), "--dll", str(dll), "--data", str(data), "--scenario", str(scenario), "--turns", str(turns), "--out", str(out)]
    if dlc is not None:
        cmd += ["--lua-dlc", str(dlc)]
    return cmd


def run_many(cmds: dict[str, tuple[list[str], Path]], jobs: int = 3, log=print) -> dict[str, dict]:
    """Run civhost command lines with limited parallelism; returns {name: {exit, secs}}."""
    pending = list(cmds.items())
    running: dict[str, tuple[subprocess.Popen, float]] = {}
    done: dict[str, dict] = {}
    while pending or running:
        while pending and len(running) < jobs:
            name, (cmd, out) = pending.pop(0)
            if out.exists():
                shutil.rmtree(out, ignore_errors=True)
            out.mkdir(parents=True, exist_ok=True)
            p = subprocess.Popen(cmd, stdout=open(out / "stdout.txt", "wb"), stderr=subprocess.STDOUT, cwd=str(C.REPO))
            C.attach_process(p)
            running[name] = (p, time.time())
            log(f"  started {name}")
        for name in list(running):
            p, t0 = running[name]
            if p.poll() is not None:
                done[name] = {"exit": p.returncode, "secs": round(time.time() - t0, 1)}
                log(f"  finished {name}: exit {p.returncode} in {done[name]['secs']} s")
                del running[name]
        time.sleep(0.5)
    return done


def digest_compare(a: Path, b: Path) -> dict:
    from .. import digestbin as D
    c = D.compare(D.parse(a), D.parse(b))
    return {k: c[k] for k in ("equal", "common_turns", "first_divergent_turn", "divergent_segments", "n_divergent_turns")}


def cmd_ai(a) -> int:
    game, out = Path(a.game), Path(a.out)
    exe = Path(a.host) if a.host else default_host()
    dll = C.resolve_dll(a.dll).path
    scenario = C.REPO / a.scenario
    variants = {
        "base-ui": dict(eui=False),                                            # the 37 gameplay contexts only
        "base-eui": dict(eui=True),
        "full-ui": dict(eui=False, ui_contexts=True),                          # + every UI context the host manages to load
        "full-eui": dict(eui=True, ui_contexts=True),
        "full-eui-handler": dict(eui=True, ui_contexts=True, shim_notification=True),    # + the EUI notification handler, registered
    }
    cmds, stages = {}, {}
    for name, kw in variants.items():
        st = stage_profile(game, a.lekmod, name, kw["eui"], out, ui_contexts=kw.get("ui_contexts", False), shim_notification=kw.get("shim_notification", False))
        stages[name] = st
        cmds[name] = (host_cmd(exe, dll, scenario, out / "runs" / "ai" / name, a.turns, st["dlc"]), out / "runs" / "ai" / name)
    res = run_many(cmds, jobs=a.jobs)
    summary = {"turns": a.turns, "scenario": a.scenario, "dll": str(dll), "host": str(exe), "runs": {}}
    base = out / "runs" / "ai" / "base-ui" / "digest.bin"
    for name in variants:
        d = out / "runs" / "ai" / name / "digest.bin"
        ent = {"exit": res[name]["exit"], "secs": res[name]["secs"], "added_contexts": len(stages[name]["added_contexts"]), "shim": stages[name]["shim"]}
        rj = out / "runs" / "ai" / name / "run.json"
        if rj.is_file():
            j = json.loads(rj.read_text(encoding="utf-8"))
            ent.update({k: j.get(k) for k in ("lua_contexts", "lua_contexts_loaded", "lua_errors", "turns_completed", "end_turn") if k in j})
        if d.is_file():
            ent["sha256"] = C.file_sha256(d)[:16]
            ent["vs_base_ui"] = digest_compare(base, d) if base.is_file() else None
        summary["runs"][name] = ent
    C.write_json(out / "ai-only.json", summary)
    for name, e in summary["runs"].items():
        print(f"{name:18} exit {e['exit']} sha {e.get('sha256')} loaded {e.get('lua_contexts_loaded')}/{e.get('lua_contexts')} errors {e.get('lua_errors')} "
              f"vs base-ui: {'equal' if (e.get('vs_base_ui') or {}).get('equal') else e.get('vs_base_ui')}")
    return 0


# ----------------------------------------------------------------------------------------------------------------
# mixed lockstep session (human slot)
# ----------------------------------------------------------------------------------------------------------------
HUMAN_SCENARIO = "headless/scenarios/eui/lekmod6-human0.json"


def write_human_scenario() -> Path:
    base = C.read_json(C.REPO / "headless" / "scenarios" / "smoke" / "lekmod6.json")
    base["name"] = "lekmod6-human0"
    base["game_type"] = "network"
    base["game_options_preset"] = "lekmod-mp"
    base["civs"][0]["human"] = True
    p = C.REPO / HUMAN_SCENARIO
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(base, indent=2) + "\n", encoding="utf-8")
    return p


def cmd_mixed(a) -> int:
    import copy
    from ..lockstep import coord
    from ..lockstep import __main__ as lsmain
    game, out = Path(a.game), Path(a.out)
    exe = Path(a.host) if a.host else default_host()
    data = sorted((C.OUT / "oracle").glob("cache-*"))[-1]
    write_human_scenario()
    stg = {
        "plain": stage_profile(game, a.lekmod, "ls-plain", False, out, bot=True),
        "shim": stage_profile(game, a.lekmod, "ls-shim", True, out, shim_notification=True, bot=True),
    }
    ref = C.read_json(C.REPO / "headless" / "scenarios" / "lockstep" / "italy-human-ghyg.json")      # masks follow the current DLL build
    sessions = {
        "control": ["plain", "plain", "plain"],              # nobody runs the EUI handler
        "eui-owner": ["shim", "plain", "plain"],             # the human's own machine has EUI, the others do not
        "all-eui": ["shim", "shim", "shim"],                 # everybody has EUI
    }
    orig_peer = coord.Peer
    results = {}
    for sname, kinds in sessions.items():
        def patched(pid, player, d, exe_, dll_, extra, _kinds=kinds):
            # the private host copy sits outside headless\out\host\Release, so its repo-relative defaults (data profile) must be explicit
            return orig_peer(pid, player, d, exe_, dll_, list(extra or []) + ["--data", str(data), "--lua-dlc", str(stg[_kinds[pid]]["dlc"])])
        coord.Peer = patched
        sess = {"name": f"eui-{sname}", "scenario": HUMAN_SCENARIO, "peers": [0, 1, 2], "until_turn": a.turns, "dll": a.dll, "mask": ref.get("mask", []),
                "bots": [{"peer": 0, "auto_end_turn": True}], "single": False, "stop_on_divergence": False}
        sp = out / f"session-{sname}.json"
        sp.parent.mkdir(parents=True, exist_ok=True)
        sp.write_text(json.dumps(sess, indent=2), encoding="utf-8")
        sdir = C.OUT / "sessions" / f"eui-{sname}"
        rc = lsmain.main(["run", str(sp), "--host", str(exe), "--out", str(sdir), "--no-single", "--keep-going", "--no-localize"])
        r = C.read_json(sdir / "result.json") if (sdir / "result.json").is_file() else {}
        results[sname] = {"exit": rc, "status": r.get("status"), "turns": r.get("turns"), "divergences": [d["turn"] for d in r.get("divergences", [])][:50],
                          "n_divergent_turns": len(r.get("divergences", [])), "lua_errors": r.get("lua_errors"), "dir": str(sdir)}
        # per-peer digest comparison (all segments, unmasked) and masked flag
        dg = {}
        for i in (1, 2):
            pa, pb = sdir / "peer-0" / "digest.bin", sdir / f"peer-{i}" / "digest.bin"
            if pa.is_file() and pb.is_file():
                dg[f"peer0_vs_peer{i}_unmasked"] = digest_compare(pa, pb)
        pa, pb = sdir / "peer-1" / "digest.bin", sdir / "peer-2" / "digest.bin"
        if pa.is_file() and pb.is_file():
            dg["peer1_vs_peer2_unmasked"] = digest_compare(pa, pb)
        results[sname]["unmasked"] = dg
    coord.Peer = orig_peer
    C.write_json(out / "mixed.json", {"turns": a.turns, "sessions": results})
    for k, v in results.items():
        print(k, json.dumps({x: v[x] for x in ("status", "n_divergent_turns", "divergences")}, default=str)[:300])
    return 0


MIGHT_PROBE_LUA = """-- what a UI script that asks every player for its military might does (Demographics' BuildTables, the city-state popups): Player:GetMilitaryMight()
-- recomputes and stores m_iMilitaryMight / m_iEconomicMight / m_iTurnMightRecomputed (FAutoVariables) on the first call of a turn.
-- Here: once, at the first PlayerDoTurn of MIGHT_TURN, on the peers that carry this context only.
local MIGHT_TURN = 10
local g_done = false
GameEvents.PlayerDoTurn.Add(function(iPlayer)
	if g_done or Game.GetGameTurn() ~= MIGHT_TURN then return end
	g_done = true
	for i = 0, 33 do
		local p = Players[i]
		if p then pcall(function() p:GetMilitaryMight() end) end
	end
	print("MightProbe: asked all players for their military might at turn " .. tostring(MIGHT_TURN) .. ", first PlayerDoTurn was player " .. tostring(iPlayer))
end)
"""


def cmd_might(a) -> int:
    """Side experiment: one peer calls Player:GetMilitaryMight() for every player mid-game (what the Demographics screen does); the other two do not."""
    from ..lockstep import coord
    from ..lockstep import __main__ as lsmain
    from .. import digestbin as D
    game, out = Path(a.game), Path(a.out)
    exe = Path(a.host) if a.host else default_host()
    data = sorted((C.OUT / "oracle").glob("cache-*"))[-1]
    write_human_scenario()
    ref = C.read_json(C.REPO / "headless" / "scenarios" / "lockstep" / "italy-human-ghyg.json")
    plain = stage_profile(game, a.lekmod, "ls-might-plain", False, out, bot=True)
    probe = stage_profile(game, a.lekmod, "ls-might", False, out, bot=True, extra_contexts=["EuiVsUiMightProbe"])
    (probe["dlc"] / "Lua" / "EuiVsUiMightProbe.lua").write_text(MIGHT_PROBE_LUA, encoding="latin-1", newline="")
    stg = {"plain": plain, "probe": probe}
    sessions = {"might-control": ["plain", "plain", "plain"], "might-peer0": ["probe", "plain", "plain"]}
    orig_peer = coord.Peer
    for sname, kinds in sessions.items():
        def patched(pid, player, d, exe_, dll_, extra, _k=kinds):
            return orig_peer(pid, player, d, exe_, dll_, list(extra or []) + ["--data", str(data), "--lua-dlc", str(stg[_k[pid]]["dlc"])])
        coord.Peer = patched
        sess = {"name": sname, "scenario": HUMAN_SCENARIO, "peers": [0, 1, 2], "until_turn": a.turns, "dll": a.dll, "mask": ref.get("mask", []),
                "bots": [{"peer": 0, "auto_end_turn": True}], "single": False, "stop_on_divergence": False}
        sp = out / f"session-{sname}.json"
        sp.write_text(json.dumps(sess, indent=2), encoding="utf-8")
        lsmain.main(["run", str(sp), "--host", str(exe), "--out", str(C.OUT / "sessions" / sname), "--no-single", "--keep-going", "--no-localize"])
    coord.Peer = orig_peer
    S = C.OUT / "sessions"
    res = {}
    for peer in (0, 1, 2):
        c = D.compare(D.parse(S / "might-control" / f"peer-{peer}" / "digest.bin"), D.parse(S / "might-peer0" / f"peer-{peer}" / "digest.bin"))
        res[f"peer{peer}"] = {k: c[k] for k in ("equal", "first_divergent_turn", "divergent_segments", "n_divergent_turns")}
        print(f"peer {peer}: control vs probe: {'equal' if c['equal'] else 'differs from turn %s in %d segment(s): %s' % (c['first_divergent_turn'], len(c['divergent_segments']), ', '.join(c['divergent_segments']))}")
    C.write_json(out / "might.json", {"turns": a.turns, "peers": res})
    return 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(prog="python -m civsim.detlab.euiui")
    ap.add_argument("cmd", choices=["diff", "ai", "mixed", "might"])
    ap.add_argument("--game", default=str(GAME_DEFAULT))
    ap.add_argument("--lekmod", default=LEKMOD_DEFAULT)
    ap.add_argument("--out", default=str(C.OUT / "eui-vs-ui"))
    ap.add_argument("--reference", action="store_true", help="diff: also write headless/reference/profiles-eui-vs-ui.md")
    ap.add_argument("--host", default=None, help="civhost.exe (default headless/out/host-eui/civhost.exe, a private copy)")
    ap.add_argument("--dll", default="golden-hyg")
    ap.add_argument("--scenario", default="headless/scenarios/smoke/lekmod6.json")
    ap.add_argument("--turns", type=int, default=None)
    ap.add_argument("--jobs", type=int, default=3)
    a = ap.parse_args(argv)
    Path(a.out).mkdir(parents=True, exist_ok=True)
    if a.cmd == "diff":
        res = analyse(Path(a.game), a.lekmod)
        C.write_json(Path(a.out) / "analysis.json", res)
        c = res["counts"]
        print(f"differing Lua names: {c[SIM_AUTO] + c[SIM_CLICK] + c[UI_ONLY]} ({SIM_AUTO} {c[SIM_AUTO]}, {SIM_CLICK} {c[SIM_CLICK]}, {UI_ONLY} {c[UI_ONLY]}), XML {c['xml']}")
        print("gameplay closure differing:", res["gameplay_closure"]["differing"] or "none")
        print("bat emulation == installed Lua\\UI:", res["bat_emulation_matches_installed"]["equal"])
        if a.reference:
            p = C.REPO / "headless" / "reference" / "profiles-eui-vs-ui.md"
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(render_reference(res), encoding="utf-8")
            print("wrote", p)
        return 0
    if a.turns is None:
        a.turns = {"ai": 50, "might": 20}.get(a.cmd, 60)
    return {"ai": cmd_ai, "mixed": cmd_mixed, "might": cmd_might}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
