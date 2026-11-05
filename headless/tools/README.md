# headless\tools: Python orchestration (runs, determinism, performance)

Python 3.10, stdlib only. Run everything from the repo root with `$env:PYTHONPATH='headless\tools'`.
A pragmatic V1 cut (the cuts are listed at the end). Build and toolchain overview: [../README.md](../README.md).

| package | what |
|---|---|
| `civsim.simrunner` | scenarios + corpora, sim keys, cache, process pool, crash classification, batch reports |
| `civsim.detlab` | `digest`, `check`, `equiv`, `bisect`, `saveload`, `perturb`, `oos` (real-MP desync reports, see "Desync reports") (plus the existing `scan` / `catalog`) |
| `civsim.perflab` | `measure`, `compare`, `gate` (ABBA, per-turn medians, geometric mean, block-bootstrap CI); `sample`, `hotspots` (external sampling profiler, see below) |
| `civsim.common`, `civsim.digestbin` | paths, hashing, DLL/host/data resolution; `digest.bin` v1 reader/comparer |

## Inputs and conventions

- Host: `headless\out\host\Release\civhost.exe` (`--host <exe|fake .py>` or `$env:CIVHOST`). The host contract used is
  `civhost.exe --dll <dll> --data <profile dir> [--game <civ5>] --scenario <json> --turns N --out <run dir>`
  plus `--dump-state-at T`, `--checkpoint-every N`, `--load <ckpt>` and perturbation flags `--<axis-name> <value>` **only when the
  host lists them in its `--help` text** (probed once per process). Missing flags make the dependent command skip or report
  `unsupported`; nothing is guessed.
- DLL: `golden|diag|optimized|optimized-diag|profile` resolve to `headless\out\dll\<variant>\CvGameCore_Expansion2.dll`, or give a path
  (any directory name under `headless\out\dll` works).
  For equivalence and long `check` runs use the hygiene baseline `golden-hyg` (`build-dll.ps1 -Variant optimized -FlagSet hyg -OutName golden-hyg`),
  not raw golden. Raw golden saves stack residue: replay ExtraData, empty notification slots and pantheon holy-city coordinates.
  The first two differ between builds; the pantheon coordinates differ even between runs.
- Data profile: `--data <dir>` (default: newest `headless\out\oracle\cache-*`; needs `Civ5DebugDatabase.db`, `Localization-Merged.db`).
- `$env:CIVSIM_OUT` relocates `headless\out` (tests/sandboxes).
- Scenario = the host's JSON (`headless\scenarios\smoke\pangaea6.json`) plus `turns`, `env`, `note`. Seeds are mandatory, floats are rejected,
  names are checked against the profile DB. Base scenarios: `headless\scenarios\base\*.json`.
- Corpora: `headless\corpora\{starter,early,realistic,stress}.json`, cartesian expansion over a base scenario (`groups[].base/set/matrix`,
  synthetic axes `seed`, `civs.count`, `minors.count`). `simrunner expand <corpus>` lists scenarios and keys.
  starter = 3 x 20 turns; early = 6 seeds x 20/50 turns; realistic = 4 seeds x 4/6/8 civs x 100 turns; stress = Large (10 civs) and Huge (12 civs) x 4 seeds x 200 turns.
- `sim_key = sha256("simrunner-key-v1" + len-prefixed [canonical sim fields, dll sha256, data hash, host build sha256])`. `name`, `note`, `dll`,
  `outputs` are not part of it; `turns` and `env` are. Changing the host exe or the DLL therefore invalidates the cache by design.
- Cache: `headless\out\runs\cache\<k0k1>\<key>\` (scenario.json, result.json, digest.bin, run.json, host.log.gz, ..., `.complete`; built in
  `runs\tmp\` then renamed). The host's per-run copies of the data DBs and `map-write.bin` are deleted. Cached statuses: `ok` and `crash-repro`.
  Anything else lands in `runs\failed\<stamp>-<key8>-<name>\`. Batches: `headless\out\batches\<stamp>-<name>\` (batch.json, report.md/json, runner.log).
- Crash policy: nonzero exit or timeout is retried once; same last turn again = `crash-repro` (cached negative), different outcome = `crash-flaky`.
  Hosts run inside a kill-on-close Job Object (no orphans after the runner dies). Default timeout per run: 900 s + 30 s x turns (`--timeout`).

## Commands

```powershell
$env:PYTHONPATH = 'headless\tools'
python -m civsim.simrunner validate starter early realistic stress        # exit 2 on errors, < 1 s
python -m civsim.simrunner expand realistic
python -m civsim.simrunner run headless\scenarios\smoke\pangaea6.json --turns 10 [--dll golden|diag|<path>] [--no-cache]
python -m civsim.simrunner batch starter --jobs 3 [--repeat 2] [--turns N]   # exit 1 on crash/hang/repeat mismatch
python -m civsim.simrunner report headless\out\batches\<dir>
python -m civsim.simrunner selftest                                         # fake host, 1 s

