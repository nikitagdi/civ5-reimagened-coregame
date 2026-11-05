#!/usr/bin/env bash
# Scenario runner over gamecore_abi.h.
#
#   run.sh --target wasm|linux --lib-dir DIR [--game GAMEDATA_DIR] --scenario boot|db|load|lua|vfs|gameinfo|setup|newgame|observer|dirtybench|humanturn|autochoice|autochoice2|autoplay|saveload|spike-net|mp|mphybrid|timer|dirty|purity|s2|peace|mpslots|mpsave|diplo|wbmapread|wbmap [--turns N] [--seed N] [--seeds N,..] [--ai N[,..]] [--report FILE]
#          [--humans N] [--mode simultaneous|hybrid] [--world-size WORLDSIZE_*] [--game-type network] [--repeat N]
#
# DIR is a build.sh output directory that has been through --mode smoke (it holds libgamecore.a and the Lua
# and SQLite objects in DIR/smoke). wasm runs under node's WASI with GAMEDATA_DIR as /game and the report
# directory as /out preopens; linux runs the native binary. The report (JSON) defaults to
# headless/out/runtime/<scenario>/report.json. Scenarios: boot; db (needs --game: db/gameplay.db + db/loc-en_us.db; opens the game DB from memory, expects 427 tables, round-trips the
# in-memory saved-game DB); load (M1, needs --game: rt_session boot D1-D2 B1-B4 I1-I8 on the real DB, key-table counts vs civhost).
# lua (rt_lua: exe-Lua checks, runs runtime/lua/test_main.lua; include() looks up that
# directory; no --game needed); vfs (rt_vfs: in-memory fixture with base-name conflicts; with --game also loads the real
# export's vfs-index.json, resolves includes and the map script, compiles every listed Lua file); gameinfo (A5, needs --game:
# GameInfo / DB.Query on the real DB, runtime/lua/gameinfo/gameinfo_probe.lua vs civhost_reference.txt, 50 queries).
# setup (rt_setup: parse of every ABI 4.2 key, unknown-key errors; with --game also the session, seeded AI choice and PreGame readback).
# newgame (A8, M2, needs --game: new game from {"seed":N}: Init2 with the map script, 37 contexts, DoGameStarted, 0 Lua errors; the
# report holds "map_digest", to compare between processes / targets).
# observer (A9, M3 / M6, needs --game): --turns N AI-only turns (all civilizations AI, an observer slot is active; --ai N = AI civilizations besides the first, default 1)
# through the turn loop and the net loopback: 0 Lua errors, no STUCK; the report holds per-turn ms (median, max), Init / Init2 times and the state digest
# ("state_digest", per-turn "digests") to compare between processes / targets.
# dirtybench (N1.3, needs --game): the observer game on WORLDSIZE_HUGE (use --ai 7 = 8 civilizations, --turns default 30); per turn, outside the turn timing: full Q_PLOTS
# per team (a), snapshot + comparison of the gcq_plot and basis fields (b), changed plots (c), AI turn time (d); "dirtybench-table|..." = p50 / p95 / max, "dirtybench-decision|..." = rule b > 3 ms -> task D5.
# humanturn (F1, M4a, needs --game): --turns N (default 5) human turns in the default setup (human in slot 0, --ai N AI civilizations):
# WAITING_HUMAN each turn, "end turn" while blocked rejected with the blocker, blockers resolved by the human's commands (or FORCE_END_TURN
# on odd turns), then the turn advances; "humanturn-block|..." lines are the observed blocker table. --game-type network (N3.4): the same in a network game
# (one human in slot 0, the server seat active), plus the GC_REASON_* checks, CANCEL_END_TURN and END_TURN_TIMEOUT.
# autochoice (F3, needs --game): --turns N (default 60) human turns (--ai default 1) with rt_autochoice part 1: research auto off from the setup JSON
# must stop the turn on the research blocker, switched on it clears it; the human never waits on research / policy / ideology blockers;
# then ERA_MODERN is set for the human's team and the ideology must be chosen automatically. "autochoice-event|..." lines are the
# AUTO_CHOICE events.
# autochoice2 (F4, needs --game): --turns N (default 40) is the turn budget; rt_autochoice part 2 (promotion, free / faith / Maya great person,
# pantheon, religion, enhancement, reformation, captured city; F5: World Congress proposals / votes, archaeology, ruin reward, tech steal, STUCK): per kind the flag-off pump must stop on the blocker, the flag-on pump must
# choose with an AUTO_CHOICE event ("autochoice2-event|..." lines).
# autochoice, autochoice2 --game-type network (N3.14, rt_selftest_acnet.cpp): the same for each of --humans N (default 2) test-bot humans of a network game with --ai A (default 1):
# flag off = every human waits on his own blocker, flag on = one AUTO_CHOICE event of his own; --turns T (default 60 / 40). Also the AI's requests to the humans (answered, none holds a turn).
# autoplay (F8, M4, needs --game): the test bot (rt_autoplay, setup "autoplay_human": 1) plays the human through the C ABI for --turns N
# (default 100) turns, once per seed x AI count: --seeds S1,S2,.. (default 1,2,3; --seed N alone = that seed) x --ai A1,A2,.. (default 1,2), one process
# per run. Each run: 0 STUCK / GAME_OVER, 0 log and Lua errors, diplomacy answered; the matrix also needs >= 1 AI diplomacy request over all runs.
# "autoplay-matrix|..." lines give per-run turns, errors, STUCK, diplomacy requests and wall time; per-run reports are report-s<S>-a<A>.json.
# saveload (G4a, M5, needs --game): per seed x AI count (--seeds default 1,2, --ai default 1,2) a save process and a load process: the test bot plays to
# game turn --save-at (default 30), gc_save at the first save point, --continue (default 20) more turns; the load process loads it in a fresh instance
# (broken saves first: BAD_DATA, slot kept), compares every viewer's gc_state_digest right after the load and the digests of -1 / the human's team turn
# by turn (a failing check fails the run; --strict-continuation 0 only reports it. Default 1 since FIX-0010 is in the portable build). --save-from DIR loads the saves another target wrote into DIR (cross-target). "saveload-matrix|..." lines give per-run results.
# spike-net (S1, N1.1, needs --game): a network game (Pitboss, server seat) with --humans N (default 2) test-bot humans and --ai A (default 2)
# AI for --turns T (default 30) in --mode simultaneous|hybrid (--world-size WORLDSIZE_* optional; the setup is the P2 "players" array) (hybrid: war between humans 0 and 1 at game turn 5); --game-type network only. One process
# per mode; its report is report-<mode>-<target>.json next to --report (default headless/out/runtime/spike-net/report.json), which lists every mode report there.
# timer (N3.6, needs --game): a network game with --humans N (default 2) humans played by the scenario and --ai A (default 1): Q_GAME v2 / Q_TURN, END_TURN_TIMEOUT,
# CANCEL_END_TURN, the safe point and events 2 / 12 / 14, --turns T (default 4); one run, one report.
# mp, mphybrid (N3.5, needs --game): a network game through the C ABI only, --humans N (default 2) test-bot humans (setup "autoplay_humans") and --ai A (default 2) AI, --turns T
# (default 100 / 30); mp = simultaneous turns, mphybrid = hybrid (war between humans 0 and 1 at game turn 5; use --humans 3 for the third, peaceful one). Per seed (--seeds S,.. or --seed S,
# default 1) the game runs --repeat R times (default 2), one process each, and the "mp-digest|turn|hex" lines of the runs must be equal turn by turn. Reports: report-<scenario>-s<S>-r<R>-<target>.json
# and the logs log-<scenario>-s<S>-r<R>-<target>.txt next to --report (default headless/out/runtime/<scenario>/report.json), which holds the matrix.
# dirty (N3.8, needs --game): a network game like mp (--humans N, default 2, test-bot humans; --ai A, default 2; --turns T, default 30; --world-size optional): after every gc_pump call
# Q_PLOTS_DIRTY against the full Q_PLOTS of every human's team and the full Q_PLOT_BASIS (0 misses), the first call, the reset by the call, the index forms, scenario-only plot edits; "dirty-cost|..." is
# the cost of the call (use --world-size WORLDSIZE_HUGE --humans 8 for the Huge x 8 figure). One run, one report.
# wbmapread (P14, N3.20, needs --game and --maps DIR): every *.Civ5Map under DIR (or only the --map REL files, REL relative to DIR) through
# CvWorldBuilderMap::Load against an independent parser of the same file. The gamecore reads them as game-data paths "maps/REL" (wasm: DIR is the /maps preopen).
# wbmap (P14, N3.20, needs --game, --maps DIR and --map REL): a network game on that map (--scenario-data 1: with its scenario) for --turns (default 5), twice
# in two processes; the map must match the file and both runs must end on the same state_digest (report-run1/2-<target>.json next to --report).
# Others report "not implemented" (exit 3).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../../.." && pwd)"
TARGET=linux; LIB_DIR=; GAME=; SCENARIO=boot; TURNS=; SEED=1; SEED_GIVEN=; SEEDS=; AI=; REPORT=; SAVE_AT=30; CONTINUE=20; SAVE_FROM=; STRICT=1
HUMANS=2; HUMANS_GIVEN=; AI_GIVEN=; LIB_DIR_OFF=; MAP_SCRIPTS=; SIZES=WORLDSIZE_DUEL,WORLDSIZE_HUGE; PARALLEL=; MODE=simultaneous; GAME_TYPE=; WORLD_SIZE=; REPEAT=2; MAPS=; MAP_LIST=(); SCENARIO_DATA=0; NOISES=0,1,2; NOISE_KINDS=
while [ $# -gt 0 ]; do
	case "$1" in
		--target) TARGET="$2"; shift 2 ;;
		--lib-dir) LIB_DIR="$2"; shift 2 ;;
		--game) GAME="$2"; shift 2 ;;
		--scenario) SCENARIO="$2"; shift 2 ;;
		--turns) TURNS="$2"; shift 2 ;;
		--seed) SEED="$2"; SEED_GIVEN=1; shift 2 ;;
		--seeds) SEEDS="$2"; shift 2 ;;
		--ai) AI="$2"; AI_GIVEN=1; shift 2 ;;
		--report) REPORT="$2"; shift 2 ;;
		--save-at) SAVE_AT="$2"; shift 2 ;;
		--continue) CONTINUE="$2"; shift 2 ;;
		--save-from) SAVE_FROM="$2"; shift 2 ;;
		--strict-continuation) STRICT="$2"; shift 2 ;;
		--humans) HUMANS="$2"; HUMANS_GIVEN=1; shift 2 ;;
		--lib-dir-off) LIB_DIR_OFF="$2"; shift 2 ;;
		--map-scripts) MAP_SCRIPTS="$2"; shift 2 ;;
		--sizes) SIZES="$2"; shift 2 ;;
		--parallel) PARALLEL="$2"; shift 2 ;;
		--mode) MODE="$2"; MODE_GIVEN=1; shift 2 ;;
		--world-size) WORLD_SIZE="$2"; shift 2 ;;
		--game-type) GAME_TYPE="$2"; shift 2 ;;
		--repeat) REPEAT="$2"; shift 2 ;;
		--maps) MAPS="$2"; shift 2 ;;
		--map) MAP_LIST+=("$2"); shift 2 ;;
		--scenario-data) SCENARIO_DATA="$2"; shift 2 ;;
		--noises) NOISES="$2"; shift 2 ;;
		--noise-kinds) NOISE_KINDS="$2"; shift 2 ;;
		*) echo "run.sh: unknown argument '$1'" >&2; exit 2 ;;
	esac
