#!/usr/bin/env bash
# Portable (clang) build of the Lekmod gamecore: headless/portable/README.md.
#
#   build.sh --mode lib   --target linux|wasm --out DIR [--jobs N] [--opt -O2] [--stdlib auto|libstdc++|libc++] [--threads]
#   build.sh --mode smoke --target linux|wasm --out DIR [--jobs N]
#   build.sh ... --target linux --pgo-gen            instrumented archive (a training run writes .profraw files, see pgo/README.md)
#   build.sh ... --target linux --pgo | --pgo-use F  profile-guided archive: the committed pgo/gamecore-linux.profdata, or the profdata F
#
# lib:   DIR/libgamecore.a (gamecore + rewritten Firaxis libraries + host layer + gamecore_abi.h v0)
#        and DIR/build-info.txt. No Lua and no SQLite objects inside: the consumer links those once.
# --threads (wasm only): the archive for a client link with a shared memory (Rust's threaded wasm build): wasm32-wasip1-threads
#   (atomics, bulk-memory, mutable-globals; the wasi-sdk sysroot with the libc/libc++ built the same way). The game core stays
#   single-threaded; same sources and defines, so the replica digests equal those of the single-threaded archive.
# smoke: lib, then a C harness linked against libgamecore.a + Lua 5.1.5 (built as C++) + the SQLite
#        amalgamation, checked for zero undefined symbols and run (wasm: under node's WASI).
#
# PGO (linux only; the wasm core is not profile-guided): -fprofile-instr-generate / -fprofile-instr-use. It changes inlining and code
# layout, not semantics: the build id (below) is unchanged, and the digest and the save of a turn are bit-equal with and without it
# (headless/tools/clang/pgo/README.md). The profile belongs to a source state; a stale one is still correct, only less fast.
#
# Environment: CXX/CC (linux, default clang++-18/clang-18), WASI_SDK (wasm), LUA_SRC (Lua 5.1.5 src
# dir for smoke), SQLITE_SRC (dir with sqlite3.c for smoke), see smoke/smoke.sh for defaults.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
GC_REL="LEKMOD_DLL/CvGameCoreDLL_Expansion2"
GC="$ROOT/$GC_REL"
PORT="$ROOT/headless/portable"

MODE=lib
TARGET=linux
OUT=
JOBS="$(nproc 2>/dev/null || echo 4)"
OPT="-O2"
STDLIB=auto
THREADS=0
PGO=none       # none | gen | use
PGO_PROFILE=

while [ $# -gt 0 ]; do
	case "$1" in
		--mode) MODE="$2"; shift 2 ;;
		--target) TARGET="$2"; shift 2 ;;
		--out) OUT="$2"; shift 2 ;;
		--jobs|-j) JOBS="$2"; shift 2 ;;
		--opt) OPT="$2"; shift 2 ;;
		--stdlib) STDLIB="$2"; shift 2 ;;
		--threads) THREADS=1; shift ;;
		--pgo-gen) PGO=gen; shift ;;
		--pgo) PGO=use; PGO_PROFILE="$HERE/pgo/gamecore-linux.profdata"; shift ;;
		--pgo-use) PGO=use; PGO_PROFILE="$2"; shift 2 ;;
		-h|--help) sed -n '2,16p' "$0"; exit 0 ;;
		*) echo "build.sh: unknown argument '$1'" >&2; exit 2 ;;
	esac
done
[ -n "$OUT" ] || { echo "build.sh: --out DIR is required" >&2; exit 2; }
case "$MODE" in lib|smoke) ;; *) echo "build.sh: --mode must be lib or smoke" >&2; exit 2 ;; esac
case "$TARGET" in linux|wasm) ;; *) echo "build.sh: --target must be linux or wasm" >&2; exit 2 ;; esac
[ "$THREADS" = 0 ] || [ "$TARGET" = wasm ] || { echo "build.sh: --threads is for --target wasm" >&2; exit 2; }
[ "$PGO" = none ] || [ "$TARGET" = linux ] || { echo "build.sh: --pgo / --pgo-gen / --pgo-use are for --target linux" >&2; exit 2; }
PGO_FLAGS=()
PGO_ID=none
case "$PGO" in
	gen) PGO_FLAGS=(-fprofile-instr-generate); PGO_ID=gen ;;
	use)
		[ -f "$PGO_PROFILE" ] || { echo "build.sh: profile $PGO_PROFILE not found (headless/tools/clang/pgo/README.md)" >&2; exit 2; }
		PGO_PROFILE="$(cd "$(dirname "$PGO_PROFILE")" && pwd)/$(basename "$PGO_PROFILE")"
		PGO_FLAGS=("-fprofile-instr-use=$PGO_PROFILE")
		PGO_ID="use $(sha1sum "$PGO_PROFILE" | cut -c1-12)" ;;
