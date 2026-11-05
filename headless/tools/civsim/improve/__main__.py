"""civsim.improve - dll-improvements registry tooling.

Usage:
  python -m civsim.improve journal render [--check]   # BEHAVIOR-CHANGES.md + OPTIMIZATIONS.md from the json
  python -m civsim.improve check-registry             # flag sets, records, sources and HeadlessImprovements.h agree

Exit codes: 0 ok; 1 generated documents out of date (--check); 2 registry/configuration error.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

from . import check as C
from . import journal as J
from . import registry as R


def cmd_journal(args: argparse.Namespace) -> int:
    reg = R.load(Path(args.registry) if args.registry else None)
    errs = R.validate(reg)
    if errs:
        print("registry is invalid:", *errs, sep="\n  ")
        return 2
    changed = J.write(reg, Path(args.docs) if args.docs else None, check_only=args.check)
    if args.check:
        if changed:
            print("out of date:", ", ".join(changed))
            return 1
        print("generated documents are up to date")
        return 0
    print("written:" if changed else "unchanged", ", ".join(changed))
    return 0


def cmd_check_registry(args: argparse.Namespace) -> int:
    try:
        reg = R.load(Path(args.registry) if args.registry else None)
    except R.RegistryError as e:
        print(f"error: {e}")
        return 2
    errs, notes = C.check(reg)
    for n in notes:
        print(n)
    if errs:
        print(f"{len(errs)} problem(s):")
        for e in errs:
            print("  " + e)
        return 2
    print("check-registry: OK")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="civsim.improve", description=__doc__.splitlines()[0])
    ap.add_argument("--registry", help="directory with the json files (default: headless/improvements)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    pj = sub.add_parser("journal", help="render the generated documents")
    pj.add_argument("action", choices=["render"])
    pj.add_argument("--check", action="store_true", help="do not write, exit 1 if the documents differ")
    pj.add_argument("--docs", help="output directory (default: headless/generated-docs)")
    pj.set_defaults(func=cmd_journal)

    pc = sub.add_parser("check-registry", help="consistency of registry, flag sets and sources")
    pc.set_defaults(func=cmd_check_registry)

    args = ap.parse_args(argv)
    try:
        return args.func(args)
    except R.RegistryError as e:
        print(f"error: {e}")
        return 2


if __name__ == "__main__":
    sys.exit(main())
