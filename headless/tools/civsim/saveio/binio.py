"""civsim.saveio.binio — little-endian reader that records every consumed byte range as a named Span.

Design rule: every byte of a save is either a parsed field or a named opaque blob.
`Reader` is the only way the parsers consume bytes, so span coverage is a by-product that `check_coverage` verifies.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import Any

# kinds: parsed = decoded by us; opaque = carried as bytes, meaning unknown; dll = written by the DLL (CvPreGame::write / body)
# zlib = the compressed stream. `owner` is who writes it (exe / dll / zlib).
KINDS = ("parsed", "opaque", "dll", "zlib")


class SaveFormatError(Exception):
    """The bytes do not match the expected layout. `offset` is where parsing stopped."""

    def __init__(self, msg: str, offset: int | None = None):
        super().__init__(msg if offset is None else f"{msg} (at 0x{offset:x})")
        self.msg = msg
        self.offset = offset


@dataclass
class Span:
    name: str
    off: int
    len: int
    kind: str               # one of KINDS
    owner: str = "exe"      # exe | dll | zlib
    note: str = ""

    @property
    def end(self) -> int:
        return self.off + self.len

    def to_json(self) -> dict[str, Any]:
        d: dict[str, Any] = {"name": self.name, "offset": self.off, "length": self.len, "kind": self.kind, "owner": self.owner}
        if self.note:
            d["note"] = self.note
        return d


MAX_STR = 1 << 20


class Reader:
    """Cursor over bytes. Named reads append a Span to `spans` and (optionally) a value to `fields`."""

    def __init__(self, data: bytes, pos: int = 0, end: int | None = None,
                 spans: list[Span] | None = None, fields: dict[str, Any] | None = None,
                 kind: str = "parsed", owner: str = "exe"):
        self.data = data
        self.pos = pos
        self.end = len(data) if end is None else end
        self.spans: list[Span] = spans if spans is not None else []
        self.fields: dict[str, Any] = fields if fields is not None else {}
        self.kind = kind
        self.owner = owner

    # -- raw access --------------------------------------------------------
    def remaining(self) -> int:
        return self.end - self.pos

    def _take(self, n: int, what: str) -> bytes:
        if n < 0 or self.pos + n > self.end:
            raise SaveFormatError(f"truncated while reading {what} ({n} bytes, {self.remaining()} left)", self.pos)
        b = self.data[self.pos:self.pos + n]
        self.pos += n
        return b

    def _span(self, name: str | None, start: int, kind: str | None = None, note: str = "") -> None:
        if name:
            self.spans.append(Span(name, start, self.pos - start, kind or self.kind, self.owner, note))

    def _field(self, name: str | None, value: Any, store: bool) -> Any:
        if name and store:
            self.fields[name] = value
        return value

    # -- scalars -------------------------------------------------------------
    def u8(self, name: str | None = None, store: bool = True) -> int:
        s = self.pos
        v = self._take(1, name or "u8")[0]
        self._span(name, s)
        return self._field(name, v, store)

    def u32(self, name: str | None = None, store: bool = True) -> int:
        s = self.pos
        v = struct.unpack_from("<I", self._take(4, name or "u32"))[0]
        self._span(name, s)
        return self._field(name, v, store)

    def i32(self, name: str | None = None, store: bool = True) -> int:
        s = self.pos
        v = struct.unpack_from("<i", self._take(4, name or "i32"))[0]
        self._span(name, s)
        return self._field(name, v, store)

    def f32(self, name: str | None = None, store: bool = True) -> float:
        s = self.pos
        v = struct.unpack_from("<f", self._take(4, name or "f32"))[0]
        self._span(name, s)
        # floats are kept as their raw u32 bit pattern in `fields` (no float ever enters the sim-facing JSON)
        return self._field(name, struct.unpack_from("<I", self.data, s)[0], store)

    def boolean(self, name: str | None = None, store: bool = True) -> bool:
        s = self.pos
        v = self._take(1, name or "bool")[0]
        if v > 1:
            raise SaveFormatError(f"{name or 'bool'}: byte {v:#x} is not 0/1", s)
        self._span(name, s)
        return self._field(name, bool(v), store)

    # -- strings / blobs -------------------------------------------------------
    def string(self, name: str | None = None, store: bool = True, max_len: int = MAX_STR) -> str:
        """u32 length + bytes (FDataStream string; length excludes any terminator)."""
        s = self.pos
        n = struct.unpack_from("<I", self._take(4, (name or "str") + ".len"))[0]
        if n > max_len or n > self.remaining():
            raise SaveFormatError(f"{name or 'string'}: implausible length {n}", s)
        raw = self._take(n, name or "string")
        self._span(name, s)
        return self._field(name, raw.decode("utf-8", errors="replace"), store)

    def raw_string(self, name: str | None = None, max_len: int = MAX_STR) -> bytes:
        s = self.pos
        n = struct.unpack_from("<I", self._take(4, (name or "str") + ".len"))[0]
        if n > max_len or n > self.remaining():
            raise SaveFormatError(f"{name or 'string'}: implausible length {n}", s)
        raw = self._take(n, name or "string")
        self._span(name, s)
        return raw

    def blob(self, n: int, name: str | None = None, kind: str | None = None, note: str = "") -> bytes:
        s = self.pos
        b = self._take(n, name or "blob")
        self._span(name, s, kind, note)
        return b

    def guid(self, name: str | None = None, store: bool = True) -> str:
        s = self.pos
        b = self._take(16, name or "guid")
        self._span(name, s)
        # Windows GUID text form (Data1/2/3 little-endian), matches what the game's DLC DB stores
        d1, d2, d3 = struct.unpack_from("<IHH", b)
        text = f"{d1:08X}-{d2:04X}-{d3:04X}-{b[8:10].hex().upper()}-{b[10:].hex().upper()}"
        return self._field(name, text, store)

    # -- counted vectors (FStlContainerSerialization: u32 count, then elements) ----
    def _count(self, name: str, elem_min: int) -> int:
        s = self.pos
        n = struct.unpack_from("<I", self._take(4, name + ".count"))[0]
        if n * elem_min > self.remaining():
            raise SaveFormatError(f"{name}: implausible element count {n}", s)
        return n

    def vec_i32(self, name: str, store: bool = True) -> list[int]:
        s = self.pos
        n = self._count(name, 4)
        vals = list(struct.unpack_from(f"<{n}i", self._take(4 * n, name))) if n else []
        self._span(name, s)
        return self._field(name, vals, store)

    def vec_bool(self, name: str, store: bool = True) -> list[bool]:
        s = self.pos
        n = self._count(name, 1)
        raw = self._take(n, name)
        if any(c > 1 for c in raw):
            raise SaveFormatError(f"{name}: non-boolean byte", s)
        self._span(name, s)
        return self._field(name, [bool(c) for c in raw], store)

    def vec_str(self, name: str, store: bool = True) -> list[str]:
        s = self.pos
        n = self._count(name, 4)
        out = []
        for _ in range(n):
            ln = struct.unpack_from("<I", self._take(4, name + ".len"))[0]
            if ln > self.remaining():
                raise SaveFormatError(f"{name}: implausible string length {ln}", self.pos - 4)
            out.append(self._take(ln, name).decode("utf-8", errors="replace"))
        self._span(name, s)
        return self._field(name, out, store)


def check_coverage(spans: list[Span], total: int) -> list[str]:
    """Problems with a span list: gaps, overlaps, out-of-range. Empty list = every byte accounted for exactly once."""
    problems: list[str] = []
    cur = 0
    for sp in sorted(spans, key=lambda x: (x.off, x.len)):
        if sp.len < 0 or sp.off < 0 or sp.end > total:
            problems.append(f"{sp.name}: out of range [{sp.off}, {sp.end}) of {total}")
            continue
        if sp.off > cur:
            problems.append(f"gap [{cur:#x}, {sp.off:#x}) before {sp.name}")
        elif sp.off < cur:
            problems.append(f"overlap at {sp.off:#x}: {sp.name}")
        cur = max(cur, sp.end)
    if cur < total:
        problems.append(f"gap [{cur:#x}, {total:#x}) at end")
    return problems
