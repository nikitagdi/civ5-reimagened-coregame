"""civsim.saveio.envelope — Python-side parse of the `.Civ5Save` envelope (everything except the DLL's body bytes).

File layout:

    exe.header        "CIV5", u32 format version, strings (game version, build), u32 turn, u8, 7 preview strings,
                      DLC list, mod list, tail (2 strings, player color, GUID, string, GUID, u32)
    dll.pregame       CvPreGame::write (saveSlotHints v3 + writeArchive v6), UNCOMPRESSED, parsed in pregame.py
    exe.zstream       u32 2 (meaning unknown, same in every corpus file) followed by CHUNKS to EOF:
                      { u32 length, length bytes } with length = 65536 for every chunk but the last (1..65536).
                      The chunk payloads concatenated are ONE zlib stream (78 9C ...) that ends in a Z_SYNC_FLUSH marker
                      (00 00 FF FF), has NO final block and no adler32. (The "8 unknown bytes" of the first spec draft are
                      the u32 2 plus the first chunk length 0x10000; feeding the chunk headers to inflate corrupts the
                      stream after the first 64 KiB.)

Every byte is a Span (parsed / opaque / dll / zlib). A structure that does not match yields a non-"supported" status with
a reason; nothing here raises on a hostile file (the caller gets a result object either way).
"""
from __future__ import annotations

import hashlib
import re
import struct
import zlib
from dataclasses import dataclass, field
from typing import Any

from .binio import Reader, SaveFormatError, Span, check_coverage
from .pregame import active_slot_count, parse_pregame

MAGIC = b"CIV5"
EXPECTED_FORMAT_VERSION = 8
EXPECTED_GAME_VERSION = "1.0.3.279 (403694)"
SUPPORTED_LEKMOD_VERSION = "34.15"
ZSTREAM_VERSION = 2                 # the u32 in front of the chunk table (constant in all 80 corpus files with a body)
CHUNK_SIZE = 65536                  # every chunk but the last
SYNC_FLUSH_TAIL = b"\x00\x00\xff\xff"
MAX_DLC = 4096


# ---------------------------------------------------------------------------------- zlib / chunk layer
@dataclass
class InflateResult:
    body: bytes
    complete: bool              # True iff a final block (and adler32) was seen — never true for the game's own saves
    ends_with_sync_flush: bool
    unused_tail: int            # bytes after the end of a complete stream (0 for the game's saves)
    error: str | None = None
    error_at: int | None = None   # offset in the (de-chunked) compressed stream where inflate gave up


def zlib_header_ok(b: bytes) -> bool:
    return len(b) >= 2 and (b[0] & 0x0F) == 8 and ((b[0] << 8) | b[1]) % 31 == 0


def inflate_stream(stream: bytes) -> InflateResult:
    """Inflate a zlib stream that may stop at Z_SYNC_FLUSH without a final block (the game's saves).

    Partial output is kept on error (retrying the failing 64 KiB step in 64-byte pieces)."""
    d = zlib.decompressobj()
    parts: list[bytes] = []
    err = None
    err_at = None
    step = 1 << 16
    i = 0
    n = len(stream)
    while i < n:
        snap = d.copy()
        try:
            parts.append(d.decompress(stream[i:i + step]))
            i += step
        except zlib.error as e:
            d = snap
            j = i
            end = min(i + step, n)
            try:
                while j < end:
                    parts.append(d.decompress(stream[j:j + 64]))
                    j += 64
            except zlib.error as e2:
                err, err_at = str(e2), j
            else:
                err, err_at = str(e), i
            break
    if err is None:
        parts.append(d.flush())
    return InflateResult(b"".join(parts), d.eof, stream.endswith(SYNC_FLUSH_TAIL), len(d.unused_data), err, err_at)


@dataclass
class ZStream:
    off: int                        # offset of the u32 version (== end of the pregame block)
    version: int
    chunk_lens: list[int]
    chunk_offs: list[int]           # absolute file offset of each payload
    stream: bytes                   # payloads concatenated
    clean: bool                     # chunk table ends exactly at EOF


