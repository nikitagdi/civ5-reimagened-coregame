"""Loading and schema validation of headless/improvements/*.json."""
from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path

# headless/tools/civsim/improve/registry.py -> repo root is parents[4]
REPO = Path(__file__).resolve().parents[4]
IMPROVEMENTS_DIR = REPO / "headless" / "improvements"
SRC_DIR = REPO / "LEKMOD_DLL" / "CvGameCoreDLL_Expansion2"
DOCS_DIR = REPO / "headless" / "generated-docs"

SCHEMA = 1
# statuses whose code is in the tree and that may sit in a flag set
IMPLEMENTED = ("implemented", "accepted", "released")
STATUSES = IMPLEMENTED + ("proposed", "awaiting-user", "not-applicable", "not-implemented")
KINDS = ("fix", "validation", "hyg", "opt")

# fields every FIX / HYG record must have
FIX_FIELDS = (
    "id", "flag", "ds", "kind", "status", "approval", "release", "title", "before", "after", "scope",
    "save_compat", "mp_compat", "balance", "files", "evidence",
)
SAVE_COMPAT_FIELDS = ("old_saves_load", "new_saves_load_in_old", "continuing_old_game_changes")
# OPT lifecycle: proposed -> prototyped -> equiv-ok -> perf-ok -> accepted
OPT_STATUSES = ("proposed", "prototyped", "equiv-ok", "perf-ok", "accepted", "released", "rejected")
OPT_IN_TREE = ("prototyped", "equiv-ok", "perf-ok", "accepted", "released")  # code is in the tree behind its flag
OPT_RISK_CLASSES = ("A1", "A2", "A3", "A4", "A5", "A6")
# the three mandatory justification lines of every OPT (iteration order, RNG, float)
OPT_FIELDS = ("id", "flag", "kind", "status", "title", "hotspot", "risk_class", "gain", "equivalence", "files",
              "iteration_order", "rng", "float")


class RegistryError(Exception):
    pass


@dataclass
class Registry:
    fixes: list[dict] = field(default_factory=list)
    hyg: list[dict] = field(default_factory=list)
    opts: list[dict] = field(default_factory=list)
    flagsets: dict[str, list[dict]] = field(default_factory=dict)

    def records(self) -> list[dict]:
        return self.opts + self.hyg + self.fixes

    def by_id(self) -> dict[str, dict]:
        return {r["id"]: r for r in self.records()}


def _load(path: Path, key: str | None) -> object:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        raise RegistryError(f"missing registry file: {path}")
    except json.JSONDecodeError as e:
        raise RegistryError(f"{path}: invalid JSON: {e}")
    if data.get("schema") != SCHEMA:
        raise RegistryError(f"{path}: schema {data.get('schema')!r}, expected {SCHEMA}")
    if key is None:
        return data
    if key not in data or not isinstance(data[key], (list, dict)):
        raise RegistryError(f"{path}: missing '{key}'")
    return data[key]


def load(root: Path | None = None) -> Registry:
    root = root or IMPROVEMENTS_DIR
    reg = Registry(
        fixes=list(_load(root / "fixes.json", "fixes")),
        hyg=list(_load(root / "hyg.json", "hyg")),
        opts=list(_load(root / "opts.json", "opts")),
        flagsets=dict(_load(root / "flagsets.json", "sets")),
    )
    return reg


def validate(reg: Registry) -> list[str]:
    """Schema-level problems (empty list = fine)."""
    errs: list[str] = []
    seen_ids: set[str] = set()
    seen_flags: dict[str, str] = {}
    for rec in reg.fixes + reg.hyg:
        rid = rec.get("id", "<no id>")
        for f in FIX_FIELDS:
            if f not in rec:
                errs.append(f"{rid}: missing field '{f}'")
        sc = rec.get("save_compat")
        if isinstance(sc, dict):
            for f in SAVE_COMPAT_FIELDS:
                if f not in sc:
                    errs.append(f"{rid}: save_compat missing '{f}'")
        if rec.get("kind") not in KINDS:
            errs.append(f"{rid}: bad kind {rec.get('kind')!r}")
        if rec.get("status") not in STATUSES:
            errs.append(f"{rid}: bad status {rec.get('status')!r}")
        if rec.get("release") and rec.get("status") not in IMPLEMENTED:
            errs.append(f"{rid}: release=true but status is {rec.get('status')!r}")
        if rec.get("release") and rec.get("approval") == "awaiting-user":
            errs.append(f"{rid}: release=true but approval is awaiting-user")
        if rid in seen_ids:
            errs.append(f"{rid}: duplicate id")
        seen_ids.add(rid)
        flag = rec.get("flag")
        if flag:
            if flag in seen_flags:
                errs.append(f"{rid}: flag {flag} already used by {seen_flags[flag]}")
            seen_flags[flag] = rid
    for rec in reg.opts:
        rid = rec.get("id", "<no id>")
        for f in OPT_FIELDS:
            if f not in rec:
                errs.append(f"{rid}: missing field '{f}'")
            elif f in ("iteration_order", "rng", "float") and not str(rec[f]).strip():
                errs.append(f"{rid}: empty justification '{f}'")
        if rec.get("kind") != "opt":
            errs.append(f"{rid}: bad kind {rec.get('kind')!r} (expected 'opt')")
        if rec.get("status") not in OPT_STATUSES:
            errs.append(f"{rid}: bad status {rec.get('status')!r}")
        if rec.get("risk_class") not in OPT_RISK_CLASSES:
            errs.append(f"{rid}: bad risk_class {rec.get('risk_class')!r}")
        if rec.get("release") and rec.get("status") not in OPT_IN_TREE:
            errs.append(f"{rid}: release=true but status is {rec.get('status')!r}")
        if rid in seen_ids:
            errs.append(f"{rid}: duplicate id")
        seen_ids.add(rid)
        flag = rec.get("flag")
        if flag:
            if flag in seen_flags:
                errs.append(f"{rid}: flag {flag} already used by {seen_flags[flag]}")
            seen_flags[flag] = rid
    for setname in ("opt", "fix", "release"):
        if setname not in reg.flagsets or not isinstance(reg.flagsets[setname], list):
            errs.append(f"flagsets.json: missing set '{setname}'")
    return errs
