"""Scenario files (the civhost JSON format), corpora (cartesian expansion over a base scenario), validation and sim keys.

Scenario = the host's own format (see headless/scenarios/smoke/pangaea6.json) plus runner fields:
  note      free text, not part of the key
  turns     number of turns to simulate (CLI --turns overrides)
  env       perturbation axes for determinism experiments (heap_pattern, ...), part of the key
  outputs   reserved (not part of the key)
Corpus = {"schema":1,"corpus":name,"groups":[{"base":<path or inline scenario>,"set":{dotted:value},"matrix":{dotted:[values]}}]}
(top-level "base"/"set"/"matrix" are a single implicit group). Paths are relative to the corpus file.
Synthetic matrix axes: "seed" (map seed = v, sync seed derived), "civs.count" (first N of the base civ list, extended from a
default pool), "minors.count" (same).
"""
from __future__ import annotations

import copy
import itertools
import re
import sqlite3
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .. import common as C

SIM_EXCLUDE = {"name", "note", "dll", "outputs"}
KNOWN = {"name", "note", "dll", "outputs", "turns", "env", "seeds", "game_type", "world_size", "map_script", "map_options",
         "climate", "sea_level", "era", "game_speed", "handicap", "max_turns", "active_player", "civs", "minors", "game_options",
         "game_options_preset"}
KEY_VERSION = b"simrunner-key-v1\0"

# Names verified against the BNW + Lekmod gameplay DB (Civilization_Leaders pairs).
DEFAULT_CIVS = [
    ("CIVILIZATION_ROME", "LEADER_AUGUSTUS"), ("CIVILIZATION_GREECE", "LEADER_ALEXANDER"), ("CIVILIZATION_EGYPT", "LEADER_RAMESSES"),
    ("CIVILIZATION_CHINA", "LEADER_WU_ZETIAN"), ("CIVILIZATION_GERMANY", "LEADER_BISMARCK"), ("CIVILIZATION_JAPAN", "LEADER_ODA_NOBUNAGA"),
    ("CIVILIZATION_AMERICA", "LEADER_WASHINGTON"), ("CIVILIZATION_ENGLAND", "LEADER_ELIZABETH"), ("CIVILIZATION_FRANCE", "LEADER_NAPOLEON"),
    ("CIVILIZATION_RUSSIA", "LEADER_CATHERINE"), ("CIVILIZATION_INDIA", "LEADER_GANDHI"), ("CIVILIZATION_ARABIA", "LEADER_HARUN_AL_RASHID"),
    ("CIVILIZATION_PERSIA", "LEADER_DARIUS"), ("CIVILIZATION_OTTOMAN", "LEADER_SULEIMAN"), ("CIVILIZATION_AZTEC", "LEADER_MONTEZUMA"),
    ("CIVILIZATION_SONGHAI", "LEADER_ASKIA"), ("CIVILIZATION_SIAM", "LEADER_RAMKHAMHAENG"), ("CIVILIZATION_MONGOL", "LEADER_GENGHIS_KHAN"),
    ("CIVILIZATION_INCA", "LEADER_PACHACUTI"), ("CIVILIZATION_POLYNESIA", "LEADER_KAMEHAMEHA"), ("CIVILIZATION_DENMARK", "LEADER_HARALD"),
    ("CIVILIZATION_KOREA", "LEADER_SEJONG"),
]
DEFAULT_MINORS = [
    "MINOR_CIV_BRATISLAVA", "MINOR_CIV_BUDAPEST", "MINOR_CIV_GENEVA", "MINOR_CIV_RIGA", "MINOR_CIV_GENOA", "MINOR_CIV_FLORENCE",
    "MINOR_CIV_RAGUSA", "MINOR_CIV_SYDNEY", "MINOR_CIV_QUEBEC_CITY", "MINOR_CIV_BUCHAREST", "MINOR_CIV_BELGRADE", "MINOR_CIV_BRUSSELS",
    "MINOR_CIV_ALMATY", "MINOR_CIV_ANTWERP", "MINOR_CIV_BOGOTA", "MINOR_CIV_BYBLOS", "MINOR_CIV_CAHOKIA", "MINOR_CIV_CAPE_TOWN",
    "MINOR_CIV_COLOMBO", "MINOR_CIV_HANOI", "MINOR_CIV_HONG_KONG", "MINOR_CIV_IFE", "MINOR_CIV_JERUSALEM", "MINOR_CIV_KABUL",
]
AXIS_ALIAS = {"civs.count": "c", "minors.count": "m", "seed": "s", "seeds.map": "map", "seeds.sync": "sync", "turns": "t"}


@dataclass
class Item:
    name: str
    scenario: dict
    source: str


# ---------------------------------------------------------------- DB name checks
_DB_MEMO: dict[str, dict[str, set]] = {}