esac
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"

# ---- toolchain -----------------------------------------------------------------------------------
if [ "$TARGET" = linux ]; then
	CXX="${CXX:-$(command -v clang++-18 || command -v clang++)}"
	CC="${CC:-$(command -v clang-18 || command -v clang)}"
	AR="${AR:-$(command -v llvm-ar-18 || command -v llvm-ar || command -v ar)}"
	TARGET_FLAGS=(-fPIC)
	# libc++ when available: WASM uses it, and libstdc++/libc++ order std::sort ties differently.
	if [ "$STDLIB" = auto ]; then
		if echo '#include <vector>' | "$CXX" -stdlib=libc++ -x c++ -fsyntax-only - >/dev/null 2>&1; then STDLIB=libc++; else STDLIB=libstdc++; fi
	fi
	case "$STDLIB" in
		libc++) STDLIB_FLAGS=(-stdlib=libc++) ;;
		libstdc++) STDLIB_FLAGS=() ;;
		*) echo "build.sh: --stdlib must be auto, libstdc++ or libc++" >&2; exit 2 ;;
	esac
else
	[ -n "${WASI_SDK:-}" ] || { echo "build.sh: WASI_SDK is not set (e.g. WASI_SDK=~/wasi-sdk-33)" >&2; exit 2; }
	[ -x "$WASI_SDK/bin/clang++" ] || { echo "build.sh: wasi-sdk not found at WASI_SDK=$WASI_SDK" >&2; exit 2; }
	WASI_SDK="$(cd "$WASI_SDK" && pwd)"
	CXX="$WASI_SDK/bin/clang++"
	CC="$WASI_SDK/bin/clang"
	AR="$WASI_SDK/bin/llvm-ar"
	# _LIBCPP_USE_FROZEN_CXX03_HEADERS: the gamecore is C++03, and wasi-sdk's libc++ (22) then uses its frozen C++03 headers (__cxx03/),
	# whose containers behave like the libc++ 18 of the Linux build and like MSVC's. The current libc++ headers return an arbitrary
	# element from multimap::find (__find_equal stops at the first equal node met); the gamecore's find-then-iterate-to-upper_bound
	# loops (CvTraitEntry::IsFreePromotionUnitCombat, ...) then skip entries and the WASM game diverged from Linux (Songhai's
	# TRAIT_AMPHIB_WARLORD promotion missing on the starting warrior, seed 1 / 1 AI, visible at turn 44).
	# wasip1-threads: libc/libc++ of the sysroot built with atomics (a link with --shared-memory refuses objects without them).
	WASM_TRIPLE=wasm32-wasip1; [ "$THREADS" = 0 ] || WASM_TRIPLE=wasm32-wasip1-threads
	TARGET_FLAGS=("--target=$WASM_TRIPLE" "--sysroot=$WASI_SDK/share/wasi-sysroot" -fwasm-exceptions -mllvm -wasm-use-legacy-eh=false -D_WASI_EMULATED_PROCESS_CLOCKS
		-D_LIBCPP_USE_FROZEN_CXX03_HEADERS)
	[ "$THREADS" = 0 ] || TARGET_FLAGS+=(-matomics -mbulk-memory -mmutable-globals)
	STDLIB=libc++
	STDLIB_FLAGS=(-stdlib=libc++)
fi
command -v python3 >/dev/null || { echo "build.sh: python3 is required (VFS overlay)" >&2; exit 2; }