done
if [ -z "$TURNS" ]; then case "$SCENARIO" in observer|autoplay) TURNS=100 ;; dirtybench) TURNS=30 ;; purity) TURNS=50 ;; s2) TURNS=100 ;; humanturn) TURNS=5 ;; dirty) TURNS=30 ;; autochoice) TURNS=60 ;; autochoice2) TURNS=40 ;; spike-net) TURNS=30 ;; mp) TURNS=100 ;; mphybrid) TURNS=30 ;; timer) TURNS=4 ;; peace) TURNS=20 ;; mpslots) TURNS=35 ;; mpsave) TURNS=20 ;; mp8) TURNS=30 ;; mapscripts) TURNS=2 ;; mpmsg) TURNS=3 ;; replicaui) TURNS=6 ;; wbmap) TURNS=5 ;; *) TURNS=3 ;; esac; fi
# --game-type: spike-net is network only (default network); humanturn runs in both (default single, the B1 regress; N3.4); the other scenarios are single.
if [ -z "$GAME_TYPE" ]; then case "$SCENARIO" in spike-net|mp|mphybrid|timer|dirty|purity|s2|peace|mpslots|mpsave|mp8|mpmsg|replicaui|diplo) GAME_TYPE=network ;; *) GAME_TYPE=single ;; esac; fi
case "$GAME_TYPE" in single|network) ;; *) echo "run.sh: --game-type must be single or network" >&2; exit 2 ;; esac
if [ "$GAME_TYPE" = network ]; then
	case "$SCENARIO" in spike-net|humanturn|autochoice|autochoice2|mp|mphybrid|timer|dirty|purity|s2|peace|mpslots|mpsave|mp8|mpmsg|replicaui|diplo) ;; *)
		echo "run.sh: scenario $SCENARIO runs in the single game type only (--game-type network is for spike-net, humanturn, autochoice, autochoice2, mp, mphybrid, timer, dirty, purity, s2, peace, mpslots, mpsave, mp8 and mpmsg)" >&2; exit 2 ;; esac