python -m civsim.detlab check starter --repeat 2 --jobs 3                   # same DLL N times; exit 6 if digests differ
python -m civsim.detlab equiv realistic --dll golden --vs optimized         # equivalence.json; exit 6 on any divergence
python -m civsim.detlab bisect <scn> --dll golden --vs optimized            # turn -> segment -> byte offset + hex context
python -m civsim.detlab bisect <scn> --dll golden --vs golden               # determinism bisect (side B is a fresh process)
python -m civsim.detlab saveload <scn> --at 50 --until 60                   # needs host --checkpoint-every/--load, else "skipped"
python -m civsim.detlab perturb <scn> --axis active_player=0,1,3 --axis heap_pattern=0x00,0xFF --bisect
python -m civsim.detlab digest <A> <B> [--json]                             # A/B: digest.bin, run dir or sim key; exit 6 on divergence
python -m civsim.detlab.euiui diff --reference                              # EUI vs standard UI profile (ui_check.bat emulated): classify differing Lua, write headless\reference\profiles-eui-vs-ui.md
python -m civsim.detlab.euiui ai --turns 50                                 # AI-only digests, EUI vs standard Lua (private host copy headless\out\host-eui)
python -m civsim.detlab.euiui mixed --turns 60                              # 3 lockstep peers, human slot: control / EUI owner / all EUI
python -m civsim.detlab.euiui might                                         # 3 lockstep peers, one asks every player for GetMilitaryMight mid-game (DS-0034)
python -m civsim.enginehost.roundtrip <run>\checkpoints\turn-0050.ckpt [--dll <dll>] [--out <dir>]   # save/load round trip, no turn played:
                                                                            # civhost --load --ckpt-verify, per-segment byte ranges that change (exit 1)
python -m civsim.enginehost.roundtrip --compare <ckpt> <run dir>            # re-read an existing --ckpt-verify run

python -m civsim.perflab measure realistic --dll golden,optimized --repeat 3 --window 51-100 --jobs 4 --pin
python -m civsim.perflab measure starter --dll golden,golden --repeat 5 --turns 20      # A/A: CI half-width = noise floor / MDE
python -m civsim.perflab measure stress --dll golden,optimized --window 151-200 --target 5.0
python -m civsim.perflab gate standard --dll optimized --against golden     # needs a prior `detlab equiv golden-vs-optimized` (or --no-equiv); exit 4 = regression
python -m civsim.perflab compare headless\out\perf\<A> headless\out\perf\<B>

