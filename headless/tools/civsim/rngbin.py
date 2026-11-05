"""Readers and comparers for civhost's RNG artifacts (diag DLLs only): rng-digest.bin, rng.log.bin, rng.sites.tsv.

rng-digest.bin  "CRNG", u32 version=1, u32 n_kinds=5, u32 record_size=128; records {i32 turn, u32 flags, 5 x {u32 calls,
                u32 reseeds, u32 first_seed, u32 last_seed, u64 stream_hash}}. turn -1 = pregame (flag 1), -2 = calls outside
                any phase (flag 2). Kinds: jon, map, async, other-sync, other-async.
rng.log.bin     "CRLG", u32 version=1, u32 record_size=20, u32 flags; records {u16 turn (0xFFFF pregame), u16 slice, u8 rng
                (bit 0x80 = reseed), u8 zone, u16 site, u32 call_index, u32 seed_before, u16 range, u16 result}.
rng.sites.tsv   site, kind, calls, ret, rva, function+disp, file, line, log text (one row per site id).

The first differing call is the first index where kind, range, result, seed or the pszLog text differ; the site address and the
function are shown for context but not compared (they differ between DLL builds).
"""
from __future__ import annotations

import struct
from pathlib import Path

KINDS = ("jon", "map", "async", "other-sync", "other-async")
_DIGEST_HDR = struct.Struct("<4sIII")
_KIND = struct.Struct("<IIIIQ")
_LOG_HDR = struct.Struct("<4sIII")
_LOG_REC = struct.Struct("<HHBBHIIHH")


def parse_digest(path: Path) -> list[dict]:
    d = Path(path).read_bytes()
    magic, ver, nk, rs = _DIGEST_HDR.unpack_from(d, 0)
    if magic != b"CRNG" or ver != 1 or nk != len(KINDS) or rs != 8 + nk * _KIND.size:
        raise ValueError(f"{path}: not an rng-digest.bin v1")
    out = []
    for off in range(_DIGEST_HDR.size, len(d) - rs + 1, rs):
        turn, flags = struct.unpack_from("<iI", d, off)
        kinds = [dict(zip(("calls", "reseeds", "first_seed", "last_seed", "hash"), _KIND.unpack_from(d, off + 8 + i * _KIND.size))) for i in range(nk)]
        out.append({"turn": turn, "flags": flags, "kinds": kinds})
    return out


def digest_divergence(a: list[dict], b: list[dict]) -> dict | None:
    """First phase (aligned by position, then checked for equal turn numbers) whose per-kind records differ; None when equal over
    the common prefix."""
    for i, (ra, rb) in enumerate(zip(a, b)):
        if ra["turn"] != rb["turn"]:
            return {"index": i, "turn": ra["turn"], "reason": f"phase order differs ({ra['turn']} vs {rb['turn']})", "kinds": []}
        diff = [KINDS[k] for k in range(len(KINDS)) if ra["kinds"][k] != rb["kinds"][k]]
        if diff:
            return {"index": i, "turn": ra["turn"], "reason": "stream differs", "kinds": diff,
                    "a": {KINDS[k]: ra["kinds"][k] for k in range(len(KINDS)) if KINDS[k] in diff},
                    "b": {KINDS[k]: rb["kinds"][k] for k in range(len(KINDS)) if KINDS[k] in diff}}
    return None