# ---- flags ---------------------------------------------------------------------------------------
# Defines of the Release|Win32 configuration of CvGameCoreDLL_Expansion2.vcproj, plus the port's own.
DEFINES=(FXS_IS_DLL WIN32 NDEBUG _WINDOWS _USRDLL CVGAMECOREDLL_EXPORTS FINAL_RELEASE _CRT_SECURE_NO_WARNINGS
	LEKMOD_PORTABLE LEKMOD_LUA_INTEGER=int
	# Approved core fixes (user 2026-10-21, B1): FIX-0010 ruin memory read in Write's order (save/load continuation),
	# HYG-0011 first element of a multimap key (linux/wasm), HYG-0012 combat lines with type and plot (journal).
	# Registry: headless/improvements/{fixes,hyg}.json.
	LEKMOD_HEADLESS_FIX_0010 LEKMOD_HEADLESS_HYG_MULTIMAP_FIRST_EQUAL LEKMOD_HEADLESS_HYG_COMBAT_MESSAGE_PLOT
	# Server set (user 2026-10-21, B2 gc-abi-v2 4.13). A: save/load continuation and server-safe queries.
	LEKMOD_HEADLESS_FIX_0001 LEKMOD_HEADLESS_FIX_0002 LEKMOD_HEADLESS_FIX_0009 LEKMOD_HEADLESS_FIX_0011
	LEKMOD_HEADLESS_FIX_0012 LEKMOD_HEADLESS_FIX_0013 LEKMOD_HEADLESS_FIX_0014 LEKMOD_HEADLESS_FIX_0015
	LEKMOD_HEADLESS_FIX_0016 LEKMOD_HEADLESS_FIX_0018 LEKMOD_HEADLESS_FIX_0019 LEKMOD_HEADLESS_FIX_0020
	LEKMOD_HEADLESS_FIX_0021 LEKMOD_HEADLESS_FIX_0022 LEKMOD_HEADLESS_FIX_0023 LEKMOD_HEADLESS_FIX_0025 LEKMOD_HEADLESS_FIX_0030
	# B: command validation, FIX-0101 0102 0104 0105 0106 0107 0108 0109 0111 0112 0113.
	LEKMOD_HEADLESS_FIX_VAL_RESEARCH LEKMOD_HEADLESS_FIX_VAL_CHANGE_WAR LEKMOD_HEADLESS_FIX_VAL_FOUND_RELIGION
	LEKMOD_HEADLESS_FIX_VAL_ENHANCE_RELIGION LEKMOD_HEADLESS_FIX_VAL_DIPLOMACY_FROM_UI LEKMOD_HEADLESS_FIX_VAL_MINOR_CIV
	LEKMOD_HEADLESS_FIX_VAL_GOODY_CHOICE LEKMOD_HEADLESS_FIX_VAL_LEAGUE LEKMOD_HEADLESS_FIX_VAL_RESEARCH_TECH
	LEKMOD_HEADLESS_FIX_VAL_PUSH_MISSION LEKMOD_HEADLESS_FIX_VAL_LUA_FOUNDER
	# C: HYG-0002..0009 uninitialised-field hygiene.
	LEKMOD_HEADLESS_HYG_REPLAY_MSG_INIT LEKMOD_HEADLESS_HYG_NOTIFICATION_INIT LEKMOD_HEADLESS_HYG_PANTHEON_HOLYCITY_INIT
	LEKMOD_HEADLESS_HYG_MILITARY_AI_LASTTURN_INIT LEKMOD_HEADLESS_HYG_MINOR_BULLY_TOTALS_INIT
	LEKMOD_HEADLESS_HYG_TRAIT_COMBAT_IMPROVEMENT_INIT LEKMOD_HEADLESS_HYG_CULTURE_OPINION_INIT
	LEKMOD_HEADLESS_HYG_AIOP_REPLACE_LOSSES_INIT
	# D: HYG-0013 core message lines to every human (approved 2026-10-21, B2; UI only).
	LEKMOD_HEADLESS_HYG_MESSAGES_TO_HUMANS)
