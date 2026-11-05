"""civsim.perflab — DLL speed measurement (V1 cut): measure / compare / gate.

python -m civsim.perflab measure <corpus|scn> --dll golden,optimized [--repeat 3] [--window 51-100] [--jobs N] [--pin] [--target 5.0]
python -m civsim.perflab compare <measure-dir-A> <measure-dir-B>
python -m civsim.perflab gate smoke|standard|nightly|<corpus> --dll <candidate> --against <reference> [--no-equiv] [--reuse <measure-dir>]
python -m civsim.perflab sample <scenario|corpus --pick N> [--dll profile] [--turns N] [--verify]   # external sampling profiler -> out/perf/<name>/hotspots.md/json
python -m civsim.perflab hotspots <perf-dir>                                                        # redo the offline analysis of samples.json.gz
A/A noise floor: measure <corpus> --dll golden,golden  (the CI half-width is the minimum detectable effect).
Exit: 0 ok, 1 runs failed, 2 config, 4 speed regression, 5 inconclusive (noise above MDE, work differs, equivalence not confirmed, target not met).
"""
