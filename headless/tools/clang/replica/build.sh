#!/usr/bin/env bash
# Links the G0 spike of B4 gc-replica (replica_spike.c) against a build.sh --mode lib output: libgamecore.a + Lua 5.1.5 (as C++ with C
# linkage) + the SQLite amalgamation, as smoke/smoke.sh does.
#
#   replica/build.sh --lib-dir DIR [--target linux|wasm] [--jobs N]     -> DIR/replica/replica_spike[.wasm]
#
# LUA_SRC, SQLITE_SRC, WASI_SDK: as smoke/smoke.sh. Run (wasm): node --experimental-wasm-exnref smoke/run_wasi.mjs DIR/replica/replica_spike.wasm
#   --preopen /game=GAMEDATA --preopen /data=SPIKEDIR -- --mode replica --game /game --snap /data/snap-100.gcs1 ...
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../../.." && pwd)"
GC="$ROOT/LEKMOD_DLL/CvGameCoreDLL_Expansion2"
LIB_DIR=
TARGET=linux
JOBS="$(nproc 2>/dev/null || echo 4)"
while [ $# -gt 0 ]; do
	case "$1" in
		--lib-dir) LIB_DIR="$2"; shift 2 ;;
		--jobs|-j) JOBS="$2"; shift 2 ;;
		--target) TARGET="$2"; shift 2 ;;
		*) echo "replica/build.sh: unknown argument '$1'" >&2; exit 2 ;;
	esac
done
[ -n "$LIB_DIR" ] && [ -f "$LIB_DIR/libgamecore.a" ] || { echo "replica/build.sh: --lib-dir must contain libgamecore.a" >&2; exit 2; }
LIB_DIR="$(cd "$LIB_DIR" && pwd)"
LUA_SRC="${LUA_SRC:-$ROOT/../civ5-reimagened/third_party/lua-5.1.5/src}"
SQLITE_SRC="${SQLITE_SRC:-$ROOT/../civ5-reimagened/third_party/sqlite-3.53.2}"
[ -f "$LUA_SRC/lapi.c" ] || { echo "replica/build.sh: Lua 5.1.5 sources not found (set LUA_SRC)" >&2; exit 2; }
[ -f "$SQLITE_SRC/sqlite3.c" ] || { echo "replica/build.sh: sqlite3.c not found (set SQLITE_SRC)" >&2; exit 2; }

if [ "$TARGET" = wasm ]; then
	[ -n "${WASI_SDK:-}" ] && [ -x "$WASI_SDK/bin/clang++" ] || { echo "replica/build.sh: WASI_SDK is not set or invalid" >&2; exit 2; }
	SYSROOT="$WASI_SDK/share/wasi-sysroot"
	CXX="$WASI_SDK/bin/clang++"
	CC="$WASI_SDK/bin/clang"
	TFLAGS=(--target=wasm32-wasip1 "--sysroot=$SYSROOT" -fwasm-exceptions -mllvm -wasm-use-legacy-eh=false
		-D_WASI_EMULATED_PROCESS_CLOCKS -D_WASI_EMULATED_GETPID -D_WASI_EMULATED_MMAN -D_WASI_EMULATED_SIGNAL)
	CXXSTD=(-stdlib=libc++)
	SQLITE_FLAGS=(-DSQLITE_THREADSAFE=0 -DSQLITE_TEMP_STORE=3 -DSQLITE_OMIT_WAL -DLONGDOUBLE_TYPE=double)
	EXE=replica_spike.wasm
else
	CXX="${CXX:-$(command -v clang++-18 || command -v clang++)}"
	CC="${CC:-$(command -v clang-18 || command -v clang)}"
	TFLAGS=(-fPIC)
	CXXSTD=()
	grep -q '^stdlib: libc++' "$LIB_DIR/build-info.txt" 2>/dev/null && CXXSTD=(-stdlib=libc++)
	SQLITE_FLAGS=(-DSQLITE_THREADSAFE=1)
	EXE=replica_spike
fi
OUT="$LIB_DIR/replica"
mkdir -p "$OUT/lua"
LUA_INC="$GC/ThirdPartyLibs/Lua51/include"
objs=()
pids=()
for src in "$LUA_SRC"/*.c; do
	name="$(basename "$src" .c)"
	case "$name" in lua|luac|print) continue ;; esac
	objs+=("$OUT/lua/$name.o")
	[ "$OUT/lua/$name.o" -nt "$src" ] && continue
	wrap="$OUT/lua/$name.cpp"
	printf '#if defined(__wasi__)\n#include <stdio.h>\n#define L_tmpnam 32\nstatic inline char* lua_no_tmpnam(char*) { return NULL; }\nstatic inline FILE* lua_no_tmpfile(void) { return NULL; }\nstatic inline int lua_no_system(const char* c) { return c == NULL ? 0 : -1; }\n#endif\n' > "$wrap"
	printf '#include <assert.h>\n#include <ctype.h>\n#include <errno.h>\n#include <float.h>\n#include <limits.h>\n#include <locale.h>\n#include <math.h>\n#include <setjmp.h>\n#include <stdarg.h>\n#include <stddef.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include <time.h>\n#if defined(__wasi__)\n#define tmpnam(b) lua_no_tmpnam(b)\n#define tmpfile() lua_no_tmpfile()\n#define system(c) lua_no_system(c)\n#endif\nextern "C" {\n#include "%s"\n}\n' "$src" >> "$wrap"
	"$CXX" "${TFLAGS[@]}" "${CXXSTD[@]}" -std=gnu++14 -O2 -w -DLEKMOD_LUA_INTEGER=int -I"$LUA_INC" -I"$LUA_SRC" -c "$wrap" -o "$OUT/lua/$name.o" &
	pids+=($!)
	if [ "${#pids[@]}" -ge "$JOBS" ]; then wait "${pids[0]}"; pids=("${pids[@]:1}"); fi
done
[ "$OUT/sqlite3.o" -nt "$SQLITE_SRC/sqlite3.c" ] ||
	"$CC" "${TFLAGS[@]}" -O2 -w -DSQLITE_CORE -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_DEPRECATED -DSQLITE_DQS=0 "${SQLITE_FLAGS[@]}" -c "$SQLITE_SRC/sqlite3.c" -o "$OUT/sqlite3.o"
for p in "${pids[@]}"; do wait "$p"; done
"$CC" "${TFLAGS[@]}" -std=c99 -O2 -Wall -Wextra -I"$ROOT/headless/portable/abi" -c "$HERE/replica_spike.c" -o "$OUT/replica_spike.o"
if [ "$TARGET" = wasm ]; then
	"$CXX" "${TFLAGS[@]}" "${CXXSTD[@]}" -O2 -o "$OUT/$EXE" "$OUT/replica_spike.o" -Wl,--whole-archive "$LIB_DIR/libgamecore.a" -Wl,--no-whole-archive \
		"${objs[@]}" "$OUT/sqlite3.o" -L"$SYSROOT/lib/wasm32-wasip1/eh" -lc++ -lc++abi -lunwind \
		-lwasi-emulated-process-clocks -lwasi-emulated-getpid -lwasi-emulated-mman -lwasi-emulated-signal -Wl,-z,stack-size=8388608
else
	"$CXX" "${CXXSTD[@]}" -o "$OUT/$EXE" "$OUT/replica_spike.o" -Wl,--whole-archive "$LIB_DIR/libgamecore.a" -Wl,--no-whole-archive \
		"${objs[@]}" "$OUT/sqlite3.o" -Wl,--no-undefined -lm -lpthread
fi
echo "replica/build.sh: $OUT/$EXE"
