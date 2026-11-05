"""civsim.detlab — determinism lab CLI.

Usage:
  python -m civsim.detlab scan nonser|uninit|ptrorder|activeplayer|lua-local|all [--json OUT] [--min-score N] [--rule PREFIX]
  python -m civsim.detlab catalog list | show <DS-id> | check | render [--out headless/generated-docs/DESYNC-CATALOG.md]
  python -m civsim.detlab digest A B [--json]                          A/B: digest.bin, run dir or sim key; exit 6 on divergence
  python -m civsim.detlab check <scn|corpus> [--repeat 2] [--dll golden]   same DLL N times, digests must match (exit 6 if not)
  python -m civsim.detlab equiv <corpus> --dll golden --vs <dll>      writes equivalence.json (exit 6 on any divergence)
  python -m civsim.detlab bisect <scn> --dll A --vs B                 first divergent turn -> segment -> byte offset (divergence.md/json)
  python -m civsim.detlab saveload <scn> --at N --until M             continue-after-load vs continuous (needs host checkpoints)
  python -m civsim.detlab perturb <scn> --axis active_player=0,1 [--axis heap_pattern=0x00,0xFF]
  python -m civsim.detlab oos <report.zip|dir> <report.zip|dir> [...] [--json OUT]   real-MP desync reports: first divergent turn (exit 6)
common run options: --host --data --game --timeout --turns --jobs --out
exit: 0 equal, 1 run failure, 2 config, 6 divergence
"""
from __future__ import annotations

import sys


def main(argv: list[str]) -> int:
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return 0 if argv else 2
    cmd, rest = argv[0], argv[1:]
    if cmd == "scan":
        from . import scan
        return scan.main(rest)
    if cmd == "catalog":
        from . import catalog
        return catalog.main(rest)
    if cmd == "oos":
        from . import oos
        return oos.main(rest)
    if cmd in ("digest", "check", "equiv", "bisect", "saveload", "perturb"):
        from . import dyn
        return dyn.main(argv)
    print(f"unknown command {cmd!r}\n{__doc__}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
