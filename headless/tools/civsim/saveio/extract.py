"""civsim.saveio.extract — split a `.Civ5Save` into the input format of the future civhost import.

    <out>/header.bin     exe header bytes [0, pregame offset)                 (carried verbatim on export: --envelope-from)
    <out>/pregame.bin    EXACT bytes of the DLL-owned CvPreGame::write block   (contains nicknames/SteamIDs: stays under headless/out)
    <out>/body.bin       inflated body (DLL stream: CvGame::Write, CvMap::Write, teams, players, supporting data)
    <out>/envelope.json  offsets, chunk table, opaque blobs (hex), masked pregame fields/slots, heuristic body segments
    <out>/zstream.bin    (--with-zstream) the de-chunked zlib stream, for byte-exact recompression experiments

"""
from __future__ import annotations

import hashlib
import zlib
from pathlib import Path
from typing import Any

from .. import common
from . import segments as seg_mod
from .envelope import CHUNK_SIZE, ZSTREAM_VERSION, Envelope, parse_envelope, summarize
from .pregame import slot_table
from .report import mask_text, public_fields

SCHEMA = 1

# exe load order of the body, from Resync_LoadGameFromStreamImpl; NOT verified against the exe
LOAD_ORDER = [
    "CvPreGame::read (pregame.bin; uncompressed, before the zlib body)",
    "ICvGame1::Read            body.bin starts here: CvGame::Write (ends with the embedded SQLite blob)",
    "ICvMap1::Read             CvMap::Write: header, resource arrays, W*H plots, areas, landmasses, AI hints",
    "ICvTeam1::Read  x teams",
    "ICvPlayer1::Read x players",
    "ICvGame1::ReadSupportingClassData  (CvBarbarians, CvGoodyHuts)",
]