def _db_names(data_dir: Path) -> dict[str, set] | None:
    db = data_dir / "Civ5DebugDatabase.db"
    key = str(db)
    if key in _DB_MEMO:
        return _DB_MEMO[key]
    if not db.is_file():
        return None
    try:
        con = sqlite3.connect(f"file:{db.as_posix()}?mode=ro", uri=True)
        con.execute("select count(*) from sqlite_master").fetchone()
    except sqlite3.Error:
        return None
    try:
        def col(sql: str) -> set:
            return {r[0] for r in con.execute(sql)}
        names = {
            "world_size": col("select Type from Worlds"), "game_speed": col("select Type from GameSpeeds"),
            "era": col("select Type from Eras"), "handicap": col("select Type from HandicapInfos"),
            "climate": col("select Type from Climates"), "sea_level": col("select Type from SeaLevels"),
            "minor": col("select Type from MinorCivilizations"), "map_script": col("select FileName from MapScripts"),
            "pair": {(a, b) for a, b in con.execute("select CivilizationType, LeaderheadType from Civilization_Leaders")},
        }
    except sqlite3.Error:
        return None
    finally:
        con.close()
    _DB_MEMO[key] = names
    return names


def validate(scn: Any, data_dir: Path | None = None) -> tuple[list[str], list[str]]:
    """(errors, warnings). Errors carry the JSON path of the field."""
    errs: list[str] = []
    warns: list[str] = []
    if not isinstance(scn, dict):
        return ["$: scenario must be an object"], warns
    for k in scn:
        if k not in KNOWN:
            warns.append(f"$.{k}: unknown field (kept in the sim key)")
    try:
        C.canonical_json({k: v for k, v in scn.items() if k not in SIM_EXCLUDE})
    except ValueError as e:
        errs.append(str(e))
    seeds = scn.get("seeds")
    if not isinstance(seeds, dict):
        errs.append("$.seeds: mandatory object {sync, map}")
    else:
        for k in ("sync", "map"):
            if not isinstance(seeds.get(k), int) or isinstance(seeds.get(k), bool):
                errs.append(f"$.seeds.{k}: mandatory integer seed")
    civs = scn.get("civs")
    if not isinstance(civs, list) or not civs:
        errs.append("$.civs: needs a non-empty list of {civ, leader}")
    else:
        if len(civs) > 22:
            errs.append("$.civs: at most 22 major civs")
        seen = set()
        for i, c in enumerate(civs):
            if not (isinstance(c, dict) and isinstance(c.get("civ"), str) and isinstance(c.get("leader"), str)):
                errs.append(f"$.civs[{i}]: needs string fields civ and leader")
            elif c["civ"] in seen:
                errs.append(f"$.civs[{i}]: duplicate civ {c['civ']}")
            else:
                seen.add(c["civ"])
    minors = scn.get("minors", [])
    if not isinstance(minors, list) or not all(isinstance(m, str) for m in minors) or len(minors) > 41:
        errs.append("$.minors: list of at most 41 strings")
    elif len(set(minors)) != len(minors):
        errs.append("$.minors: duplicates")
    for k in ("turns", "max_turns", "active_player"):
        if k in scn and (not isinstance(scn[k], int) or isinstance(scn[k], bool) or scn[k] < 0):
            errs.append(f"$.{k}: non-negative integer expected")
    if "turns" in scn and scn.get("turns") == 0:
        errs.append("$.turns: must be > 0")
    env = scn.get("env", {})
    if not isinstance(env, dict) or not all(isinstance(v, (int, str, bool)) for v in env.values()):
        errs.append("$.env: object of axis -> int|string|bool")
    if data_dir is not None:
        names = _db_names(data_dir)
        if names is None:
            warns.append(f"{data_dir}: no Civ5DebugDatabase.db, names not checked")
        else:
            for f in ("world_size", "game_speed", "era", "handicap", "climate", "sea_level"):
                if isinstance(scn.get(f), str) and scn[f] not in names[f]:
                    errs.append(f"$.{f}: unknown name {scn[f]}")
            if isinstance(scn.get("map_script"), str) and scn["map_script"] not in names["map_script"]:
                errs.append(f"$.map_script: not in MapScripts: {scn['map_script']}")
            for i, c in enumerate(civs if isinstance(civs, list) else []):
                if isinstance(c, dict) and (c.get("civ"), c.get("leader")) not in names["pair"]:
                    errs.append(f"$.civs[{i}]: unknown civ/leader pair {c.get('civ')}/{c.get('leader')}")
            for i, m in enumerate(minors if isinstance(minors, list) else []):
                if isinstance(m, str) and m not in names["minor"]:
                    errs.append(f"$.minors[{i}]: unknown minor civ {m}")
    return errs, warns


# ---------------------------------------------------------------- keys
def effective_turns(scn: dict, override: int | None = None) -> int | None:
    return override if override else scn.get("turns")