fi
case "$SCENARIO" in mp|mphybrid|mpslots) MPFAMILY=1 ;; *) MPFAMILY= ;; esac  # the scenarios that run twice per seed and compare their digest lines (mpslots: N3.12)
if [ -n "$MPFAMILY" ]; then
	[ "$GAME_TYPE" = network ] || { echo "run.sh: scenario $SCENARIO is the network game type only (--game-type network)" >&2; exit 2; }
	[ -n "$AI" ] || AI=2
	case "$REPEAT" in ''|*[!0-9]*|0) echo "run.sh: --repeat must be a number >= 1" >&2; exit 2 ;; esac
fi
if [ "$SCENARIO" = dirty ] || [ "$SCENARIO" = purity ]; then
	[ "$GAME_TYPE" = network ] || { echo "run.sh: scenario $SCENARIO is the network game type only (--game-type network)" >&2; exit 2; }
	[ -n "$AI" ] || AI=2
fi
if [ "$SCENARIO" = mp8 ]; then
	[ "$GAME_TYPE" = network ] || { echo "run.sh: scenario mp8 is the network game type only" >&2; exit 2; }
	[ -n "$HUMANS_GIVEN" ] || HUMANS=8
	[ -n "$AI" ] || AI=2
	[ -n "$WORLD_SIZE" ] || WORLD_SIZE=WORLDSIZE_HUGE
fi
if [ "$SCENARIO" = replicaui ]; then
	[ -n "$AI" ] || AI=2
fi
if [ "$SCENARIO" = mpmsg ]; then
	[ -n "$HUMANS_GIVEN" ] || HUMANS=2
	[ -n "$AI" ] || AI=1
fi
if [ "$SCENARIO" = s2 ]; then
	# s2 (N3.16): network (default) with --humans N, or --game-type single: the B1 reference with one human.
	[ -n "$AI" ] || AI=2
fi
if [ "$SCENARIO" = timer ]; then
	[ "$GAME_TYPE" = network ] || { echo "run.sh: scenario timer is the network game type only (--game-type network)" >&2; exit 2; }
fi
if [ "$SCENARIO" = spike-net ]; then
	case "$MODE" in simultaneous|hybrid) ;; *) echo "run.sh: --mode must be simultaneous or hybrid" >&2; exit 2 ;; esac
	[ "$GAME_TYPE" = network ] || { echo "run.sh: scenario spike-net is the network game type only (--game-type network)" >&2; exit 2; }
	[ -n "$AI" ] || AI=2
fi
if [ "$SCENARIO" = autoplay ] || [ "$SCENARIO" = saveload ]; then
	# The M4 matrix by default (saveload: seeds 1,2); --seed N alone (no --seeds) is a one-seed list.
	DEFAULT_SEEDS=1,2,3; [ "$SCENARIO" = saveload ] && DEFAULT_SEEDS=1,2
	[ -n "$SEEDS" ] || { [ -n "$SEED_GIVEN" ] && SEEDS="$SEED"; } || SEEDS=$DEFAULT_SEEDS
	[ -n "$AI" ] || AI=1,2
elif [ -n "$MPFAMILY" ]; then
	[ -n "$SEEDS" ] || SEEDS="$SEED"
else
	[ -z "$SEEDS" ] || { echo "run.sh: --seeds is for scenarios autoplay, saveload, mp and mphybrid (use --seed)" >&2; exit 2; }
	[ -n "$AI" ] || AI=1
	case "$AI" in *,*) echo "run.sh: --ai takes a list only for scenarios autoplay and saveload" >&2; exit 2 ;; esac
fi
case "$TARGET" in linux|wasm) ;; *) echo "run.sh: --target must be linux or wasm" >&2; exit 2 ;; esac
[ -n "$LIB_DIR" ] && [ -f "$LIB_DIR/libgamecore.a" ] && [ -f "$LIB_DIR/smoke/sqlite3.o" ] ||
	{ echo "run.sh: --lib-dir must be a build.sh --mode smoke output (libgamecore.a + smoke/*.o)" >&2; exit 2; }
LIB_DIR="$(cd "$LIB_DIR" && pwd)"
[ -n "$REPORT" ] || REPORT="$ROOT/headless/out/runtime/$SCENARIO/report.json"
mkdir -p "$(dirname "$REPORT")"
REPORT="$(cd "$(dirname "$REPORT")" && pwd)/$(basename "$REPORT")"
[ -z "$GAME" ] || GAME="$(cd "$GAME" && pwd)"
[ -z "$MAPS" ] || MAPS="$(cd "$MAPS" && pwd)"