def _sha(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def deflate_recipe_check(stream: bytes, body: bytes) -> dict[str, Any]:
    """Does Python's zlib reproduce the game's compressed stream byte for byte? (level 6, windowBits 15, memLevel 8, default
    strategy, input fed in one go; the end is either Z_SYNC_FLUSH ["a"] or Z_PARTIAL_FLUSH + Z_SYNC_FLUSH ["b"].)"""
    info: dict[str, Any] = {"zlib_runtime": zlib.ZLIB_RUNTIME_VERSION, "params": "level 6, windowBits 15, memLevel 8, Z_DEFAULT_STRATEGY"}
    last_len = -1
    for name, flushes in (("a", (zlib.Z_SYNC_FLUSH,)), ("b", (zlib.Z_PARTIAL_FLUSH, zlib.Z_SYNC_FLUSH))):
        c = zlib.compressobj(6, zlib.DEFLATED, 15, 8, zlib.Z_DEFAULT_STRATEGY)
        out = c.compress(body) + b"".join(c.flush(f) for f in flushes)
        if out == stream:
            info.update(reproduced=True, variant=name,
                        end="Z_SYNC_FLUSH" if name == "a" else "Z_PARTIAL_FLUSH then Z_SYNC_FLUSH (one extra empty static block)")
            return info
        n = min(len(out), len(stream))
        k = 0
        while k < n and out[k] == stream[k]:
            k += 1
        last_len = k
    info.update(reproduced=False, variant=None, first_difference_at=last_len)
    return info


def _slots(env: Envelope) -> list[dict[str, Any]]:
    if "slot_hints.slot_status" not in env.fields:
        return []
    rows = []
    for r in slot_table(env.fields):
        if r["status"] in ("OPEN", "CLOSED") and r["claim"] == "UNASSIGNED" and not r["civ"]:
            continue
        r = dict(r)
        r["civ"] = mask_text(r["civ"])
        r["leader"] = mask_text(r["leader"])
        r["nickname"] = f"player#{r['slot']}" if r.pop("has_nickname") else ""
        rows.append(r)
    return rows


def build_envelope_json(data: bytes, env: Envelope, files: dict[str, dict[str, Any]], with_segments: bool = True) -> dict[str, Any]:
    zs = env.zstream
    inf = env.inflate
    j: dict[str, Any] = {
        "schema": SCHEMA,
        "tool": "civsim.saveio extract",
        "source": {"sha256": env.sha256, "size": env.size, "status": env.status, "reason": mask_text(env.reason)},
        "summary": summarize(env),
        "layout": {
            "header": {"file": "header.bin", "offset": 0, "length": env.pregame_off, "owner": "exe"},
            "pregame": {"file": "pregame.bin", "offset": env.pregame_off,
                        "length": None if env.pregame_end is None else env.pregame_end - env.pregame_off,
                        "owner": "dll", "writer": "CvPreGame::write (saveSlotHints v3 + writeArchive v6)"},
        },
        "files": files,
        "load_order": LOAD_ORDER,
        "spans": [s.to_json() for s in env.spans],
        "opaque_blobs": {},
        "fields": public_fields(env.fields),
        "slots": _slots(env),
        "warnings": [mask_text(w) for w in env.warnings],
        "coverage_problems": env.coverage_problems(),
    }
    if zs is not None:
        j["layout"]["zstream"] = {
            "offset": zs.off,
            "version": zs.version,
            "expected_version": ZSTREAM_VERSION,
            "chunk_size": CHUNK_SIZE,
            "chunk_count": len(zs.chunk_lens),
            "chunk_lengths": zs.chunk_lens,
            "compressed_size": len(zs.stream),
            "zlib_header": zs.stream[:2].hex(),
            "table_ends_at_eof": zs.clean,
            "note": "u32 version, then {u32 len, len bytes} chunks to EOF; payloads concatenated = one zlib stream, "
                    "ends in Z_SYNC_FLUSH (00 00 ff ff), no final block",
        }
    if inf is not None:
        j["layout"]["body"] = {"file": "body.bin", "length": len(inf.body), "sha256": _sha(inf.body),
                               "stream_complete": inf.complete, "ends_with_sync_flush": inf.ends_with_sync_flush}
    for s in env.spans:
        if s.kind == "opaque":
            j["opaque_blobs"][s.name] = {"offset": s.off, "length": s.len, "hex": data[s.off:s.end].hex(), "note": s.note}
    if with_segments and inf is not None and inf.body:
        grid = None
        wi = env.fields.get("archive.world_info")
        if isinstance(wi, dict) and "grid_width" in wi:
            grid = (wi["grid_width"], wi["grid_height"])
        j["body_segments"] = seg_mod.locate(inf.body, grid, env.fields.get("hdr.turn"))
    return j


def extract(data: bytes, out_dir: Path, with_zstream: bool = False, check_deflate: bool = True) -> dict[str, Any]:
    """Write the extract set for `data` into `out_dir` (created). Returns the envelope.json content."""
    env = parse_envelope(data)
    if env.pregame_end is None:
        raise ValueError(f"cannot extract: {env.reason or 'no pregame block found'}")
    out_dir.mkdir(parents=True, exist_ok=True)
    files: dict[str, dict[str, Any]] = {}

    def put(name: str, blob: bytes) -> None:
        (out_dir / name).write_bytes(blob)
        files[name] = {"length": len(blob), "sha256": _sha(blob)}

    put("header.bin", data[:env.pregame_off])
    put("pregame.bin", data[env.pregame_off:env.pregame_end])
    if env.inflate is not None and env.inflate.body and not env.inflate.error:
        put("body.bin", env.inflate.body)
    if with_zstream and env.zstream is not None:
        put("zstream.bin", env.zstream.stream)
    j = build_envelope_json(data, env, files)
    if check_deflate and env.zstream is not None and env.inflate is not None and env.inflate.body and not env.inflate.error:
        j["layout"]["zstream"]["deflate"] = deflate_recipe_check(env.zstream.stream, env.inflate.body)
    common.write_json(out_dir / "envelope.json", j)
    return j
