"""civsim.saveio.synth — SYNTHETIC `.Civ5Save` construction (tests, fixtures, host-import experiments).

Nothing here is derived from real save data: names, GUIDs, nicknames and numbers are invented. The writers mirror the layouts
of the envelope parser, so `parse_envelope(build_save(...))` exercises the same code paths as a real
save. `assemble_save` is also the reference for the (future) export side: u32 version + 64 KiB chunks around one zlib stream
that ends in Z_SYNC_FLUSH without a final block. With Python's zlib at level 6 this is byte-identical to the game's own file for
67 of the 77 corpus saves that have a body; the other 10 need `partial_flush=True` (an extra empty static block before the sync marker).
"""
from __future__ import annotations

import struct
import zlib
from typing import Any

from .envelope import CHUNK_SIZE, ZSTREAM_VERSION

FAKE_GUID_A = bytes(range(16))
FAKE_GUID_B = bytes(range(16, 32))


def u32(v: int) -> bytes:
    return struct.pack("<I", v & 0xFFFFFFFF)


def i32(v: int) -> bytes:
    return struct.pack("<i", v)


def s(text: str) -> bytes:
    b = text.encode("utf-8")
    return u32(len(b)) + b


def vec_i32(vals: list[int]) -> bytes:
    return u32(len(vals)) + b"".join(i32(v) for v in vals)


def vec_bool(vals: list[bool]) -> bytes:
    return u32(len(vals)) + bytes(1 if v else 0 for v in vals)


def vec_str(vals: list[str]) -> bytes:
    return u32(len(vals)) + b"".join(s(v) for v in vals)


def info_base(type_id: str = "", desc: str = "") -> bytes:
    return i32(0) + s("") + s(desc) + s("") + s("") + s("") + s(type_id) + s("") + s("")


def pregame_block(tag: str = "LEKMOD v34.15", grid: tuple[int, int] = (4, 3), humans: int = 1, ais: int = 2,
                  nickname: str = "^Synth^@76561190000000001", turn: int = 5) -> bytes:
    """A CvPreGame::write block (saveSlotHints v3 + writeArchive v6) with MAX_PLAYERS=64 vectors and invented content."""
    n = 64
    status = [3] * humans + [1] * ais + [0] * (n - humans - ais)
    claims = [2 if st in (1, 3) else 0 for st in status]
    nicks = [nickname if st == 3 else "" for st in status]
    civs = [i % 7 for i in range(n)]
    civ_keys = [f"CIVILIZATION_SYNTH{i}" if st in (1, 3) else "" for i, st in enumerate(status)]
    leader_keys = [f"LEADER_SYNTH{i}" if st in (1, 3) else "" for i, st in enumerate(status)]
    teams = list(range(n))
    hand = [2 for _ in range(n)]
    empty_s = [""] * n
    out = b""
    # ---- saveSlotHints v3
    out += u32(3) + i32(2) + i32(1) + s("Assets\\Maps\\Synth.lua")
    out += vec_i32(civs) + vec_str(nicks) + vec_i32(status) + vec_i32(claims) + vec_i32(teams) + vec_i32(hand)
    out += vec_str(civ_keys) + vec_str(leader_keys)
    # ---- writeArchive v6 (field order of CvPreGame.cpp:3348)
    out += u32(6)
    out += i32(0) + s("") + i32(0) + s("") + vec_i32([-1] * n)           # active player, admin pw, adv start pts, alias, art styles
    out += b"\x00" + u32(0) + i32(0) + i32(-1) + i32(-1)                  # autorun, delay(float bits), turn limit, bandwidth, calendar
    out += info_base()                                                  # calendar info
    out += vec_str(empty_s) * 4                                         # adjectives, descriptions, passwords, short descriptions
    out += i32(-1) + info_base("CLIMATE_SYNTH") + b"".join(i32(0) for _ in range(4)) + b"".join(u32(0) for _ in range(7))
    out += i32(-1) + vec_str(empty_s) + u32(0) + vec_str(empty_s)       # era, emails, end-turn timer (float), flag decals
    out += vec_bool([False] * 7) + i32(-1) + s(tag) + i32(2)            # force controls, game mode, game name, game speed
    out += b"\x01" + i32(turn) + i32(1) + b"\x00" + i32(0)              # started, turn, game type, map type (u8), update time
    out += vec_i32(hand) + vec_i32(hand) + b"\x00\x00"                  # handicaps, last human handicaps, earth map, internet game
    out += vec_str(empty_s) + s("") + s("") + b"\x00" + u32(1234) + b"\x00\x00"   # leader names, load file, email, no players, seed, wb, override
    out += s("Assets\\Maps\\Synth.lua") + i32(0) + i32(0) + i32(2)      # map script, city elimination, max turns, minor civs
    out += vec_str(["MINOR_CIV_SYNTH_A", "MINOR_CIV_SYNTH_B"]) + vec_bool([False] * n) + b"\x00"
    out += vec_bool([False] * 5) + vec_i32([0] * n) + vec_str(nicks) + i32(3) + i32(0)   # mp options, net ids, nicknames, victories, pitboss
    out += vec_bool([True] * n) + vec_str(["PLAYERCOLOR_SYNTH"] * n)    # playable civs, player colors
    out += b"\x00\x00\x00" + i32(-1) + b"\x00\x00\x00" + vec_bool([False] * n)     # private, quick combat x2, quick handicap, quickstart, rand size, rand script, ready
    out += i32(-1) + info_base("SEALEVEL_SYNTH") + i32(0) + b"\x00"     # sea level, info (+1 int), dummy2
    out += vec_i32(claims) + vec_i32(status) + s("") + u32(99) + i32(0) + vec_i32(teams) + b"\x00"   # claims, status, smtp, sync seed, target score, teams, transferred
    out += info_base("TURNTIMER_SYNTH") + b"".join(i32(0) for _ in range(4)) + i32(-1) + b"\x00"   # turn timer, type, city screen blocked
    out += vec_bool([True] * 3) + vec_bool([False] * n)                 # victories, white flags
    out += i32(2) + info_base("WORLDSIZE_SYNTH") + b"".join(
        i32(v) for v in (4, 1, 8, 1, 0, 0, 0, 0, 0, grid[0], grid[1], 0, 0, 0, 100, 0, 0, 0, 0))   # world info v2 (19 ints; grid w/h at 10th/11th)
    out += i32(1)                                                       # world size
    out += u32(1) + s("GAMEOPTION_SYNTH") + i32(1) + u32(0)             # game options (1), map options (0)
    out += s("000000 SYNTHETIC") + vec_bool([False] * n) + vec_bool([False] * n) + vec_str(empty_s)
    return out