# E: OPT set of the portable build (2026-10-31, live game 936: the AI phase of a Huge 12-seat game at turn 126 took 14 s in one call).
# The release OPTs of headless/improvements/opts.json that are self-contained here: left out are OPT-0001..0009 (they map onto AUI_/NQM_
# defines in HeadlessImprovements.h, which the portable build does not include, so they would be no-ops) and OPT-0022 (its plot-block
# hooks are declared there too). Same digest and same save after three turns of the live save and six of the turn before (bit-equal),
# x3.5..4 on the AI phase. Not part of the build id (below). LEKMOD_NO_OPT=1 builds without them (A/B).
OPT_DEFINES=(LEKMOD_HEADLESS_OPT_GAMEOPTION_CACHE LEKMOD_HEADLESS_OPT_BULLY_ICON_BATCH LEKMOD_HEADLESS_OPT_ASTAR_MOVE_MEMO
	LEKMOD_HEADLESS_OPT_ASTAR_NODE_CACHE_MEMO LEKMOD_HEADLESS_OPT_SITE_PLOT_MEMO LEKMOD_HEADLESS_OPT_ASTAR_WALK_MEMO
	LEKMOD_HEADLESS_OPT_DIPLO_RESPONSE_MEMO LEKMOD_HEADLESS_OPT_ASTAR_FROM_PLOT_MEMO LEKMOD_HEADLESS_OPT_TOURISM_BUILDING_FIRST
	LEKMOD_HEADLESS_OPT_INFOTYPE_STRING_MEMO LEKMOD_HEADLESS_OPT_BELIEF_PLOT_HOIST LEKMOD_HEADLESS_OPT_BELIEF_OWNED_LOOP
	LEKMOD_HEADLESS_OPT_DB_SPARSE_LISTS LEKMOD_HEADLESS_OPT_YIELD_DEFICIENT_MEMO LEKMOD_HEADLESS_OPT_TRADE_NEED_SHARE
	LEKMOD_HEADLESS_OPT_NATURE_PREREQ_SKIP LEKMOD_HEADLESS_OPT_CITIZEN_FOOD_MEMO LEKMOD_HEADLESS_OPT_BULLY_DISK_LISTS
	LEKMOD_HEADLESS_OPT_TOURISM_SPARSE_CLASSES LEKMOD_HEADLESS_OPT_GOODY_TEST_ORDER LEKMOD_HEADLESS_OPT_SITE_RING_TABLE
	LEKMOD_HEADLESS_OPT_POLICY_NUMERIC_SPARSE LEKMOD_HEADLESS_OPT_INLINE_ACCESSORS LEKMOD_HEADLESS_OPT_MAP_PLOT_FASTPATH
	LEKMOD_HEADLESS_OPT_ASTAR_OPEN_BUCKETS LEKMOD_HEADLESS_OPT_ASTAR_UNREACHABLE_STOP)
[ "${LEKMOD_NO_OPT:-0}" = 1 ] || DEFINES+=("${OPT_DEFINES[@]}")
# A/B builds (scenario mpmsg): LEKMOD_WITHOUT_DEFINES="LEKMOD_HEADLESS_HYG_MESSAGES_TO_HUMANS ..." drops those defines (the build id changes with them).
if [ -n "${LEKMOD_WITHOUT_DEFINES:-}" ]; then
	KEPT=()
	for d in "${DEFINES[@]}"; do
		skip=0
		for w in $LEKMOD_WITHOUT_DEFINES; do [ "$d" = "$w" ] && skip=1; done
		[ "$skip" = 1 ] || KEPT+=("$d")
	done
	DEFINES=("${KEPT[@]}")
fi
# Measurements (perf studies): LEKMOD_WITH_DEFINES="LEKMOD_HEADLESS_OPT_... ..." adds defines.
if [ -n "${LEKMOD_WITH_DEFINES:-}" ]; then
	for w in $LEKMOD_WITH_DEFINES; do DEFINES+=("$w"); done