# sampling profiler (Windows; 64-bit Python samples the 32-bit civhost main thread from outside, civhost is not modified)
pwsh headless\tools\build-dll.ps1 -Variant profile                               # golden + /Oy- (EBP frames); the sampler works on any DLL, the profile one unwinds exactly
python -m civsim.perflab sample headless\scenarios\smoke\pangaea6.json --turns 100 --verify [--name N] [--hz 1000] [--dll profile]
python -m civsim.perflab sample stress --pick s31 --turns 150 --verify           # corpus targets need --pick <index|name substring>
python -m civsim.perflab sample --pid 1234 --duration 30                         # attach to a running civhost
python -m civsim.perflab hotspots headless\out\perf\<name>                       # redo the offline analysis (--update-re/--digest-re/--windows)
```

Reports: `headless\out\detlab\<stamp>-<kind>-<name>\` (check.json, equivalence.json, divergence.md/json, perturb.json/md, saveload.json),
`headless\out\perf\<name>\` (measure.json/md, per-run run.json, gate.json).

Exit codes: 0 ok/equal, 1 run failures, 2 config/validation, 4 speed regression (perflab gate), 5 inconclusive (noise above MDE, A/B did different work,
equivalence not confirmed, target not met), 6 divergence found.

Sampler details (`civsim.perflab.sampler` capture + dbghelp symbolizer, `hotspots` analysis/report): the main thread is sampled at ~700 Hz achieved
(1 kHz requested; Python + a high-resolution waitable timer) with `Wow64SuspendThread` + `Wow64GetThreadContext` + one `ReadProcessMemory` of the stack window
(bounded by the TEB stack base), ~55 us suspended per sample (~4% of wall, ~2-10% slowdown of the Update time). The host is started suspended inside the
kill-on-close job and runs from content-addressed copies of civhost.exe/DLL + PDBs in `headless\out\perf\_bin\` (rebuilds during a run cannot break symbols).
Frames: EBP chain in modules that keep frame pointers (profile DLL, civhost.exe; prologue/epilogue/thunk cases handled), otherwise a stack scan for
words that are return addresses (a `call` precedes them, a rel32 call must land in a module); junk frames from the scan are possible in CRT/lua/sqlite/OS
code, so inclusive numbers use only gamecore and host frames. Names: dbghelp offline (PDB for DLL + civhost, exports otherwise, 1 KB buckets when the nearest
export is far). `--verify` runs the same scenario unsampled (cached by sim key) and requires the same `digest.bin`; when it differs a second unsampled run
decides whether the scenario is inherently nondeterministic (exit 6 only if sampling itself changed the result). Output in `headless\out\perf\<name>\`:
`hotspots.md/json` (phases update/digest/other, Amdahl line, categories and who drives them, subsystems, top self/inclusive, hottest lines, per-window tables),
`samples.json.gz` (raw stacks and per-sample times), `run\` (the host's run dir). The Amdahl scope `update` is the host's per-turn `Update()` = what `measure` times.

Perf details: ratio `r = sum(median_K(ms_A)) / sum(median_K(ms_B))` over the window (A = first `--dll`; `r > 1` means B is faster), geometric mean over
scenarios, 95% CI from 2000 block-bootstrap iterations (10-turn blocks, scenarios resampled, `random.Random(20261017)`). Runs are ABBA ordered
(`A B B A ...`), never cached, one civhost per run; `--jobs N` runs N scenarios in parallel, `--pin` pins worker i to logical CPU 2i.
`gate` rule: regression = ratio (candidate/reference) > 1.03 and CI lower bound > 1.00; inconclusive when the CI half-width exceeds `--mde` (3%),
fewer than 4 bootstrap blocks exist, the digests of A and B differ, or no matching `equivalence.json` exists.
Suites: smoke = starter x2, standard = early x3, nightly = realistic x5.

## Tests

```powershell
python -m unittest discover -s headless\tools\tests -p "test_simrunner*.py"
python -m unittest discover -s headless\tools\tests -p "test_detlab_dyn*.py"
python -m unittest discover -s headless\tools\tests -p "test_perflab*.py"
python -m unittest discover -s headless\tools\tests -p "test_sampler*.py"     # PE parsing, return-address check, unwinder, dbghelp symbolizer (Windows)
```
All use `civsim.simrunner.fakehost` (a Python stand-in for civhost: same CLI and output files, behaviour steered by tags in the fake "DLL" text:
`MS=`, `DIVERGE=turn:segment`, `CRASH_AT=`, `NONSER`, `C2`, `C5`). They need no game files.

## Desync reports from real MP games

What the players' machines write, and how to read it.

**1. Per-turn checksum log (HYG-0010, flag `LEKMOD_HEADLESS_DIAG_TURN_LOG`, in the `release` and `opt` sets).** At the start of every
`CvGame::doTurn` (all commands and AI moves of turn T done, turn processing not started; every machine reaches it with the same state) the DLL
appends to `Logs\LekmodNG-turns.log` through the game's own log manager, only when logging is on (`GC.getLogging()` = `MessageLog`, or the log
manager enabled = `LoggingEnabled`; set both in the game's `config.ini`). Lines (<= 900 characters each, several per turn, optional exe timestamp prefix):

```
LTL1 t=57 ap=0 jon=<seed>,<calls>,<resets> map=<seed>,<calls>,<resets> g=<hex> m=<hex> v=<hex> us=<microseconds> kc=<kilocycles>
LTL1 t=57 T00=<techs known>,<hex> T01=...                  teams that ever lived: techs + research progress, war state
LTL1 t=57 P00=<h|a|d>,<cities>,<units>,<goldT100>,<econ>,<cities>,<units>,<policies> ...   players that ever lived
```

`g` game counters (turn, cities, population, alive flags), `m` plots (owner, terrain, feature, resource, improvement + pillage, route + pillage,
upgrade progress, units per plot), `v` plot visibility per team; per player `econ` = treasury, culture, faith, golden age, current research;
`cities` = id, position, population, damage, food, production, culture, threat value, head order; `units` = id, type, position, damage, moves, XP,
level, activity; `policies` = policies + branches. 32-bit murmur-style hashes of plain member reads: no RNG, no cached getters, no writes, no FPU.
`ap` (active player) and `us` / `kc` (cost) differ between machines by design and are ignored by the comparison. Every N turns: build with
`/D LEKMOD_HEADLESS_DIAG_TURN_LOG_EVERY=N`. Not a full state hash: AI-internal state (diplomacy opinions, danger / tactical maps) shows up only
through its effects (RNG call counts, units, cities). civhost writes the same file into `<run>\Logs\` with `--dll-logs`.

Evidence (2026-10-17, private host `headless\out\host-turnlog`, DLLs `tl-on` = release set incl. the flag, `tl-off` = the same 64 flags without it,
runs in `headless\out\turnlog\`): digest.bin identical for lekmod6 100 turns (tl-off, tl-on without logs, tl-on with `--dll-logs`) and pangaea6 100 turns
(tl-off vs tl-on with logs). Cost (TSC; civhost virtualises QPC, so `us` is 0 there): mean 1.8 M cycles (~0.55 ms at 3.4 GHz), max 3.3 M per turn on
lekmod6 / pangaea6; 1.4-3.1 M cycles (< 1 ms) per turn on a real 10+ civ save at turn 214 (`--import-save headless\out\saveio\many\x13`, 2.4 s turns).
About 1.4 KB of log per turn for 19 players.

**2. Analysis.** One report zip of a player-side log collector (or unpacked dir, Logs dir, civhost run dir, or a bare `LekmodNG-turns.log`) per machine:

```powershell
$env:PYTHONPATH = 'headless\tools'
python -m civsim.detlab oos LekmodReport-PC1-*.zip LekmodReport-PC2-*.zip LekmodReport-PC3-*.zip [--json oos.json]   # exit 0 equal, 6 divergence, 2 usage
python -m unittest discover -s headless\tools\tests -p "test_detlab_oos*.py"
```

It prints the first turn at which any machine's turn log differs, the differing keys with the sub-field (`P03: units_hash`, `T01: techs known 12/13`,
`m` plots, `jon` RNG with the call-count delta), the machines grouped by value; the first differing `RandCalls.csv` row of every machine vs the first
(Instance column ignored; this is usually the culprit's `Location` text); the OOS lines of each `net_message_debug.log` (RNG out of sync, City / Unit
/ Plot out of sync with variable names, resync); and the autosaves next to the divergent turn. The line of turn T is written before turn T's
processing, so the cause lies in turn T's commands / AI moves or in turn T-1's processing.

Next step by hand (not automated yet): `python -m civsim.saveio extract` two machines' autosaves of that turn (`AutoSave_Post_<T-1>` /
`AutoSave_Post_<T>`; in MP an `AutoSave_<T> <date>` is the host's resync source, `AutoSave_Resync` the received copy), import each with
`civhost --import-save <dir>` (with `--dump-state-at` / `--trace-writes` and a diag DLL of the same flag set for the
writing source line) and diff the segments (`civsim.enginehost.digest_diff`, the `localize` logic of `civsim.lockstep`). Two saves of the same turn
can also be compared directly: `body.bin` of both extracts, segment map from `envelope.json`. Follow-up: let `detlab oos` do this itself.

## Deliberately cut (V1)

No JSON-lines control channel or warm process pool (one `civhost` process per run, so isolation is always `fresh`), no `index.sqlite`, no `cache gc`/`pin`,
no `suite` budget enforcement, no `source.save` / save-io, no `metrics.bin` (per-turn ms come from `run.json`), no peak-memory/early-turn/overhead gates,
no `doctor`/machine snapshot, no RNG-call level in `bisect` (needs the diag RNG log; ends at segment + byte offset), no `civdiff.exe` (Python reads the
small `digest.bin`; exit-code contract kept), no `peers`, `traps`, `object`/`full` digest levels, `cross-env`/`chain` save/load modes, ETW profiling (replaced by the external `sample`/`hotspots` sampler above),
no `paired-parallel` mode. Checkpoint-based commands (`saveload`, late-turn perf corpora) activate when the host gains `--checkpoint-every` / `--load`.