def exe_header(turn: int = 5, civ: str = "CIVILIZATION_SYNTH0", n_dlc: int = 2) -> bytes:
    out = b"CIV5" + u32(8) + s("1.0.3.279 (403694)") + s("403694") + u32(turn) + b"\x01"
    for p in (civ, "HANDICAP_SYNTH", "ERA_ANCIENT", "ERA_ANCIENT", "GAMESPEED_SYNTH", "WORLDSIZE_SYNTH", "Assets\\Maps\\Synth.lua"):
        out += s(p)
    out += u32(n_dlc)
    for i in range(n_dlc):
        out += bytes([i]) * 16 + u32(1) + s(f"Synthetic DLC {i}")
    out += u32(0)                                                       # mods
    out += s("") + s("") + s("PLAYERCOLOR_SYNTH") + FAKE_GUID_A + s("1.0.0") + FAKE_GUID_B + u32(0)
    return out


def synth_body(grid: tuple[int, int] = (4, 3), plot_extra: int = 52, tail: int = 4096, db_size: int = 256) -> bytes:
    """A body with the structure `segments.locate` looks for: game head, SQLite blob, map header, plots, unlocated rest."""
    w, h = grid
    head = i32(1) + i32(0) + i32(5) + i32(0) + i32(-1) + i32(-4000) + i32(0) + i32(0) + i32(777) + i32(0)   # first 10 CvGame ints
    game = head + b"\x00" * 200
    db = b"SQLite format 3\x00" + b"\x00" * (db_size - 16)
    game += u32(len(db)) + db
    hdr = u32(1) + i32(w) + i32(h) + i32(3) + i32(1) + i32(0) + i32(90) + i32(-90) + b"\x01\x00" + bytes(range(16))
    for _ in range(2):                                                  # two hashed resource arrays, 2 entries each
        hdr += u32(2) + u32(0xAAAA0001) + i32(1) + u32(0xAAAA0002) + i32(2)
    plots = b""
    for idx in range(w * h):
        y, x = divmod(idx, w)
        plots += u32(7) + struct.pack("<HH", x, y) + bytes((idx + k) & 0xFF for k in range(plot_extra))
    return game + hdr + plots + b"\x00" * tail


def assemble_save(header: bytes, pregame: bytes, body: bytes, level: int = 6, partial_flush: bool = False) -> bytes:
    """header + pregame + u32 version + 64 KiB chunks of ONE zlib stream (Z_SYNC_FLUSH end, no final block)."""
    c = zlib.compressobj(level, zlib.DEFLATED, 15, 8, zlib.Z_DEFAULT_STRATEGY)
    stream = c.compress(body) + (c.flush(zlib.Z_PARTIAL_FLUSH) if partial_flush else b"") + c.flush(zlib.Z_SYNC_FLUSH)
    out = bytearray(header + pregame + u32(ZSTREAM_VERSION))
    for i in range(0, len(stream), CHUNK_SIZE):
        chunk = stream[i:i + CHUNK_SIZE]
        out += u32(len(chunk)) + chunk
    return bytes(out)


def build_save(turn: int = 5, tag: str = "LEKMOD v34.15", body: bytes | None = None, **pregame_kw: Any) -> bytes:
    grid = pregame_kw.pop("grid", (4, 3))
    return assemble_save(exe_header(turn), pregame_block(tag=tag, grid=grid, turn=turn, **pregame_kw), body if body is not None else synth_body(grid))