def canonical(scn: dict, turns: int) -> dict:
    """The sim fields: everything that can change the simulation (name/note/dll/outputs excluded)."""
    c = {k: copy.deepcopy(v) for k, v in scn.items() if k not in SIM_EXCLUDE and k != "turns"}
    c["turns"] = turns
    c.setdefault("env", {})
    return c


def sim_key(canon: dict, dll_sha256: str, data_hash: str, host_build: str) -> str:
    import hashlib
    h = hashlib.sha256(KEY_VERSION)
    for part in (C.canonical_json(canon), dll_sha256.encode(), data_hash.encode(), host_build.encode()):
        h.update(len(part).to_bytes(8, "little"))
        h.update(part)
    return h.hexdigest()


# ---------------------------------------------------------------- corpora
def _set_path(d: dict, dotted: str, value: Any) -> None:
    parts = dotted.split(".")
    cur = d
    for p in parts[:-1]:
        cur = cur.setdefault(p, {})
    cur[parts[-1]] = value


def _apply(scn: dict, dotted: str, value: Any) -> None:
    if dotted == "seed":            # one number drives both seeds (map = v, sync derived), for corpora with many seeds
        scn["seeds"] = {"map": value, "sync": value * 1009 + 17}
    elif dotted == "civs.count":
        pool = list(scn.get("civs", []))
        have = {c["civ"] for c in pool}
        pool += [{"civ": a, "leader": b} for a, b in DEFAULT_CIVS if a not in have]
        if not 1 <= value <= len(pool):
            raise ValueError(f"civs.count {value} out of range 1..{len(pool)}")
        scn["civs"] = pool[:value]
    elif dotted == "minors.count":
        pool = list(scn.get("minors", []))
        pool += [m for m in DEFAULT_MINORS if m not in pool]
        if not 0 <= value <= len(pool):
            raise ValueError(f"minors.count {value} out of range 0..{len(pool)}")
        scn["minors"] = pool[:value]
    else:
        _set_path(scn, dotted, value)


def load_scenario(path: Path) -> dict:
    scn = C.read_json(path)
    if isinstance(scn, dict):
        scn.setdefault("name", path.stem)
    return scn


def is_corpus(obj: Any) -> bool:
    return isinstance(obj, dict) and "corpus" in obj


def expand_corpus(path: Path) -> list[Item]:
    spec = C.read_json(path)
    if not is_corpus(spec):
        raise ValueError(f"{path}: not a corpus (no 'corpus' key)")
    groups = spec.get("groups") or [{k: spec[k] for k in ("base", "set", "matrix") if k in spec}]
    cname = spec["corpus"]
    items: list[Item] = []
    for gi, g in enumerate(groups):
        base = g.get("base")
        if isinstance(base, str):
            base = load_scenario((path.parent / base).resolve())
        elif not isinstance(base, dict):
            raise ValueError(f"{path}: group {gi} needs a 'base' (path or inline scenario)")
        axes = list((g.get("matrix") or {}).items())
        combos = list(itertools.product(*[vals for _k, vals in axes])) if axes else [()]
        for combo in combos:
            scn = copy.deepcopy(base)
            for k, v in (g.get("set") or {}).items():
                _apply(scn, k, v)
            suffix = []
            for (k, _vals), v in zip(axes, combo):
                _apply(scn, k, v)
                tail = AXIS_ALIAS.get(k, k.split(".")[-1])
                suffix.append(f"{tail}{v}".replace("/", "_").replace("\\", "_").replace(" ", "_"))
            name = f"{cname}/{len(items):02d}-{'-'.join(suffix)}" if suffix else f"{cname}/{len(items):02d}-{scn.get('name', 'scn')}"
            scn["name"] = name
            items.append(Item(name=name, scenario=scn, source=str(path)))
    return items


def resolve_targets(args: list[str]) -> list[Item]:
    """Each arg: a corpus name (headless/corpora/<name>.json), a corpus file, or a scenario file."""
    items: list[Item] = []
    for a in args:
        p = Path(a)
        if not p.is_file() and not re.search(r"[\\/]", a):
            p = C.CORPORA / f"{a}.json"
        if not p.is_file():
            raise FileNotFoundError(f"no such scenario/corpus: {a}")
        obj = C.read_json(p)
        if is_corpus(obj):
            items += expand_corpus(p.resolve())
        else:
            scn = load_scenario(p.resolve())
            items.append(Item(name=str(scn.get("name", p.stem)) if isinstance(scn, dict) else p.stem, scenario=scn, source=str(p)))
    names = [i.name for i in items]
    for n in {n for n in names if names.count(n) > 1}:
        k = 0
        for it in items:
            if it.name == n:
                it.name = f"{n}#{k}"
                k += 1
    return items