def parse_zstream(data: bytes, off: int, spans: list[Span], fields: dict[str, Any]) -> ZStream:
    """Parse `u32 version` + chunk table at data[off:]. Appends spans; raises SaveFormatError only if even the version is missing."""
    if off + 4 > len(data):
        raise SaveFormatError("file ends before the compressed-stream version", off)
    ver = struct.unpack_from("<I", data, off)[0]
    spans.append(Span("exe.zstream.version", off, 4, "opaque", "exe", "u32 2 in every corpus file; meaning unknown"))
    pos = off + 4
    lens: list[int] = []
    offs: list[int] = []
    payload: list[bytes] = []
    clean = True
    i = 0
    while pos < len(data):
        if pos + 4 > len(data):
            clean = False
            spans.append(Span("exe.zstream.tail", pos, len(data) - pos, "opaque", "exe", "truncated chunk header"))
            break
        n = struct.unpack_from("<I", data, pos)[0]
        if n == 0 or n > CHUNK_SIZE or pos + 4 + n > len(data):
            clean = False
            spans.append(Span("exe.zstream.tail", pos, len(data) - pos, "opaque", "exe",
                              f"bad chunk length {n} at chunk {i} ({len(data) - pos - 4} bytes left)"))
            break
        spans.append(Span(f"zstream.chunk[{i}].len", pos, 4, "parsed", "exe"))
        spans.append(Span(f"zstream.chunk[{i}].data", pos + 4, n, "zlib", "exe"))
        lens.append(n)
        offs.append(pos + 4)
        payload.append(data[pos + 4:pos + 4 + n])
        pos += 4 + n
        i += 1
    fields["zstream.version"] = ver
    fields["zstream.chunks"] = len(lens)
    return ZStream(off, ver, lens, offs, b"".join(payload), clean)


@dataclass
class Envelope:
    size: int
    sha256: str
    spans: list[Span] = field(default_factory=list)
    fields: dict[str, Any] = field(default_factory=dict)        # RAW parsed values (may contain nicknames; use report.public_*)
    status: str = "error"                  # supported | unsupported | no-body | error
    reason: str = ""
    warnings: list[str] = field(default_factory=list)
    pregame_off: int | None = None
    pregame_end: int | None = None
    zstream: ZStream | None = None
    inflate: InflateResult | None = None

    @property
    def supported(self) -> bool:
        return self.status == "supported"

    @property
    def zstream_off(self) -> int | None:
        return self.zstream.off if self.zstream else None

    def coverage_problems(self) -> list[str]:
        return check_coverage(self.spans, self.size)


# --------------------------------------------------------------------------- header
def _parse_exe_header(r: Reader) -> int:
    """Reads up to the start of the DLL pregame. Returns that offset. Raises SaveFormatError."""
    r.kind, r.owner = "parsed", "exe"
    magic = r.blob(4, "hdr.magic")
    if magic != MAGIC:
        raise SaveFormatError(f"not a Civ5Save (magic {magic!r})", 0)
    fv = r.u32("hdr.format_version")
    if fv != EXPECTED_FORMAT_VERSION:
        raise SaveFormatError(f"unsupported envelope format version {fv}", 4)
    r.string("hdr.game_version", max_len=256)
    r.string("hdr.build_number", max_len=256)
    r.u32("hdr.turn")
    r.u8("hdr.game_type")
    for name in ("civ", "difficulty", "start_era", "current_era", "game_speed", "world_size", "map_script"):
        r.string("hdr.preview." + name, max_len=1024)
    # DLC list: u32 count, each {GUID16, u32 flag, string name}
    n = r.u32("hdr.dlc.count")
    if n > MAX_DLC:
        raise SaveFormatError(f"implausible DLC count {n}", r.pos - 4)
    dlc = []
    for i in range(n):
        g = r.guid(f"hdr.dlc[{i}].guid", store=False)
        fl = r.u32(f"hdr.dlc[{i}].flag", store=False)
        nm = r.string(f"hdr.dlc[{i}].name", store=False, max_len=256)
        dlc.append({"guid": g, "flag": fl, "name": nm})
    r.fields["hdr.dlc"] = dlc
    # mods: u32 count, entries assumed {GUID16, u32, string} like DLC (010 template); the corpus has 0 mods
    m = r.u32("hdr.mod.count")
    if m > MAX_DLC:
        raise SaveFormatError(f"implausible mod count {m}", r.pos - 4)
    mods = []
    for i in range(m):
        g = r.guid(f"hdr.mod[{i}].guid", store=False)
        fl = r.u32(f"hdr.mod[{i}].flag", store=False)
        nm = r.string(f"hdr.mod[{i}].name", store=False, max_len=256)
        mods.append({"guid": g, "flag": fl, "name": nm})
    r.fields["hdr.mod"] = mods
    # tail (semantics guessed, bytes verified against the corpus): two strings (empty in the corpus), the active player's
    # PLAYERCOLOR_*, GUID, version string, GUID, u32
    r.kind = "opaque"                       # structure known, meaning unknown: carried verbatim by extract (header.bin)
    r.string("hdr.tail.str_a", max_len=1024)
    r.string("hdr.tail.str_b", max_len=1024)
    r.kind = "parsed"
    r.string("hdr.tail.player_color", max_len=1024)
    r.kind = "opaque"
    r.guid("hdr.tail.guid_a")
    r.string("hdr.tail.version_str", max_len=256)
    r.guid("hdr.tail.guid_b")
    r.u32("hdr.tail.u32")
    r.kind = "parsed"
    return r.pos