fi
# Build id of the gamecore (RtSession::BuildId, in every save header): the set of FIX / HYG defines is part of it, so a save written by a build with another
# set is refused with GC_ERR_BAD_DATA: "<tag>-fx<count>-<first 8 hex of the sha1 of the sorted LEKMOD_HEADLESS_* defines>".
# LEKMOD_BUILD_TAG (environment) replaces the leading "dev".
# The OPT defines (LEKMOD_HEADLESS_OPT_*) are left out: an OPT changes no result (headless/improvements/opts.json "equivalence"; the
# digest and the save of a turn are bit-equal with and without it), so a save of a build with another OPT set loads, and a server core
# with OPTs serves replicas whose core has none.
FLAGSET_COUNT="$(printf '%s\n' "${DEFINES[@]}" | grep '^LEKMOD_HEADLESS_' | grep -vc '^LEKMOD_HEADLESS_OPT_' || true)"
FLAGSET_HASH="$(printf '%s\n' "${DEFINES[@]}" | grep '^LEKMOD_HEADLESS_' | grep -v '^LEKMOD_HEADLESS_OPT_' | LC_ALL=C sort | sha1sum | cut -c1-8)"
DEFINES+=("LEKMOD_PORTABLE_BUILD_TAG=\"${LEKMOD_BUILD_TAG:-dev}-fx${FLAGSET_COUNT}-${FLAGSET_HASH}\"")
# Determinism: no FMA contraction, no fast-math, defined signed overflow, no type-based aliasing.
# -fno-delete-null-pointer-checks: the gamecore null-tests references (`&getLeaderInfo() == NULL`, MSVC keeps
# those tests); clang folds them away and the observer slot crashed in Init2 (B1 A9, operator decision 2026-10-20).
SEMANTIC_FLAGS=(-ffp-contract=off -fno-fast-math -fwrapv -fno-strict-aliasing -fno-delete-null-pointer-checks)
# MSVC 2008 dialect: dependent-base lookup, lazy template bodies, __declspec/__int64. -fms-compatibility drops
# the GNU macros clang normally predefines; glibc/libstdc++ (and FastDelegate's member-pointer code) need them.
DIALECT_FLAGS=(-std=gnu++03 -fms-extensions -fms-compatibility -fdelayed-template-parsing
	-D__GNUC__=4 -D__GNUC_MINOR__=2 -D__GNUC_PATCHLEVEL__=1 -D__GNUG__=4 -D__GNUC_GNU_INLINE__=1)
WARN_FLAGS=(-w -Wno-invalid-token-paste)
INCLUDES=("$PORT/include" "$GC" "$GC/CvWorldBuilderMap/include" "$GC/CvGameCoreDLLUtil/include"
	"$GC/CvLocalization/include" "$GC/CvGameDatabase/include" "$GC/FirePlace/include"
	"$GC/FirePlace/include/FireWorks" "$GC/ThirdPartyLibs/Lua51/include" "$PORT/third_party/sqlite" "$PORT/abi")

[ "$TARGET" = wasm ] && INCLUDES=("$PORT/include-wasm" "${INCLUDES[@]}")

OBJ="$OUT/obj"
mkdir -p "$OBJ"
OVERLAY="$OBJ/vfs-overlay.yaml"
python3 "$HERE/gen_vfs_overlay.py" "$OVERLAY" "$GC" "$PORT/include" "$PORT/include-wasm"

COMMON=("${TARGET_FLAGS[@]}" "${STDLIB_FLAGS[@]}" "${DIALECT_FLAGS[@]}" "${SEMANTIC_FLAGS[@]}" "$OPT" "${PGO_FLAGS[@]}" -g0
	"${WARN_FLAGS[@]}" -ivfsoverlay "$OVERLAY" -include lekmod_portable/prelude.h)
for d in "${DEFINES[@]}"; do COMMON+=("-D$d"); done
for i in "${INCLUDES[@]}"; do COMMON+=("-I$i"); done

# Objects are reused only with identical flags.
FLAGS_FILE="$OBJ/flags.txt"
printf '%s\n' "$CXX" "${COMMON[@]}" "pgo: $PGO_ID" > "$FLAGS_FILE.new"
if [ -f "$FLAGS_FILE" ] && ! cmp -s "$FLAGS_FILE" "$FLAGS_FILE.new"; then rm -rf "$OBJ/gamecore" "$OBJ/portable"; fi
mv "$FLAGS_FILE.new" "$FLAGS_FILE"

# ---- sources -------------------------------------------------------------------------------------
# The gamecore list is the one of the MSVC project (RelativePath entries ending in .cpp).
mapfile -t GC_SRCS < <(grep -o 'RelativePath="[^"]*\.cpp"' "$GC/CvGameCoreDLL_Expansion2.vcproj" |
	sed -e 's/^RelativePath="//' -e 's/"$//' -e 's/\\/\//g' -e 's/^\.\///' | sort -u)
mapfile -t PORT_SRCS < <(cd "$PORT" && find firaxis host abi runtime -name '*.cpp' 2>/dev/null | sort)

