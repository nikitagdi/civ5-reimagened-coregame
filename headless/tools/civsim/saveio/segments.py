"""civsim.saveio.segments — HEURISTIC segment map of the decompressed body (body.bin).

The authoritative segmentation is the host's (MemDataStream::GetPosition after each ICvGame1/ICvMap1/... Read). Until the host
does it, this module finds the boundaries that can be located from the bytes alone, using structure known from the DLL source:

  game      CvGame::Write: version u32 (=1) + counters ... + embedded Civ5SavedGameDatabase.db as `u32 size + bytes`
            (blob starts with "SQLite format 3\\0"); the blob is the LAST field, so game_end = blob end   (EXACT, cross-checked)
  map       CvMap::Write starts right after the blob with `u32 1, i32 W, i32 H`; W,H may differ from the pregame world_info
            grid (the map script resizes the map: Lekmap Pangaea 74x66 on a "large" 104x64 world)             (EXACT)
  map.hdr   version, W, H, counters, wraps, GUID, two hashed resource arrays (u32 count; per entry u32 hash [+ i32 value])
  map.plots W*H CvPlot::write records. Each starts `u32 7, i16 x, i16 y`; a plot is 1573 bytes, +8 per unit on the plot (and +n
            for script data), so plot starts are found by scanning for the next expected (7,x,y)                (EXACT starts)
  rest      map tail (areas, landmasses, AI hints), teams, players, supporting data: boundaries NOT located. Observed: a
            hashed-array pattern repeats every 13396 bytes 64 times from ~0.9 MB after the last plot; the player records
            are ~15 MB of mostly zeros.

Everything here is labelled "heuristic" in the output; a failed step yields fewer segments, never an exception.
"""
from __future__ import annotations

import hashlib
import struct
from typing import Any

SQLITE_MAGIC = b"SQLite format 3\x00"
PLOT_VERSION_HINT = 7