_ZSTREAM_PROBE = struct.pack("<II", ZSTREAM_VERSION, CHUNK_SIZE)


def _scan_zstream(data: bytes, lo: int) -> int | None:
    """Fallback locator when the pregame cannot be parsed: first offset >= lo of `u32 2, u32 0x10000, <zlib header that inflates>`."""
    pos = lo
    while True:
        i = data.find(_ZSTREAM_PROBE, pos)
        if i < 0:
            return None
        z = i + 8
        if zlib_header_ok(data[z:z + 2]):
            try:
                zlib.decompressobj().decompress(data[z:z + CHUNK_SIZE])
                return i
            except zlib.error:
                pass
        pos = i + 1


def parse_envelope(data: bytes, inflate: bool = True) -> Envelope:
    env = Envelope(size=len(data), sha256=hashlib.sha256(data).hexdigest())
    r = Reader(data, spans=env.spans, fields=env.fields)
    try:
        hdr_end = _parse_exe_header(r)
    except SaveFormatError as e:
        env.status, env.reason = "unsupported", f"envelope header: {e}"
        _close_unparsed(env, data, max((s.end for s in env.spans), default=0))
        return env
    env.pregame_off = hdr_end
    # ---- DLL pregame --------------------------------------------------------------------
    try:
        pspans, pfields, pend = parse_pregame(data, hdr_end)
    except SaveFormatError as e:
        env.status, env.reason = "unsupported", f"pregame: {e}"
        z = _scan_zstream(data, hdr_end)
        if z is None:
            _close_unparsed(env, data, hdr_end)
        else:
            env.spans.append(Span("unparsed.pregame", hdr_end, z - hdr_end, "opaque", "dll", "pregame not parsed: " + env.reason))
            env.pregame_end = z
            _attach_zstream(env, data, z, inflate)
        return env
    env.spans.extend(pspans)
    env.fields.update(pfields)
    env.pregame_end = pend
    # ---- compressed body ----------------------------------------------------------------
    if pend == len(data):
        env.status, env.reason = "no-body", "no compressed body (game configuration save)"
        return env
    if len(data) - pend < 12:
        env.status, env.reason = "unsupported", "file ends inside the compressed-stream header"
        _close_unparsed(env, data, pend)
        return env
    _attach_zstream(env, data, pend, inflate)
    return env


def _attach_zstream(env: Envelope, data: bytes, off: int, inflate: bool) -> None:
    try:
        zs = parse_zstream(data, off, env.spans, env.fields)
    except SaveFormatError as e:
        env.status, env.reason = "unsupported", str(e)
        _close_unparsed(env, data, off)
        return
    env.zstream = zs
    if zs.version != ZSTREAM_VERSION:
        env.warnings.append(f"zstream version {zs.version} (expected {ZSTREAM_VERSION})")
    if not zs.clean:
        env.status, env.reason = "unsupported", "chunk table does not end at EOF"
    if not zlib_header_ok(zs.stream[:2]):
        env.status, env.reason = "unsupported", f"no zlib header at the start of the chunk payload ({zs.stream[:2].hex()})"
        return
    env.fields["zlib.header"] = zs.stream[:2].hex()
    if inflate:
        res = inflate_stream(zs.stream)
        env.inflate = res
        if res.error:
            env.status, env.reason = "unsupported", f"zlib error at stream offset {res.error_at}: {res.error}"
            return
        if res.complete or res.unused_tail:
            env.warnings.append(f"zlib stream is complete (eof) with {res.unused_tail} trailing bytes; the game's saves end in a sync flush")
        if not res.ends_with_sync_flush:
            env.warnings.append("stream does not end with a Z_SYNC_FLUSH marker (00 00 ff ff)")
    if env.status == "unsupported":
        return
    _classify(env, need_inflate=inflate)