# ---- Makefile (incremental, header deps via -MMD) --------------------------------------------------
quote() { local a out=""; for a in "$@"; do out+=" '${a//\'/\'\\\'\'}'"; done; printf '%s' "$out"; }
MK="$OBJ/build.mk"
{
	echo "CXX := '$CXX'"
	echo "AR := '$AR'"
	echo "COMMON :=$(quote "${COMMON[@]}")"
	echo ".SUFFIXES:"
	echo "all: $OUT/libgamecore.a"
	# No precompiled header: clang loses the libstdc++ __is_pointer keyword reversion across a PCH,
	# which breaks the delayed (MSVC-style) template parsing the gamecore needs.
	objs=()
	for s in "${GC_SRCS[@]}"; do
		o="$OBJ/gamecore/${s%.cpp}.o"
		objs+=("$o")
		echo "$o: $GC/$s"
		echo "	@mkdir -p \$(@D)"
		echo "	@echo '  CXX $GC_REL/$s'"
		echo "	@\$(CXX) \$(COMMON) -MMD -MP -c \$< -o \$@"
		echo "-include ${o%.o}.d"
	done
	for s in "${PORT_SRCS[@]}"; do
		o="$OBJ/portable/${s%.cpp}.o"
		objs+=("$o")
		echo "$o: $PORT/$s"
		echo "	@mkdir -p \$(@D)"
		echo "	@echo '  CXX headless/portable/$s'"
		echo "	@\$(CXX) \$(COMMON) -MMD -MP -c \$< -o \$@"
		echo "-include ${o%.o}.d"
	done
	echo "$OUT/libgamecore.a: ${objs[*]}"
	echo "	@rm -f \$@.tmp"
	echo "	\$(AR) qcs \$@.tmp \$^"
	echo "	@mv \$@.tmp \$@"
} > "$MK.tmp"
mv "$MK.tmp" "$MK"

echo "build.sh: $TARGET, ${#GC_SRCS[@]} gamecore + ${#PORT_SRCS[@]} portable sources, -j$JOBS"
make -f "$MK" -j"$JOBS" ${MAKE_KEEP_GOING:+-k}

# ---- build-info.txt ------------------------------------------------------------------------------
# GIT_COMMIT overrides: WSL git cannot read a worktree created by Windows git (absolute Windows-path gitdir).
COMMIT="${GIT_COMMIT:-$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo unknown)}"
DIRTY="$(git -C "$ROOT" status --porcelain -- "$GC_REL" headless/portable headless/tools/clang 2>/dev/null | head -c1 || true)"
{
	echo "target: $TARGET"
	echo "compiler: $("$CXX" --version | head -n1)"
	echo "compiler path: $CXX"
	echo "git commit: $COMMIT${DIRTY:+ (dirty)}"
	echo "archives: libgamecore.a"
	echo "stdlib: $STDLIB"
	echo "flags: ${TARGET_FLAGS[*]} ${STDLIB_FLAGS[*]} ${DIALECT_FLAGS[*]} ${SEMANTIC_FLAGS[*]} $OPT"
	echo "pgo: $PGO_ID${PGO_PROFILE:+ ($PGO_PROFILE)}"
	echo "defines: ${DEFINES[*]}"
	echo "abi header: headless/portable/abi/gamecore_abi.h (GC_ABI_VERSION 1)"
	echo "lua headers: $GC_REL/ThirdPartyLibs/Lua51/include (extern \"C\", LEKMOD_LUA_INTEGER=int) lua_Integer = int"
	echo "sqlite: sqlite3 C API 3.x, not included (header used: headless/portable/third_party/sqlite/sqlite3.h)"
	if [ "$TARGET" = linux ]; then
		echo "link: libgamecore.a + Lua 5.1.5 (C linkage) + sqlite3 + $STDLIB -lm -lpthread"
	else
		echo "link: libgamecore.a + Lua 5.1.5 (C linkage) + sqlite3 (SQLITE_THREADSAFE=0) + libc++ libc++abi libunwind (sysroot lib/$WASM_TRIPLE/eh) + libc"
	fi
} > "$OUT/build-info.txt"
echo "build.sh: $OUT/libgamecore.a"

if [ "$MODE" = smoke ]; then
	exec "$HERE/smoke/smoke.sh" --lib-dir "$OUT" --target "$TARGET" --jobs "$JOBS"
fi
