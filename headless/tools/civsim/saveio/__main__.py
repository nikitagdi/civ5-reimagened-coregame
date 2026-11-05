"""civsim.saveio — `.Civ5Save` envelope inspection, corpus snapshot and extraction.

Usage:
  python -m civsim.saveio corpus snapshot --saves <Documents\\My Games\\Sid Meier's Civilization 5\\Saves> [--extra <dir> ...] [--out <dir>]
        read-only copy of every *.Civ5Save to headless/out/corpus/saves/<sha256>.Civ5Save + index.json
  python -m civsim.saveio corpus stats [--index <index.json>] [--json]
  python -m civsim.saveio inspect <save|sha256-prefix> [--json] [--segments]
        --segments adds the byte-span table and the HEURISTIC body segment map (game/sqlite/map/plots)
  python -m civsim.saveio extract <save|sha256-prefix> --out <dir> [--with-zstream]
        header.bin, pregame.bin, body.bin, envelope.json (input of the civhost import)
exit: 0 ok (any parse status), 1 error / unreadable, 2 usage
Reports never print nicknames or SteamIDs (masked as player#N / steamid#).
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from .. import common
from . import corpus as corpus_mod
from . import segments as seg_mod
from .envelope import parse_envelope, summarize
from .extract import _slots, extract
from .report import group_spans, public_fields, render_text


def _resolve_save(arg: str) -> Path:
    p = Path(arg)
    if p.is_file():
        return p
    store = corpus_mod.default_corpus_dir() / "saves"
    if store.is_dir() and len(arg) >= 6:
        hits = sorted(store.glob(f"{arg.lower()}*.Civ5Save"))
        if len(hits) == 1:
            return hits[0]
        if len(hits) > 1:
            raise SystemExit(f"sha prefix {arg!r} is ambiguous ({len(hits)} corpus files)")
    raise SystemExit(f"save not found: {arg}")


def _grid(env):
    wi = env.fields.get("archive.world_info")
    return (wi["grid_width"], wi["grid_height"]) if isinstance(wi, dict) and "grid_width" in wi else None


def cmd_inspect(args: argparse.Namespace) -> int:
    path = _resolve_save(args.save)
    data = path.read_bytes()
    env = parse_envelope(data)
    summary = summarize(env)
    groups = group_spans(env.spans)
    segs = None
    if args.segments and env.inflate is not None and env.inflate.body:
        segs = seg_mod.locate(env.inflate.body, _grid(env), env.fields.get("hdr.turn"))
    problems = env.coverage_problems()
    if args.json:
        j = {"summary": summary, "groups": groups, "slots": _slots(env), "coverage_problems": problems,
             "fields": public_fields(env.fields)}
        if args.segments:
            j["spans"] = [s.to_json() for s in env.spans]
            j["body_segments"] = segs
        print(json.dumps(j, indent=2, ensure_ascii=False))
        return 0
    extra = []
    slots = _slots(env)
    if slots:
        extra.append("")
        extra.append("active slots (nicknames masked):")
        for s in slots:
            if s["status"] in ("COMPUTER", "TAKEN", "OBSERVER"):
                extra.append(f"  slot {s['slot']:2d} {s['status']:<8s} {s['claim']:<10s} team {s['team']:<3d} {s['civ']}  {s['leader']}"
                             + (f"  [{s['nickname']}]" if s["nickname"] else ""))
    extra.append("")
    extra.append("byte coverage: " + ("every byte accounted for" if not problems else "PROBLEMS: " + "; ".join(problems[:5])))
    if segs:
        extra.extend(seg_mod.render_text(segs))
    print(render_text(summary, groups if args.segments else None, extra))
    if args.segments:
        print("")
        print(f"leaf spans: {len(env.spans)} (chunk spans omitted here; --json lists all)")
        for sp in env.spans:
            if not sp.name.startswith("zstream.chunk["):
                print(f"  0x{sp.off:08x} +{sp.len:<8d} {sp.kind:<6s} {sp.name}")
    return 0 if not problems else 1


def cmd_extract(args: argparse.Namespace) -> int:
    path = _resolve_save(args.save)
    out = Path(args.out)
    if "my games" in str(out.resolve()).lower():
        print("refusing to write under Documents\\My Games", file=sys.stderr)
        return 2
    try:
        out.resolve().relative_to(common.OUT.resolve())
    except ValueError:
        if not args.allow_outside_out:
            print(f"refusing: {out} is outside {common.OUT} (use --allow-outside-out)", file=sys.stderr)
            return 2
    j = extract(path.read_bytes(), out, with_zstream=args.with_zstream)
    print(f"extracted {j['source']['sha256'][:12]} ({j['source']['status']}) -> {out}")
    for name, f in j["files"].items():
        print(f"  {name:<12s} {f['length']:>10d} bytes  sha256 {f['sha256'][:16]}")
    print("  note: pregame.bin holds raw nicknames/SteamIDs; keep it under headless/out (git-ignored)")
    return 0


def cmd_corpus(args: argparse.Namespace) -> int:
    if args.corpus_cmd == "snapshot":
        out = Path(args.out) if args.out else None
        corpus_mod.snapshot(Path(args.saves), [Path(p) for p in (args.extra or [])], out)
        return 0
    idx_path = Path(args.index) if args.index else corpus_mod.default_corpus_dir() / "index.json"
    if not idx_path.is_file():
        print(f"no index at {idx_path}; run `corpus snapshot` first", file=sys.stderr)
        return 1
    st = corpus_mod.stats(common.read_json(idx_path))
    if args.json:
        print(json.dumps(st, indent=2))
        return 0
    print(f"files {st['files']}, unique {st['unique']}")
    for k in ("supported", "no-body", "unsupported", "error"):
        d = st[k]
        line = f"  {k:<12s} {d['count']:>3d}"
        if d["count"]:
            line += f"  size {d['size_min']}..{d['size_max']}"
            if "turn_min" in d:
                line += f"  turn {d['turn_min']}..{d['turn_max']}"
            if "body_min" in d:
                line += f"  body {d['body_min']}..{d['body_max']}"
            if "players_min" in d:
                line += f"  players {d['players_min']}..{d['players_max']}"
        print(line)
        for r, n in (d.get("reasons") or {}).items():
            if r != "-":
                print(f"      {n:>3d} x {r}")
    return 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(prog="python -m civsim.saveio", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    pc = sub.add_parser("corpus", help="snapshot / stats")
    cs = pc.add_subparsers(dest="corpus_cmd", required=True)
    ps = cs.add_parser("snapshot")
    ps.add_argument("--saves", required=True, help="the game's Saves directory (read only)")
    ps.add_argument("--extra", action="append", help="additional read-only source roots (e.g. ~/Civ5Debug)")
    ps.add_argument("--out", help="corpus directory (default headless/out/corpus)")
    pt = cs.add_parser("stats")
    pt.add_argument("--index")
    pt.add_argument("--json", action="store_true")
    pi = sub.add_parser("inspect")
    pi.add_argument("save")
    pi.add_argument("--json", action="store_true")
    pi.add_argument("--segments", action="store_true")
    pe = sub.add_parser("extract")
    pe.add_argument("save")
    pe.add_argument("--out", required=True)
    pe.add_argument("--with-zstream", action="store_true")
    pe.add_argument("--allow-outside-out", action="store_true", help="allow --out outside headless/out (never under Documents\\My Games)")
    args = ap.parse_args(argv)
    try:
        if args.cmd == "corpus":
            return cmd_corpus(args)
        if args.cmd == "inspect":
            return cmd_inspect(args)
        return cmd_extract(args)
    except BrokenPipeError:
        return 0
    except (OSError, ValueError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