LUA_DIR="$HERE/lua"
BIN="$LIB_DIR/runtime"
mkdir -p "$BIN"
shopt -s nullglob
LUA_OBJS=("$LIB_DIR"/smoke/lua/*.o)
if [ "$TARGET" = wasm ]; then
	[ -n "${WASI_SDK:-}" ] && [ -x "$WASI_SDK/bin/clang++" ] || { echo "run.sh: WASI_SDK is not set or invalid" >&2; exit 2; }
	SYSROOT="$WASI_SDK/share/wasi-sysroot"
	CC="$WASI_SDK/bin/clang"; CXX="$WASI_SDK/bin/clang++"
	TFLAGS=(--target=wasm32-wasip1 "--sysroot=$SYSROOT" -fwasm-exceptions -mllvm -wasm-use-legacy-eh=false
		-D_WASI_EMULATED_PROCESS_CLOCKS -D_WASI_EMULATED_GETPID -D_WASI_EMULATED_MMAN -D_WASI_EMULATED_SIGNAL)
	CXXSTD=(-stdlib=libc++); EXE=harness.wasm
else
	CC="${CC:-$(command -v clang-18 || command -v clang)}"; CXX="${CXX:-$(command -v clang++-18 || command -v clang++)}"
	TFLAGS=(-fPIC); CXXSTD=()
	grep -q '^stdlib: libc++' "$LIB_DIR/build-info.txt" 2>/dev/null && CXXSTD=(-stdlib=libc++)
	EXE=harness
fi

"$CC" "${TFLAGS[@]}" -std=c99 -O1 -Wall -Wextra -I"$ROOT/headless/portable/abi" -I"$ROOT/headless/portable/runtime" -c "$HERE/harness.c" -o "$BIN/harness.o"
if [ "$TARGET" = wasm ]; then
	"$CXX" "${TFLAGS[@]}" "${CXXSTD[@]}" -o "$BIN/$EXE" "$BIN/harness.o" -Wl,--whole-archive "$LIB_DIR/libgamecore.a" -Wl,--no-whole-archive \
		"${LUA_OBJS[@]}" "$LIB_DIR/smoke/sqlite3.o" -L"$SYSROOT/lib/wasm32-wasip1/eh" -lc++ -lc++abi -lunwind \
		-lwasi-emulated-process-clocks -lwasi-emulated-getpid -lwasi-emulated-mman -lwasi-emulated-signal -Wl,-z,stack-size=8388608
else
	"$CXX" "${CXXSTD[@]}" -o "$BIN/$EXE" "$BIN/harness.o" -Wl,--whole-archive "$LIB_DIR/libgamecore.a" -Wl,--no-whole-archive \
		"${LUA_OBJS[@]}" "$LIB_DIR/smoke/sqlite3.o" -Wl,--no-undefined -lm -lpthread
fi

echo "run.sh: scenario=$SCENARIO target=$TARGET report=$REPORT"
cd "$BIN"
# One harness process: run_one SEED AI REPORT_FILE (the report's directory must exist).
run_one() {
	local seed="$1" ai="$2" rep="$3"; shift 3
	# Extra harness arguments; "@OUT@" in them is the report directory as the target sees it (/out on wasm).
	local extra=() e
	for e in "$@"; do if [ "$TARGET" = wasm ]; then extra+=("${e//@OUT@//out}"); else extra+=("${e//@OUT@/$(dirname "$rep")}"); fi; done
	if [ "$TARGET" = wasm ]; then
		local args=(--scenario "$SCENARIO" --seed "$seed" --turns "$TURNS" --ai "$ai" --report "/out/$(basename "$rep")" "${extra[@]}")
		local pre=(--preopen "/out=$(dirname "$rep")")
		if [ -n "$GAME" ]; then args+=(--game /game); pre+=(--preopen "/game=$GAME"); fi
		args+=(--lua-dir /lua); pre+=(--preopen "/lua=$LUA_DIR")
		if [ -n "$MAPS" ]; then args+=(--maps /maps); pre+=(--preopen "/maps=$MAPS"); fi
		# node 22.22 (WSL) segfaults (SIGSEGV in node itself, not a wasm trap) once the module's memory has grown by tens of MB:
		# --no-concurrent-marking fixed scenario db; scenario load (~100 MB after CacheGameDatabaseData) also needs
		# --disable-wasm-trap-handler (explicit bounds checks instead of guard-page signals; out-of-bounds accesses still trap).
		# Runs of load: both 10/10, --no-concurrent-marking alone 5/8, --disable-wasm-trap-handler alone 0/10. node 26 on Windows needs neither, so they are kept to node < 24.
		local NODE_FLAGS=(--experimental-wasm-exnref)
		# NODE overrides the node binary (B1: ~/node-26/bin/node in WSL; node 22 crashes on the bigger scenarios even with the flags).
		local NODE="${NODE:-node}"
		local NODE_MAJOR
		NODE_MAJOR="$("$NODE" -p 'process.versions.node.split(".")[0]')"
		[ "$NODE_MAJOR" -ge 24 ] || NODE_FLAGS+=(--no-concurrent-marking --disable-wasm-trap-handler)
		"$NODE" "${NODE_FLAGS[@]}" "$HERE/run_wasi.mjs" "$BIN/$EXE" "${pre[@]}" -- "${args[@]}"
		return $?
	fi
	local args=(--scenario "$SCENARIO" --seed "$seed" --turns "$TURNS" --ai "$ai" --report "$rep" "${extra[@]}")
	[ -z "$GAME" ] || args+=(--game "$GAME")
	args+=(--lua-dir "$LUA_DIR")
	[ -z "$MAPS" ] || args+=(--maps "$MAPS")
	"$BIN/$EXE" "${args[@]}"
}

if [ "$SCENARIO" = saveload ]; then
	# saveload (G4a, M5): per seed x AI count two processes: phase save (new game, the test bot to game turn --save-at, gc_save, --continue more
	# turns, writes save-s<S>-a<A>.gcs1 and its .digests next to the report), then phase load (fresh instance, broken saves refused, gc_load_game,
	# digests equal right after the load and turn by turn). --save-from DIR skips the save phase and loads DIR's save (cross-target check).
	num() { { grep -o "\"$1\":-\?[0-9]*" "$2" 2>/dev/null || true; } | head -n 1 | cut -d: -f2; }
	IFS=, read -r -a SEED_LIST <<< "$SEEDS"
	IFS=, read -r -a AI_LIST <<< "$AI"
	DIR="$(dirname "$REPORT")"
	rc=0; runs=0; failed=0; runs_json=
	for s in "${SEED_LIST[@]}"; do
		for a in "${AI_LIST[@]}"; do
			save="save-s$s-a$a.gcs1"
			rs="$DIR/report-save-s$s-a$a.json"; rl="$DIR/report-load-s$s-a$a.json"
			rm -f "$rl"
			r1=0
			if [ -n "$SAVE_FROM" ]; then
				cp "$SAVE_FROM/$save" "$SAVE_FROM/$save.digests" "$DIR/" || r1=2
				rm -f "$rs"
			else
				rm -f "$rs" "$DIR/$save" "$DIR/$save.digests"
				run_one "$s" "$a" "$rs" --phase save --save-file "@OUT@/$save" --save-at "$SAVE_AT" --continue "$CONTINUE" || r1=$?
			fi
			r2=0
			run_one "$s" "$a" "$rl" --phase load --save-file "@OUT@/$save" --save-at "$SAVE_AT" --continue "$CONTINUE" --strict "$STRICT" || r2=$?
			echo "saveload-matrix|target $TARGET|seed $s|ai $a|save rc $r1 (turn $(num save_turn "$rs"), $(num save_bytes "$rs") bytes, $(num save "$rs") ms)|load rc $r2 ($(num load "$rl") ms, views differ $(num views_differ "$rl"), turns compared $(num turns_compared "$rl"), first diverged $(num first_diverged "$rl"))"
			runs=$((runs + 1))
			if [ "$r1" != 0 ] || [ "$r2" != 0 ]; then failed=$((failed + 1)); rc=1; fi
			runs_json+="${runs_json:+,}{\"seed\":$s,\"ai\":$a,\"save_rc\":$r1,\"load_rc\":$r2}"
		done
	done
	ok=false; [ "$rc" = 0 ] && ok=true
	echo "{\"scenario\":\"saveload\",\"task\":\"G4a\",\"milestone\":\"M5\",\"target\":\"$TARGET\",\"save_at\":$SAVE_AT,\"continue\":$CONTINUE,\"save_from\":\"$SAVE_FROM\",\"ok\":$ok,\"runs\":$runs,\"failed\":$failed,\"run_list\":[$runs_json]}" > "$REPORT"
	echo "saveload: $([ "$rc" = 0 ] && echo OK || echo FAILED) ($runs run(s), $failed failed)"
	exit $rc
fi

if [ "$SCENARIO" = spike-net ]; then
	# spike-net (S1, N1.1): one process per --mode; its report is report-<mode>-<target>.json next to REPORT, and REPORT lists every
	# mode report present there (run the scenario once per mode to fill it).
	DIR="$(dirname "$REPORT")"
	rep="$DIR/report-$MODE-$TARGET.json"
	rm -f "$rep"
	r=0
	run_one "$SEED" "$AI" "$rep" --humans "$HUMANS" --mode "$MODE" ${WORLD_SIZE:+--world-size "$WORLD_SIZE"} || r=$?
	{
		printf '{"scenario":"spike-net","task":"N1.1","reports":{'
		first=1
		for f in "$DIR"/report-*-*.json; do
			[ -f "$f" ] || continue
			b="$(basename "$f" .json)"; b="${b#report-}"
			[ "$first" = 1 ] || printf ','
			first=0
			printf '"%s":' "$b"; tr -d '\n' < "$f"
		done
		printf '}}\n'
	} > "$REPORT"
	exit $r
fi

if [ -n "$MPFAMILY" ]; then
	# mp / mphybrid / mpslots (N3.5, N3.12): per seed --repeat processes of the same game; their "<scenario>-digest|turn|hex" lines (the state digest after every
	# turn change) must be equal turn by turn. The logs of all runs are printed, so a caller sees the scenario's own result lines.
	DIR="$(dirname "$REPORT")"
	IFS=, read -r -a SEED_LIST <<< "$SEEDS"
	rc=0; runs=0; failed=0; mism=0; runs_json=
	for s in "${SEED_LIST[@]}"; do
		first_digests=
		for ((k = 1; k <= REPEAT; k++)); do
			rep="$DIR/report-$SCENARIO-s$s-r$k-$TARGET.json"; log="$DIR/log-$SCENARIO-s$s-r$k-$TARGET.txt"
			rm -f "$rep" "$log"
			t0=$(date +%s%N)
			r=0
			run_one "$s" "$AI" "$rep" --humans "$HUMANS" --mode "$MODE" ${WORLD_SIZE:+--world-size "$WORLD_SIZE"} > "$log" 2>&1 || r=$?
			ms=$(( ($(date +%s%N) - t0) / 1000000 ))
			cat "$log"
			digests="$(grep -a "^$SCENARIO-digest|" "$log" || true)"
			if [ "$k" = 1 ]; then first_digests="$digests"; same=-
			elif [ "$digests" = "$first_digests" ]; then same=equal
			else same=DIFFERENT; mism=$((mism + 1)); fi
			echo "$SCENARIO-matrix|target $TARGET|seed $s|run $k|rc $r|digest lines $(printf '%s\n' "$digests" | grep -ac .)|vs run 1: $same|wall ${ms} ms"
			runs=$((runs + 1))
			[ "$r" = 0 ] || { failed=$((failed + 1)); rc=1; }
			runs_json+="${runs_json:+,}{\"seed\":$s,\"run\":$k,\"rc\":$r,\"vs_run_1\":\"$same\",\"wall_ms\":$ms}"
		done
	done
	[ "$mism" = 0 ] || rc=1
	ok=false; [ "$rc" = 0 ] && ok=true
	echo "{\"scenario\":\"$SCENARIO\",\"task\":\"N3.5\",\"target\":\"$TARGET\",\"humans\":$HUMANS,\"ai\":$AI,\"turns\":$TURNS,\"repeat\":$REPEAT,\"ok\":$ok,\"runs\":$runs,\"failed\":$failed,\"digest_mismatches\":$mism,\"run_list\":[$runs_json]}" > "$REPORT"
	echo "$SCENARIO: $([ "$rc" = 0 ] && echo OK || echo FAILED) ($runs run(s), $failed failed, $mism seed(s) with different digests between runs)"
	exit $rc
fi

if [ "$SCENARIO" = s2 ]; then
	run_one "$SEED" "$AI" "$REPORT" --humans "$HUMANS" --game-type "$GAME_TYPE" ${WORLD_SIZE:+--world-size "$WORLD_SIZE"}
	exit $?
fi

if [ "$SCENARIO" = mp8 ]; then
	# mp8 (S4, N3.17): one game; --humans N (default 8), --ai A (default 2), --world-size (default WORLDSIZE_HUGE), --turns T (default 30; 150 for the measurements).
	# Other cases of AC-N5.4: --humans 1 --ai 1 --world-size WORLDSIZE_DUEL; --humans 2 --ai 6 --world-size WORLDSIZE_STANDARD.
	run_one "$SEED" "$AI" "$REPORT" --humans "$HUMANS" --world-size "$WORLD_SIZE"
	exit $?
fi

if [ "$SCENARIO" = mpmsg ]; then
	# mpmsg (N3.21): one game of this build (--humans >= 2, --ai >= 1). With --lib-dir-off DIR (a build.sh output made with
	# LEKMOD_WITHOUT_DEFINES=LEKMOD_HEADLESS_HYG_MESSAGES_TO_HUMANS) the same scenario runs on that build too and the "mpmsg-digest|" lines of the two runs must be equal (the flag is UI only).
	DIR="$(dirname "$REPORT")"
	rc=0
	mkdir -p "$DIR"
	run_one "$SEED" "$AI" "$REPORT" --humans "$HUMANS" > "$DIR/log-mpmsg-on-$TARGET.txt" 2>&1 || rc=$?
	cat "$DIR/log-mpmsg-on-$TARGET.txt"
	if [ -n "$LIB_DIR_OFF" ]; then
		roff=0
		bash "$HERE/run.sh" --target "$TARGET" --lib-dir "$LIB_DIR_OFF" --scenario mpmsg --game "$GAME" --seed "$SEED" --turns "$TURNS" --humans "$HUMANS" --ai "$AI" --report "$DIR/off/report.json" > "$DIR/log-mpmsg-off-$TARGET.txt" 2>&1 || roff=$?
		grep -a -E '^mpmsg: |^mpmsg-summary' "$DIR/log-mpmsg-off-$TARGET.txt" | sed 's/^/off build: /'
		on_d="$(grep -a '^mpmsg-digest|' "$DIR/log-mpmsg-on-$TARGET.txt" || true)"; off_d="$(grep -a '^mpmsg-digest|' "$DIR/log-mpmsg-off-$TARGET.txt" || true)"
		n=$(printf '%s\n' "$on_d" | grep -ac .)
		if [ "$on_d" = "$off_d" ] && [ "$n" -ge 4 ]; then same=equal; else same=DIFFERENT; rc=1; fi
		[ "$roff" = 0 ] || rc=1
		echo "mpmsg-digest-compare|target $TARGET|flag on vs off|digest lines $n|$same|off run rc $roff"
		[ "$same" = equal ] || diff <(printf '%s\n' "$on_d") <(printf '%s\n' "$off_d") | head -n 6
	fi
	exit $rc
fi

if [ "$SCENARIO" = mapscripts ]; then
	# mapscripts (N3.18, needs --game): every map script of the game data's vfs-index.json ("map_scripts", or --map-scripts NAME,..) x the sizes (--sizes, default
	# WORLDSIZE_DUEL,WORLDSIZE_HUGE): a new game and --turns (default 2) turns, one process per game, --parallel N at once. Writes map-scripts.json next to vfs-index.json (only for the full
	# list; a --map-scripts subset writes it next to --report): the scripts that passed every size ("passed") and the result of each game.
	[ -n "$GAME" ] && [ -f "$GAME/vfs-index.json" ] || { echo "run.sh: scenario mapscripts needs --game DIR with a vfs-index.json" >&2; exit 2; }
	command -v jq >/dev/null || { echo "run.sh: scenario mapscripts needs jq" >&2; exit 2; }
	DIR="$(dirname "$REPORT")/mapscripts-$TARGET"
	rm -rf "$DIR"; mkdir -p "$DIR"
	if [ -n "$MAP_SCRIPTS" ]; then IFS=, read -r -a NAMES <<< "$MAP_SCRIPTS"; else mapfile -t NAMES < <(jq -r '.map_scripts | keys[]' "$GAME/vfs-index.json"); fi
	IFS=, read -r -a SIZES <<< "$SIZES"
	[ -n "$PARALLEL" ] || { [ "$TARGET" = wasm ] && PARALLEL=2 || PARALLEL=6; }
	echo "run.sh: mapscripts: ${#NAMES[@]} script(s) x ${#SIZES[@]} size(s), $TURNS turn(s), $PARALLEL at a time"
	for name in "${NAMES[@]}"; do
		path="$(jq -r --arg n "$name" '.map_scripts[$n] // empty' "$GAME/vfs-index.json")"
		[ -n "$path" ] || { echo "run.sh: map script '$name' is not in vfs-index.json" >&2; exit 2; }
		for size in "${SIZES[@]}"; do
			while [ "$(jobs -rp | wc -l)" -ge "$PARALLEL" ]; do wait -n || true; done
			ai="$AI"; [ -n "$AI_GIVEN" ] || { [ "$size" = WORLDSIZE_DUEL ] && ai=1 || ai=3; }
			(
				r=0
				run_one "$SEED" "$ai" "$DIR/r-$name-$size.json" --map-script "$path" --world-size "$size" > "$DIR/log-$name-$size.txt" 2>&1 || r=$?
				echo "$r" > "$DIR/rc-$name-$size"
			) &
		done
	done
	wait
	: > "$DIR/results.jsonl"
	rc=0; total=0; bad=0
	for name in "${NAMES[@]}"; do
		path="$(jq -r --arg n "$name" '.map_scripts[$n]' "$GAME/vfs-index.json")"
		for size in "${SIZES[@]}"; do
			total=$((total + 1))
			r="$(cat "$DIR/rc-$name-$size" 2>/dev/null || echo 99)"
			sum="$(grep -a '^mapscripts-summary|' "$DIR/log-$name-$size.txt" | tail -n 1 || true)"
			ok=false; [ "$r" = 0 ] && printf '%s' "$sum" | grep -q '^mapscripts-summary|PASS|' && ok=true
			if [ "$ok" = true ]; then reason=; else reason="$(printf '%s' "$sum" | awk -F'|' '{print $NF}')"; [ -n "$reason" ] || reason="no result (rc $r): $(grep -a -v '^mapscripts' "$DIR/log-$name-$size.txt" | tail -n 1 | cut -c1-200)"; bad=$((bad + 1)); fi
			ms="$(grep -o '"total_ms":[0-9.]*' "$DIR/r-$name-$size.json" 2>/dev/null | head -n 1 | cut -d: -f2 || true)"
			echo "mapscripts-result|$name|$size|$([ "$ok" = true ] && echo PASS || echo FAIL)|${ms:-?} ms|$reason"
			jq -nc --arg n "$name" --arg p "$path" --arg s "$size" --argjson ok "$ok" --arg r "$reason" --argjson ms "${ms:-0}" '{name:$n,script:$p,size:$s,ok:$ok,ms:$ms,reason:$r}' >> "$DIR/results.jsonl"
		done
	done
	OUTJSON="$DIR/map-scripts.json"
	jq -s --argjson turns "$TURNS" --arg target "$TARGET" --argjson sizes "$(printf '%s\n' "${SIZES[@]}" | jq -R . | jq -sc .)" \
		'{format:1, task:"N3.18", target:$target, turns:$turns, sizes:$sizes,
		  passed:(group_by(.name) | map(select(all(.[]; .ok)) | .[0].name)),
		  failed:(group_by(.name) | map(select(any(.[]; .ok | not)) | {name:.[0].name, sizes:(map(select(.ok | not) | {size, reason}))})),
		  scripts:(group_by(.name) | map({name:.[0].name, script:.[0].script, ok:(all(.[]; .ok)), results:(map({key:.size, value:{ok, ms, reason}}) | from_entries)}))}' "$DIR/results.jsonl" > "$OUTJSON"
	if [ -z "$MAP_SCRIPTS" ]; then cp "$OUTJSON" "$GAME/map-scripts.json"; echo "run.sh: wrote $GAME/map-scripts.json"; fi
	cp "$OUTJSON" "$REPORT"
	echo "mapscripts-summary-all|target $TARGET|$total game(s)|$bad failed|passed scripts: $(jq -r '.passed | length' "$OUTJSON") of ${#NAMES[@]}"
	# Scripts that do not pass are a result (they stay out of the lobby list), not a failure of the scenario; it fails only when nothing passed.
	[ "$(jq -r '.passed | length' "$OUTJSON")" -ge 1 ] || rc=1
	echo "mapscripts: $([ "$rc" = 0 ] && echo OK || echo FAILED) ($bad of $total game(s) did not pass; the passing scripts are in map-scripts.json)"
	exit $rc
fi

if [ "$SCENARIO" = purity ]; then
	# purity (AC-N1.4, N3.15): three processes of the same game (--humans N, default 2, --ai A, default 2, --turns T, default 50, --seed S): the reference (--noise 0),
	# 10 000 random queries Q_* 1..21 between the steps (--noise 1), 5 000 random commands that must be refused (--noise 2). Their "purity-digest|turn|hex" lines (the simulation digest after every
	# turn change) must be equal turn by turn; a difference names the first turn.
	DIR="$(dirname "$REPORT")"
	rc=0; ref=; ref_n=0
	for noise in ${NOISES//,/ }; do
		rep="$DIR/report-purity-n$noise-$TARGET.json"; log="$DIR/log-purity-n$noise-$TARGET.txt"
		rm -f "$rep" "$log"
		t0=$(date +%s%N); r=0
		run_one "$SEED" "$AI" "$rep" --humans "$HUMANS" --noise "$noise" ${NOISE_KINDS:+--noise-kinds "$NOISE_KINDS"} ${WORLD_SIZE:+--world-size "$WORLD_SIZE"} > "$log" 2>&1 || r=$?
		ms=$(( ($(date +%s%N) - t0) / 1000000 ))
		cat "$log"
		digests="$(grep -a '^purity-digest|' "$log" || true)"
		n=$(printf '%s\n' "$digests" | grep -ac .)
		if [ "$noise" = 0 ] && [ -z "$ref" ]; then ref="$digests"; ref_n=$n; same=-
		elif [ "$digests" = "$ref" ] && [ "$n" -ge 1 ]; then same=equal
		else
			same=DIFFERENT; rc=1
			first="$(diff <(printf '%s\n' "$ref") <(printf '%s\n' "$digests") | grep -a -m1 '^[<>]' || true)"
			echo "purity-first-difference|noise $noise|$first"
		fi
		echo "purity-matrix|target $TARGET|seed $SEED|noise $noise|rc $r|digest lines $n|vs reference: $same|wall ${ms} ms"
		[ "$r" = 0 ] || rc=1
	done
	[ "$ref_n" -ge 1 ] || rc=1
	ok=false; [ "$rc" = 0 ] && ok=true
	echo "{\"scenario\":\"purity\",\"task\":\"N3.15\",\"target\":\"$TARGET\",\"humans\":$HUMANS,\"ai\":$AI,\"turns\":$TURNS,\"ok\":$ok}" > "$REPORT"
	echo "purity: $([ "$rc" = 0 ] && echo OK || echo FAILED) (digests of the three runs $([ "$rc" = 0 ] && echo equal || echo 'differ or a run failed'))"
	exit $rc
fi

if [ "$SCENARIO" = timer ]; then
	# timer (N3.6): one game, --humans N (default 2) humans played by the scenario, --ai A (default 1).
	run_one "$SEED" "$AI" "$REPORT" --humans "$HUMANS" ${WORLD_SIZE:+--world-size "$WORLD_SIZE"}
	exit $?
fi

if [ "$SCENARIO" = mpsave ]; then
	# mpsave (N3.13): a save process (3+ test-bot humans, a kick and a claim, gc_save at --save-at, --continue turns) and a load process (gc_load_game, CONNECT of every human, the same turns,
	# digests compared); --save-from DIR loads the save another target wrote into DIR.
	num() { { grep -o "\"$1\":-\?[0-9]*" "$2" 2>/dev/null || true; } | head -n 1 | cut -d: -f2; }
	DIR="$(dirname "$REPORT")"
	save="save-s$SEED-h$HUMANS-a$AI.gcs1"
	rs="$DIR/report-save-$TARGET.json"; rl="$DIR/report-load-$TARGET.json"
	rm -f "$rl"
	r1=0
	if [ -n "$SAVE_FROM" ]; then
		cp "$SAVE_FROM/$save" "$SAVE_FROM/$save.digests" "$DIR/" || r1=2
		rm -f "$rs"
	else
		rm -f "$rs" "$DIR/$save" "$DIR/$save.digests"
		run_one "$SEED" "$AI" "$rs" --humans "$HUMANS" --phase save --save-file "@OUT@/$save" --save-at "$SAVE_AT" --continue "$CONTINUE" ${WORLD_SIZE:+--world-size "$WORLD_SIZE"} || r1=$?
	fi
	r2=0
	run_one "$SEED" "$AI" "$rl" --humans "$HUMANS" --phase load --save-file "@OUT@/$save" --save-at "$SAVE_AT" --continue "$CONTINUE" --strict "$STRICT" ${WORLD_SIZE:+--world-size "$WORLD_SIZE"} || r2=$?
	echo "mpsave-matrix|target $TARGET|seed $SEED|humans $HUMANS|ai $AI|save rc $r1 (turn $(num save_turn "$rs"), $(num save_bytes "$rs") bytes, $(num save "$rs") ms)|load rc $r2 ($(num load "$rl") ms, turns compared $(num turns_compared "$rl"), first diverged $(num first_diverged "$rl"))"
	rc=0; [ "$r1" = 0 ] && [ "$r2" = 0 ] || rc=1
	ok=false; [ "$rc" = 0 ] && ok=true
	echo "{\"scenario\":\"mpsave\",\"task\":\"N3.13\",\"target\":\"$TARGET\",\"save_at\":$SAVE_AT,\"continue\":$CONTINUE,\"ok\":$ok,\"save_rc\":$r1,\"load_rc\":$r2}" > "$REPORT"
	echo "mpsave: $([ "$rc" = 0 ] && echo OK || echo FAILED) (save rc $r1, load rc $r2)"
	exit $rc
fi

if [ "$SCENARIO" = diplo ]; then
	# diplo (B4 fix-deal-crash): one game, --humans N (default 1), --ai A (default 1), --mode (default hybrid, as the server's games).
	[ -n "$HUMANS_GIVEN" ] || HUMANS=1
	[ "$MODE" != simultaneous ] || [ -n "${MODE_GIVEN:-}" ] || MODE=hybrid
	run_one "$SEED" "$AI" "$REPORT" --humans "$HUMANS" --mode "$MODE" ${WORLD_SIZE:+--world-size "$WORLD_SIZE"}
	exit $?
fi

if [ "$SCENARIO" = peace ]; then
	# peace (N3.10): one game, --humans N (default 2), --mode simultaneous|hybrid, the humans played by the scenario.
	run_one "$SEED" "$AI" "$REPORT" --humans "$HUMANS" --mode "$MODE" ${WORLD_SIZE:+--world-size "$WORLD_SIZE"}
	exit $?
fi

if [ "$SCENARIO" = dirty ]; then
	# dirty (N3.8): one game, --humans N (default 2) bot humans, --ai A (default 2).
	run_one "$SEED" "$AI" "$REPORT" --humans "$HUMANS" ${WORLD_SIZE:+--world-size "$WORLD_SIZE"}
	exit $?
fi

if [ "$SCENARIO" = wbmapread ]; then
	[ -n "$MAPS" ] || { echo "run.sh: scenario wbmapread needs --maps DIR" >&2; exit 2; }
	margs=()
	if [ ${#MAP_LIST[@]} -eq 0 ]; then
		while IFS= read -r -d '' f; do MAP_LIST+=("${f#"$MAPS"/}"); done < <(find "$MAPS" -type f -iname '*.civ5map' -print0 | sort -z)
	fi
	for m in "${MAP_LIST[@]}"; do margs+=(--map "maps/$m"); done
	echo "run.sh: wbmapread: ${#MAP_LIST[@]} map(s) under $MAPS"
	run_one "$SEED" "$AI" "$REPORT" "${margs[@]}"
	exit $?
fi

if [ "$SCENARIO" = wbmap ]; then
	[ -n "$MAPS" ] && [ ${#MAP_LIST[@]} -eq 1 ] || { echo "run.sh: scenario wbmap needs --maps DIR and one --map REL" >&2; exit 2; }
	DIR="$(dirname "$REPORT")"
	r1f="$DIR/report-run1-$TARGET.json"; r2f="$DIR/report-run2-$TARGET.json"
	rm -f "$r1f" "$r2f"
	r1=0; r2=0
	run_one "$SEED" "$AI" "$r1f" --map "maps/${MAP_LIST[0]}" --scenario-data "$SCENARIO_DATA" || r1=$?
	run_one "$SEED" "$AI" "$r2f" --map "maps/${MAP_LIST[0]}" --scenario-data "$SCENARIO_DATA" || r2=$?
	d1="$(grep -o '"state_digest":"[0-9a-f]*"' "$r1f" 2>/dev/null | head -n 1 | cut -d'"' -f4 || true)"
	d2="$(grep -o '"state_digest":"[0-9a-f]*"' "$r2f" 2>/dev/null | head -n 1 | cut -d'"' -f4 || true)"
	same=false; [ -n "$d1" ] && [ "$d1" = "$d2" ] && same=true
	rc=0; { [ "$r1" = 0 ] && [ "$r2" = 0 ] && [ "$same" = true ]; } || rc=1
	ok=false; [ "$rc" = 0 ] && ok=true
	echo "{\"scenario\":\"wbmap\",\"task\":\"N3.20\",\"target\":\"$TARGET\",\"map\":\"${MAP_LIST[0]}\",\"scenario_data\":$SCENARIO_DATA,\"turns\":$TURNS,\"run1_rc\":$r1,\"run2_rc\":$r2,\"digest1\":\"$d1\",\"digest2\":\"$d2\",\"same_digest\":$same,\"ok\":$ok}" > "$REPORT"
	echo "wbmap-pair|target $TARGET|map ${MAP_LIST[0]}|scenario $SCENARIO_DATA|rc $r1/$r2|digest $d1 / $d2|same $same"
	echo "wbmap: $([ "$rc" = 0 ] && echo OK || echo FAILED)"
	exit $rc
fi

if [ "$SCENARIO" = humanturn ]; then
	run_one "$SEED" "$AI" "$REPORT" --game-type "$GAME_TYPE"
	exit $?
fi

if [ "$GAME_TYPE" = network ] && { [ "$SCENARIO" = autochoice ] || [ "$SCENARIO" = autochoice2 ]; }; then
	run_one "$SEED" "$AI" "$REPORT" --game-type network --humans "$HUMANS"
	exit $?
fi

if [ "$SCENARIO" != autoplay ]; then
	run_one "$SEED" "$AI" "$REPORT"
	exit $?
fi

# autoplay (F8, M4): the matrix --seeds x --ai (comma lists), one process per run (one game per process); per-run reports next to
# REPORT (report-s<seed>-a<ai>.json), REPORT itself lists them. The matrix passes when every run passes and at least one AI diplomacy
# request reached the human over all runs.
num() { { grep -o "\"$1\":-\?[0-9]*" "$2" 2>/dev/null || true; } | head -n 1 | cut -d: -f2; }
IFS=, read -r -a SEED_LIST <<< "$SEEDS"
IFS=, read -r -a AI_LIST <<< "$AI"
rc=0; runs=0; failed=0; diplo_total=0; runs_json=
DIR="$(dirname "$REPORT")"
for s in "${SEED_LIST[@]}"; do
	for a in "${AI_LIST[@]}"; do
		rep="$DIR/report-s$s-a$a.json"
		rm -f "$rep"
		t0=$(date +%s%N)
		r=0
		run_one "$s" "$a" "$rep" || r=$?
		ms=$(( ($(date +%s%N) - t0) / 1000000 ))
		turns=$(num turns_done "$rep"); stuck=$(num stuck "$rep"); lerr=$(num log_errors "$rep"); luaerr=$(num lua_errors "$rep")
		dreq=$(num diplo_requests "$rep"); ddec=$(num diplo_declined "$rep"); dtot=$(num diplo_total "$rep")
		echo "autoplay-matrix|target $TARGET|seed $s|ai $a|rc $r|turns ${turns:-?}|stuck ${stuck:-?}|log errors ${lerr:-?}|lua errors ${luaerr:-?}|diplo requests ${dreq:-?} (messages ${dtot:-?}, declined ${ddec:-?})|wall ${ms} ms"
		runs=$((runs + 1))
		[ "$r" = 0 ] || { failed=$((failed + 1)); rc=1; }
		diplo_total=$((diplo_total + ${dreq:-0}))
		runs_json+="${runs_json:+,}{\"seed\":$s,\"ai\":$a,\"rc\":$r,\"wall_ms\":$ms,\"report\":\"$(basename "$rep")\"}"
	done
done
[ "$diplo_total" -ge 1 ] || rc=1
ok=false; [ "$rc" = 0 ] && ok=true
echo "{\"scenario\":\"autoplay\",\"task\":\"F8\",\"milestone\":\"M4\",\"target\":\"$TARGET\",\"turns\":$TURNS,\"ok\":$ok,\"runs\":$runs,\"failed\":$failed,\"diplo_requests\":$diplo_total,\"run_list\":[$runs_json]}" > "$REPORT"
echo "autoplay: $([ "$rc" = 0 ] && echo OK || echo FAILED) ($runs run(s), $failed failed, $diplo_total AI diplomacy request(s) over the matrix, need >= 1)"
exit $rc