def _close_unparsed(env: Envelope, data: bytes, pos: int) -> None:
    """Account for every remaining byte as one opaque span (the file is not supported, but nothing is dropped)."""
    if pos < len(data):
        env.spans.append(Span("unparsed.rest", pos, len(data) - pos, "opaque", "unknown", env.reason))


# --------------------------------------------------------------------------- classification
_TAG_RE = re.compile(r"(?i)(lekmod v(\d+(?:\.\d+)*))\s*$")


def lekmod_tag(fields: dict[str, Any]) -> str | None:
    """`LEKMOD vX.Y` (as spelled in the save) from the END of CvPreGame::s_gameName (archive.game_name), else None.

    The game name is `<host-typed text>` + tag, e.g. `LEKMOD v34.15`, `Lekmod v33.11`; any prefix is personal and never reported."""
    name = fields.get("archive.game_name")
    m = _TAG_RE.search(name) if isinstance(name, str) else None
    return m.group(1) if m else None


def lekmod_version(fields: dict[str, Any]) -> str | None:
    name = fields.get("archive.game_name")
    m = _TAG_RE.search(name) if isinstance(name, str) else None
    return m.group(2) if m else None


def _classify(env: Envelope, need_inflate: bool = True) -> None:
    f = env.fields
    tag = lekmod_tag(f)
    ver = lekmod_version(f)
    problems = env.coverage_problems()
    if problems:
        env.status, env.reason = "error", "coverage: " + "; ".join(problems[:3])
    elif f.get("hdr.game_version") != EXPECTED_GAME_VERSION:
        env.status, env.reason = "unsupported", f"game version {f.get('hdr.game_version')!r}"
    elif ver != SUPPORTED_LEKMOD_VERSION:
        env.status, env.reason = "unsupported", f"game name tag {tag!r} (supported: LEKMOD v{SUPPORTED_LEKMOD_VERSION})"
    elif env.pregame_end is None or env.zstream is None or (need_inflate and (env.inflate is None or env.inflate.error)):
        env.status, env.reason = "unsupported", "incomplete envelope"
    else:
        env.status, env.reason = "supported", ""


# --------------------------------------------------------------------------- summary (masked; safe for reports / index.json)
def summarize(env: Envelope) -> dict[str, Any]:
    """Flat, masked summary used by index.json and `inspect`. Never contains nicknames or SteamIDs."""
    from .report import mask_text
    f = env.fields
    s: dict[str, Any] = {
        "status": env.status,
        "reason": mask_text(env.reason),
        "size": env.size,
        "sha256": env.sha256,
        "format_version": f.get("hdr.format_version"),
        "game_version": f.get("hdr.game_version"),
        "build_number": f.get("hdr.build_number"),
        "turn": f.get("hdr.turn"),
        "game_type": f.get("hdr.game_type"),
        "civ": f.get("hdr.preview.civ"),
        "difficulty": f.get("hdr.preview.difficulty"),
        "start_era": f.get("hdr.preview.start_era"),
        "current_era": f.get("hdr.preview.current_era"),
        "game_speed": f.get("hdr.preview.game_speed"),
        "world_size": f.get("hdr.preview.world_size"),
        "map_script": f.get("hdr.preview.map_script"),
        "dlc_count": len(f["hdr.dlc"]) if "hdr.dlc" in f else None,
        "mod_count": len(f["hdr.mod"]) if "hdr.mod" in f else None,
        "lekmod_tag": lekmod_tag(f),
        "lekmod_version": lekmod_version(f),
        "lekmod": lekmod_tag(f) is not None,
        "has_body": env.zstream is not None,
        "warnings": [mask_text(w) for w in env.warnings],
    }
    if "slot_hints.slot_status" in f:
        s["players"] = active_slot_count(f)
        s["humans"] = sum(1 for st, c in zip(f["slot_hints.slot_status"], f["slot_hints.slot_claims"]) if st == 3 and c in (1, 2))
        s["pregame_game_turn"] = f.get("archive.game_turn")
        s["game_started"] = f.get("archive.game_started")
    if env.zstream is not None:
        s["zstream_chunks"] = len(env.zstream.chunk_lens)
        s["compressed_size"] = len(env.zstream.stream)
    if env.inflate is not None:
        s["body_size"] = len(env.inflate.body)
        s["body_complete_stream"] = env.inflate.complete
        s["ends_with_sync_flush"] = env.inflate.ends_with_sync_flush
    return s
