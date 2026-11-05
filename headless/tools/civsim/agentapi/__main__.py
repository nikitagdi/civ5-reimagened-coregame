"""civsim.agentapi - agent command dictionary V1 (contract only: documents + Python tools, no host/DLL code).

Usage:
  python -m civsim.agentapi check                       # schemas, canonical form, semantic rules, FIX-VALIDATION alignment, dict_hash
  python -m civsim.agentapi coverage [--dll-src DIR] [--lua-root DIR ...] [--report out.md]
  python -m civsim.agentapi render [--check]            # generated command reference (Markdown)
  python -m civsim.agentapi generate [--dll-src DIR]    # rewrite headless/agent-api/*.json from the DLL headers + overlay.py

Exit codes: 0 ok; 1 generated document out of date (render --check); 2 incomplete dictionary or configuration error.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from . import canon, coverage, dllparse, gen, render, schema


def _fixes() -> list[dict] | None:
    p = canon.REPO / "headless" / "improvements" / "fixes.json"
    return json.loads(p.read_text(encoding="utf-8"))["fixes"] if p.exists() else None


def run_check(api_dir: Path | None = None) -> tuple[int, list[str], str]:
    errs: list[str] = []
    docs = {}
    for n in canon.FILES:
        try:
            docs[n] = canon.load(n, api_dir)
        except (OSError, json.JSONDecodeError) as e:
            errs.append(f"{n}.json: {e}")
            continue
        errs += [f"{n}.json: {e}" for e in schema.validate(docs[n], schema.SCHEMAS[n])]
        if not canon.is_canonical(n, api_dir):
            errs.append(f"{n}.json: not in canonical form (sorted keys, 2 spaces, UTF-8, LF); run 'generate'")
    h = ""
    if "commands" in docs:
        if not errs:
            errs += schema.check_commands(docs["commands"], _fixes())
            if (canon.DLL_SRC / dllparse.HANDLER_CPP).exists():
                errs += coverage.check_guards(docs["commands"], canon.DLL_SRC)
            codes = {c["code"] for c in docs.get("errors", {}).get("codes", [])}
            for need in ("unknown_command", "bad_args", "out_of_range", "string_invalid", "reserved_value", "forbidden", "not_owner"):
                if need not in codes:
                    errs.append(f"errors.json: code '{need}' used by the validator is missing")
        h = canon.dict_hash(docs["commands"])
    return (2 if errs else 0), errs, h


def cmd_check(args: argparse.Namespace) -> int:
    code, errs, h = run_check()
    for e in errs:
        print("  " + e)
    print(f"dict_hash {h}")
    print("check: OK" if not code else f"check: {len(errs)} problem(s)")
    return code


def cmd_coverage(args: argparse.Namespace) -> int:
    dll = Path(args.dll_src) if args.dll_src else canon.DLL_SRC
    roots = [Path(r) for r in args.lua_root] if args.lua_root else coverage.default_lua_roots()
    doc = canon.load("commands")
    try:
        w = coverage.collect(dll, roots)
    except (OSError, ValueError) as e:
        print(f"error: {e}")
        return 2
    rep = coverage.classify(doc, w)
    rep.drift = coverage.drift(doc, dll)
    print(f"send* declarations: {w.send_decls} ({len(w.send)} names); Response*: {w.response_decls} ({len(w.response)} names); "
          + (f"Network.Send*: {len(w.lua)} names" if w.lua_scanned else "Network.Send*: not scanned (no Lua root)"))
    for kind, cl in rep.classes.items():
        print(f"  {kind}: " + ", ".join(f"{k}={v}" for k, v in sorted(cl.items())))
    for kind, n in rep.unknown:
        print(f"  UNKNOWN {kind}: {n}")
    for kind, n in rep.stale:
        print(f"  {'warning' if kind == 'lua' else 'STALE'} {kind}: dictionary names {n}, not found in the sources")
    if rep.drift:
        print(f"  warning: wire.src line numbers drifted for {len(rep.drift)} command(s); run 'generate'")
    if args.report:
        Path(args.report).write_text(coverage.markdown(doc, w, rep), encoding="utf-8")
    print("coverage: OK" if rep.code == 0 else "coverage: INCOMPLETE")
    return rep.code


def cmd_render(args: argparse.Namespace) -> int:
    doc = canon.load("commands")
    changed = render.write(doc, canon.dict_hash(doc), check_only=args.check)
    if args.check:
        print("command reference is out of date" if changed else "command reference is up to date")
        return 1 if changed else 0
    print("written" if changed else "unchanged", render.OUT)
    return 0


def cmd_generate(args: argparse.Namespace) -> int:
    dll = Path(args.dll_src) if args.dll_src else canon.DLL_SRC
    try:
        changed = gen.generate(dll)
    except (gen.GenError, OSError, ValueError) as e:
        print(f"error: {e}")
        return 2
    print("written:" if changed else "unchanged", ", ".join(changed))
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="civsim.agentapi", description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("check", help="validate the four documents").set_defaults(func=cmd_check)
    pc = sub.add_parser("coverage", help="classify every send*/Response*/Network.Send*")
    pc.add_argument("--dll-src", help="CvGameCoreDLL_Expansion2 directory (default: the repo's)")
    pc.add_argument("--lua-root", action="append", help="Lua directory to scan for Network.Send* (repeatable; default: game Assets, LEKMOD/Lua)")
    pc.add_argument("--report", help="write a markdown report")
    pc.set_defaults(func=cmd_coverage)
    pr = sub.add_parser("render", help="write the generated command reference (headless/generated-docs/AGENT-COMMANDS.md)")
    pr.add_argument("--check", action="store_true", help="do not write; exit 1 when out of date")
    pr.set_defaults(func=cmd_render)
    pg = sub.add_parser("generate", help="rewrite the json documents from the DLL headers and overlay.py")
    pg.add_argument("--dll-src")
    pg.set_defaults(func=cmd_generate)
    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
