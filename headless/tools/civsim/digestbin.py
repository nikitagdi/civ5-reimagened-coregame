"""civsim.digestbin — reader/comparer for digest.bin v1 (written by civhost, see headless/host/src/diag/HstDigest.h).

Layout (little-endian): magic "CSDG", u16 version=1, u16 flags, u32 n_segments, n_segments x (u16 len, utf8 name),
then per completed turn: u32 turn, u64 hash[n_segments]. A truncated last record (host crash mid-write) is ignored.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field
from pathlib import Path

MAGIC = b"CSDG"


@dataclass
class Digest:
    segments: list[str]
    turns: list[int] = field(default_factory=list)
    hashes: list[tuple[int, ...]] = field(default_factory=list)
    flags: int = 0
    version: int = 1
    truncated: bool = False

    def by_turn(self) -> dict[int, tuple[int, ...]]:
        return dict(zip(self.turns, self.hashes))

    @property
    def last_turn(self) -> int | None:
        return self.turns[-1] if self.turns else None


def parse(src: Path | str | bytes) -> Digest:
    data = src if isinstance(src, (bytes, bytearray)) else Path(src).read_bytes()
    if len(data) < 12 or data[:4] != MAGIC:
        raise ValueError("not a digest.bin (bad magic)")
    version, flags, n = struct.unpack_from("<HHI", data, 4)
    if version != 1:
        raise ValueError(f"unsupported digest.bin version {version}")
    off = 12
    names = []
    for _ in range(n):
        (ln,) = struct.unpack_from("<H", data, off)
        off += 2
        names.append(data[off:off + ln].decode("utf-8"))
        off += ln
    d = Digest(segments=names, flags=flags, version=version)
    rec = 4 + 8 * n
    while off + rec <= len(data):
        (t,) = struct.unpack_from("<I", data, off)
        d.turns.append(t)
        d.hashes.append(struct.unpack_from(f"<{n}Q", data, off + 4))
        off += rec
    d.truncated = off != len(data)
    return d


def build(segments: list[str], records: list[tuple[int, list[int]]], flags: int = 0) -> bytes:
    """Writer (fake host and tests)."""
    out = bytearray(MAGIC + struct.pack("<HHI", 1, flags, len(segments)))
    for s in segments:
        b = s.encode("utf-8")
        out += struct.pack("<H", len(b)) + b
    for t, hs in records:
        out += struct.pack("<I", t) + struct.pack(f"<{len(segments)}Q", *hs)
    return bytes(out)


def compare(a: Digest, b: Digest) -> dict:
    """First divergent turn and segments over the turns both digests contain (header of a/b is not compared beyond names)."""
    ta, tb = a.by_turn(), b.by_turn()
    common = sorted(set(ta) & set(tb))
    ia = {n: i for i, n in enumerate(a.segments)}
    ib = {n: i for i, n in enumerate(b.segments)}
    shared = [n for n in a.segments if n in ib]
    res = {
        "equal": True, "common_turns": len(common),
        "turns_a": [a.turns[0], a.turns[-1]] if a.turns else None, "turns_b": [b.turns[0], b.turns[-1]] if b.turns else None,
        "length_mismatch": set(ta) != set(tb), "segments_mismatch": a.segments != b.segments,
        "first_divergent_turn": None, "divergent_segments": [], "n_divergent_turns": 0, "n_divergent_segments_total": 0,
    }
    for t in common:
        bad = [n for n in shared if ta[t][ia[n]] != tb[t][ib[n]]]
        if bad:
            if res["first_divergent_turn"] is None:
                res["first_divergent_turn"] = t
                res["divergent_segments"] = bad
            res["n_divergent_turns"] += 1
            res["n_divergent_segments_total"] += len(bad)
    res["equal"] = res["n_divergent_turns"] == 0 and not res["segments_mismatch"]    # no divergence over the common turns
    res["identical"] = res["equal"] and not res["length_mismatch"] and bool(common)  # same turns, same hashes
    return res