def parse_log(path: Path) -> list[tuple]:
    d = Path(path).read_bytes()
    magic, ver, rs, _flags = _LOG_HDR.unpack_from(d, 0)
    if magic != b"CRLG" or ver != 1 or rs != _LOG_REC.size:
        raise ValueError(f"{path}: not an rng.log.bin v1")
    return list(_LOG_REC.iter_unpack(d[_LOG_HDR.size:_LOG_HDR.size + (len(d) - _LOG_HDR.size) // rs * rs]))


def parse_sites(path: Path) -> dict[int, dict]:
    out: dict[int, dict] = {}
    p = Path(path)
    if not p.is_file():
        return out
    for ln in p.read_text(encoding="utf-8", errors="replace").splitlines()[1:]:
        f = ln.split("\t")
        if len(f) >= 9:
            out[int(f[0])] = {"kind": f[1], "calls": int(f[2]), "ret": f[3], "rva": f[4], "function": f[5], "file": f[6], "line": int(f[7] or 0), "log": f[8]}
    return out


def _fmt(rec: tuple, sites: dict[int, dict]) -> dict:
    turn, slice_, rng, zone, site, call_index, seed, rng_range, result = rec
    s = sites.get(site) or {}
    return {"turn": -1 if turn == 0xFFFF else turn, "slice": slice_, "kind": KINDS[rng & 0x7F] if (rng & 0x7F) < len(KINDS) else "?",
            "reseed": bool(rng & 0x80), "zone": None if zone == 0xFF else zone, "site": site, "call_index": call_index, "seed_before": seed,
            "range": rng_range, "result": result, "function": s.get("function"), "file": s.get("file"), "line": s.get("line"), "log": s.get("log")}


def _read_records(path: Path, first: int, count: int) -> list[tuple]:
    with open(path, "rb") as f:
        f.seek(_LOG_HDR.size + first * _LOG_REC.size)
        d = f.read(count * _LOG_REC.size)
    return list(_LOG_REC.iter_unpack(d[:len(d) // _LOG_REC.size * _LOG_REC.size]))


def log_divergence(a_path: Path, b_path: Path, context: int = 20, chunk_records: int = 1 << 17) -> dict:
    """First differing call between two rng.log.bin (+ rng.sites.tsv beside them) and +-context calls on both sides.
    Streams the files (a pregame log of a Lekmap game has 16 million records): identical chunks are skipped by a byte comparison, only
    a differing chunk is decoded."""
    a_path, b_path = Path(a_path), Path(b_path)
    sa = parse_sites(a_path.with_name(a_path.name.replace(".log.bin", ".sites.tsv")))
    sb = parse_sites(b_path.with_name(b_path.name.replace(".log.bin", ".sites.tsv")))

    def key(rec: tuple, sites: dict[int, dict]) -> tuple:
        _turn, _slice, rng, _zone, site, _ci, seed, rng_range, result = rec
        return (rng, rng_range, result, seed, (sites.get(site) or {}).get("log", ""))

    n_a = (a_path.stat().st_size - _LOG_HDR.size) // _LOG_REC.size
    n_b = (b_path.stat().st_size - _LOG_HDR.size) // _LOG_REC.size
    first = None
    reason = "identical"
    csize = chunk_records * _LOG_REC.size
    with open(a_path, "rb") as fa, open(b_path, "rb") as fb:
        fa.seek(_LOG_HDR.size)
        fb.seek(_LOG_HDR.size)
        idx = 0
        while True:
            ca, cb = fa.read(csize), fb.read(csize)
            if not ca and not cb:
                break
            if ca != cb:
                na, nb = len(ca) // _LOG_REC.size, len(cb) // _LOG_REC.size
                ra = list(_LOG_REC.iter_unpack(ca[:na * _LOG_REC.size]))
                rb = list(_LOG_REC.iter_unpack(cb[:nb * _LOG_REC.size]))
                for j in range(min(na, nb)):
                    if key(ra[j], sa) != key(rb[j], sb):
                        first, reason = idx + j, "call differs"
                        break
                if first is None and na != nb:
                    first, reason = idx + min(na, nb), "one log is shorter (the other side makes more calls)"
                if first is not None:
                    break
            idx += len(ca) // _LOG_REC.size
    out: dict = {"calls_a": n_a, "calls_b": n_b, "first_diff_index": first, "reason": reason}
    if first is None:
        return out
    lo = max(0, first - context)
    out["a"] = [_fmt(r, sa) for r in _read_records(a_path, lo, 2 * context + 1)]
    out["b"] = [_fmt(r, sb) for r in _read_records(b_path, lo, 2 * context + 1)]
    out["context_start"] = lo
    return out
