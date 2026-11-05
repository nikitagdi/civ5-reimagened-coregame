"""civsim.saveio.corpus — read-only snapshot of the user's saves into headless/out/corpus and its index.

Rules: the source tree is only ever READ (files opened "rb"); the copy is named
<sha256>.Civ5Save; nothing is written outside the corpus directory (which must live under headless/out); reports and
index.json carry no nicknames/SteamIDs and only relative source paths.
"""
from __future__ import annotations

import datetime as _dt
import hashlib
import os
from pathlib import Path
from typing import Any

from .. import common
from .envelope import parse_envelope, summarize

INDEX_SCHEMA = 1
SAVE_SUFFIX = ".civ5save"


def default_corpus_dir() -> Path:
    return common.OUT / "corpus"


def _inside(child: Path, parent: Path) -> bool:
    try:
        child.resolve().relative_to(parent.resolve())
        return True
    except ValueError:
        return False


def _check_out_dir(out: Path, sources: list[Path]) -> None:
    for s in sources:
        if _inside(out, s) or _inside(s, out):
            raise SystemExit(f"refusing: corpus dir {out} overlaps source {s}")
    if "my games" in str(out.resolve()).lower():
        raise SystemExit(f"refusing to write under Documents\\My Games: {out}")


def _iso(ts: float) -> str:
    return _dt.datetime.fromtimestamp(ts, _dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _iter_saves(root: Path):
    for dirpath, _dirs, files in os.walk(root):
        for fn in sorted(files):
            if fn.lower().endswith(SAVE_SUFFIX):
                yield Path(dirpath) / fn


def load_index(corpus: Path) -> dict[str, Any]:
    p = corpus / "index.json"
    if p.is_file():
        try:
            idx = common.read_json(p)
            if idx.get("schema") == INDEX_SCHEMA:
                return idx
        except Exception:
            pass
    return {"schema": INDEX_SCHEMA, "entries": []}


def snapshot(saves: Path, extra: list[Path] | None = None, out: Path | None = None, verbose: bool = True) -> dict[str, Any]:
    """Copy every *.Civ5Save under `saves` (+ `extra` roots) to <out>/saves/<sha256>.Civ5Save and refresh <out>/index.json."""
    out = out or default_corpus_dir()
    roots = [("saves", Path(saves))] + [(f"extra{i + 1}", Path(p)) for i, p in enumerate(extra or [])]
    for _label, root in roots:
        if not root.is_dir():
            raise SystemExit(f"source directory not found: {root}")
    _check_out_dir(out, [r for _l, r in roots])
    store = out / "saves"
    store.mkdir(parents=True, exist_ok=True)

    idx = load_index(out)
    by_key: dict[tuple[str, str], dict[str, Any]] = {(e["root"], e["rel"]): e for e in idx["entries"]}
    summaries: dict[str, dict[str, Any]] = {}
    now = _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    copied = skipped = 0
    seen: set[tuple[str, str]] = set()
    for label, root in roots:
        for path in _iter_saves(root):
            rel = path.relative_to(root).as_posix()
            with open(path, "rb") as f:                      # read-only; sources are never modified
                data = f.read()
            st = path.stat()
            sha = hashlib.sha256(data).hexdigest()
            dst = store / f"{sha}.Civ5Save"
            if dst.exists() and dst.stat().st_size == len(data):
                skipped += 1
            else:
                tmp = dst.with_name(dst.name + f".tmp{os.getpid()}")
                tmp.write_bytes(data)
                os.replace(tmp, dst)
                copied += 1
            if sha not in summaries:
                try:
                    summaries[sha] = summarize(parse_envelope(data))
                except Exception as e:                       # never let one odd file stop the snapshot
                    summaries[sha] = {"status": "error", "reason": f"{type(e).__name__}: {e}", "size": len(data), "sha256": sha}
            sm = summaries[sha]
            entry = {
                "root": label,
                "rel": rel,
                "name": path.name,
                "size": len(data),
                "mtime": _iso(st.st_mtime),
                "sha256": sha,
                "last_seen": now,
                "present": True,
            }
            entry.update({k: v for k, v in sm.items() if k not in ("size", "sha256")})
            by_key[(label, rel)] = entry
            seen.add((label, rel))
    for key, e in by_key.items():
        if key not in seen:
            e["present"] = False                             # rotated away by the game; the stored copy remains
    entries = sorted(by_key.values(), key=lambda e: (e["root"], e["rel"]))
    index = {
        "schema": INDEX_SCHEMA,
        "updated": now,
        "counts": _counts(entries),
        "entries": entries,
    }
    common.write_json(out / "index.json", index)
    if verbose:
        c = index["counts"]
        print(f"snapshot: {c['files']} files, {c['unique']} unique ({copied} copied, {skipped} already stored) -> {store}")
        print(f"  supported {c['supported']}, no-body {c['no-body']}, unsupported {c['unsupported']}, error {c['error']}")
    return index


def _counts(entries: list[dict[str, Any]]) -> dict[str, int]:
    c = {"files": len(entries), "unique": len({e["sha256"] for e in entries}),
         "supported": 0, "no-body": 0, "unsupported": 0, "error": 0}
    uniq: dict[str, str] = {}
    for e in entries:
        uniq[e["sha256"]] = e.get("status", "error")
    for st in uniq.values():
        c[st if st in c else "error"] += 1
    return c


def stats(index: dict[str, Any]) -> dict[str, Any]:
    """Aggregates over UNIQUE saves (by sha256)."""
    uniq: dict[str, dict[str, Any]] = {}
    for e in index["entries"]:
        uniq.setdefault(e["sha256"], e)
    rows = list(uniq.values())
    out: dict[str, Any] = {"files": len(index["entries"]), "unique": len(rows)}
    for st in ("supported", "unsupported", "no-body", "error"):
        sel = [r for r in rows if r.get("status") == st]
        d: dict[str, Any] = {"count": len(sel)}
        if sel:
            sizes = [r["size"] for r in sel]
            d["size_min"], d["size_max"] = min(sizes), max(sizes)
            turns = [r["turn"] for r in sel if r.get("turn") is not None]
            if turns:
                d["turn_min"], d["turn_max"] = min(turns), max(turns)
            bodies = [r["body_size"] for r in sel if r.get("body_size")]
            if bodies:
                d["body_min"], d["body_max"] = min(bodies), max(bodies)
            players = [r["players"] for r in sel if r.get("players") is not None]
            if players:
                d["players_min"], d["players_max"] = min(players), max(players)
            reasons: dict[str, int] = {}
            for r in sel:
                reasons[r.get("reason") or "-"] = reasons.get(r.get("reason") or "-", 0) + 1
            d["reasons"] = reasons
            tags: dict[str, int] = {}
            for r in sel:
                tags[str(r.get("lekmod_tag"))] = tags.get(str(r.get("lekmod_tag")), 0) + 1
            d["tags"] = tags
        out[st] = d
    return out