def _sha(b: memoryview | bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def _game_head(body: bytes) -> dict[str, int]:
    """First ints of CvGame::Write (order from CvGame.cpp:10918+): version, endTurnMessagesSent, elapsedGameTurns, startTurn,
    winningTurn, startYear, estimateEndTurn, defaultEstimateEndTurn, turnSlice, cutoffSlice."""
    names = ("version", "end_turn_messages_sent", "elapsed_game_turns", "start_turn", "winning_turn", "start_year",
             "estimate_end_turn", "default_estimate_end_turn", "turn_slice", "cutoff_slice")
    vals = struct.unpack_from("<10i", body, 0)
    return dict(zip(names, vals))


def _scan_hashed_array(body: bytes, pos: int) -> int | None:
    """WriteHashedDataArray with one int per entry: u32 count, then per entry u32 hash and (if hash != 0) i32 value."""
    if pos + 4 > len(body):
        return None
    n = struct.unpack_from("<I", body, pos)[0]
    if n > 4096:
        return None
    pos += 4
    for _ in range(n):
        if pos + 4 > len(body):
            return None
        h = struct.unpack_from("<I", body, pos)[0]
        pos += 4
        if h:
            pos += 4
    return pos if pos <= len(body) else None


def locate(body: bytes, grid: tuple[int, int] | None, turn: int | None = None, with_hashes: bool = True) -> dict[str, Any]:
    """Return {segments: [...], game_head: {...}, notes: [...], plots: {...}}. `grid` = (width, height) from the pregame world_info."""
    out: dict[str, Any] = {"heuristic": True, "body_size": len(body), "segments": [], "notes": []}
    segs: list[dict[str, Any]] = out["segments"]
    notes: list[str] = out["notes"]

    def add(name: str, off: int, end: int, conf: str, note: str = "") -> None:
        s: dict[str, Any] = {"name": name, "offset": off, "length": end - off, "confidence": conf}
        if note:
            s["note"] = note
        if with_hashes and end - off > 0:
            s["sha256"] = _sha(memoryview(body)[off:end])
        segs.append(s)

    if len(body) < 64:
        notes.append("body too short")
        return out
    head = _game_head(body)
    out["game_head"] = head
    if head["version"] != 1:
        notes.append(f"CvGame version {head['version']} (expected 1): game_head not trusted")
    elif turn is not None and head["elapsed_game_turns"] != turn:
        notes.append(f"elapsed_game_turns {head['elapsed_game_turns']} != header turn {turn}")

    # ---- embedded SQLite blob -> game end ---------------------------------------------------
    i = body.find(SQLITE_MAGIC)
    if i < 4:
        notes.append("embedded SQLite database not found; no further segmentation")
        add("body", 0, len(body), "none")
        return out
    size = struct.unpack_from("<I", body, i - 4)[0]
    db_end = i + size
    if db_end > len(body):
        notes.append(f"SQLite blob size {size} runs past the body")
        add("body", 0, len(body), "none")
        return out
    # ---- map header --------------------------------------------------------------------------
    map_start = db_end
    ver, w, h = struct.unpack_from("<Iii", body, map_start)
    if ver != 1 or not (0 < w <= 512 and 0 < h <= 512):
        notes.append(f"no CvMap header at the end of the SQLite blob (u32 {ver}, {w}x{h})")
        add("game", 0, db_end, "medium", "CvGame::Write incl. embedded db; map not located")
        add("rest", db_end, len(body), "none")
        return out
    if grid is not None and (w, h) != tuple(grid):
        notes.append(f"map grid {w}x{h} differs from the pregame world_info grid {grid[0]}x{grid[1]} (map scripts may resize the map)")
    add("game.fields", 0, i - 4, "high", "CvGame::Write up to the db size field")
    add("game.sqlite_size", i - 4, i, "high", f"u32 = {size}")
    add("game.sqlite", i, db_end, "high", "embedded Civ5SavedGameDatabase.db")
    pos = map_start + 12
    land, owned, nat, top, bottom = struct.unpack_from("<iiiii", body, pos)
    pos += 20 + 2 + 16          # + wrapX, wrapY (bytes) + GUID
    out["map"] = {"version": ver, "width": w, "height": h, "land_plots": land, "owned_plots": owned, "natural_wonders": nat,
                  "top_latitude": top, "bottom_latitude": bottom, "wrap_x": body[map_start + 32], "wrap_y": body[map_start + 33]}
    p = _scan_hashed_array(body, pos)
    p = _scan_hashed_array(body, p) if p is not None else None
    if p is None:
        notes.append("resource arrays of the map header could not be walked")
        add("map", map_start, len(body), "low", "map header only partially understood")
        return out
    plots_start = p
    add("map.header", map_start, plots_start, "high", "CvMap::Write up to the first plot (counters, GUID, 2 hashed resource arrays)")
    # ---- plots ---------------------------------------------------------------------------------
    pv = struct.unpack_from("<I", body, plots_start)[0]
    x0, y0 = struct.unpack_from("<HH", body, plots_start + 4)
    if (x0, y0) != (0, 0):
        notes.append(f"first plot header is ({pv}, {x0}, {y0}), expected (v, 0, 0)")
        add("map.rest", plots_start, len(body), "none")
        return out
    n_plots = w * h
    starts = [plots_start]
    stride = None
    for idx in range(1, n_plots):
        y, x = divmod(idx, w)
        pat = struct.pack("<IHH", pv, x, y)
        lo = starts[-1] + (max(12, stride - 64) if stride else 12)
        j = body.find(pat, lo, starts[-1] + 1 + (1 << 16))
        if j < 0:
            break
        if stride is None:
            stride = j - starts[0]
        starts.append(j)
    out["plots"] = {"expected": n_plots, "found": len(starts), "first_offset": plots_start, "version": pv}
    if stride:
        lens = [starts[k + 1] - starts[k] for k in range(len(starts) - 1)]
        hist: dict[int, int] = {}
        for ln in lens:
            hist[ln] = hist.get(ln, 0) + 1
        common = max(hist, key=lambda k: hist[k])
        out["plots"]["stride"] = common
        out["plots"]["length_histogram"] = {str(k): v for k, v in sorted(hist.items(), key=lambda kv: -kv[1])[:6]}
    if len(starts) != n_plots or not stride:
        notes.append(f"plot scan found {len(starts)} of {n_plots} plots")
        add("map.plots", plots_start, starts[-1], "low", f"{len(starts)} of {n_plots} plot starts found")
        add("map.rest", starts[-1], len(body), "none")
        return out
    plots_end = starts[-1] + out["plots"]["stride"]
    add("map.plots", plots_start, plots_end, "high",
        f"{n_plots} CvPlot records, stride {out['plots']['stride']} (+8 per unit on a plot); end assumes the last plot has the common length")
    add("rest", plots_end, len(body), "none",
        "map tail (areas, landmasses, AI hints), teams, players, supporting data: boundaries not located")
    return out


def render_text(seg: dict[str, Any]) -> list[str]:
    L = ["", "body segments (HEURISTIC; authoritative map comes from the host's Read positions):"]
    for s in seg["segments"]:
        L.append(f"  0x{s['offset']:08x} +{s['length']:<10d} {s['confidence']:<6s} {s['name']:<18s} {s.get('note', '')}")
    gh = seg.get("game_head")
    if gh and gh.get("version") == 1:
        L.append(f"  game head: elapsed_game_turns={gh['elapsed_game_turns']} start_year={gh['start_year']} turn_slice={gh['turn_slice']}")
    if "plots" in seg:
        pl = seg["plots"]
        L.append(f"  plots: {pl['found']}/{pl['expected']} found, stride {pl.get('stride')}, length histogram {pl.get('length_histogram')}")
    for n in seg["notes"]:
        L.append(f"  note: {n}")
    return L
