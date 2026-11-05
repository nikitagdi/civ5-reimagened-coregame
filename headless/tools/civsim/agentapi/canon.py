"""Canonical JSON for headless/agent-api (Code Style): sorted keys, 2 spaces, UTF-8, LF.

dict_hash = SHA-256 of the canonical commands.json bytes (the file itself carries no hash: it would be circular).
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path

# headless/tools/civsim/agentapi/canon.py -> repo root is parents[4]
REPO = Path(__file__).resolve().parents[4]
API_DIR = REPO / "headless" / "agent-api"
DOCS_DIR = REPO / "headless" / "generated-docs"
DLL_SRC = REPO / "LEKMOD_DLL" / "CvGameCoreDLL_Expansion2"
FILES = ("commands", "observation", "events", "errors")


def dumps(obj: object) -> str:
    return json.dumps(obj, sort_keys=True, indent=2, ensure_ascii=False) + "\n"


def to_bytes(obj: object) -> bytes:
    return dumps(obj).encode("utf-8")


def dict_hash(commands_doc: object) -> str:
    return hashlib.sha256(to_bytes(commands_doc)).hexdigest()


def path_of(name: str, api_dir: Path | None = None) -> Path:
    return (api_dir or API_DIR) / f"{name}.json"


def load(name: str, api_dir: Path | None = None) -> dict:
    return json.loads(path_of(name, api_dir).read_text(encoding="utf-8"))


def is_canonical(name: str, api_dir: Path | None = None) -> bool:
    raw = path_of(name, api_dir).read_bytes()
    return raw == to_bytes(json.loads(raw.decode("utf-8")))


def write(name: str, obj: object, api_dir: Path | None = None) -> bool:
    """Write canonical bytes; True when the file changed."""
    p = path_of(name, api_dir)
    p.parent.mkdir(parents=True, exist_ok=True)
    data = to_bytes(obj)
    if p.exists() and p.read_bytes() == data:
        return False
    p.write_bytes(data)
    return True
