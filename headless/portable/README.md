# Portable gamecore (clang, Linux and WASM)

The Lekmod gamecore (`LEKMOD_DLL/CvGameCoreDLL_Expansion2`, the 161 sources of the MSVC project) built
with clang outside Windows, the closed Firaxis libraries it links against rewritten, and the narrow C ABI
`gamecore_abi.h`.

## Build

```sh
headless/tools/clang/build.sh --mode lib   --target linux --out build/gamecore-linux [--jobs N] [--opt -O2]
headless/tools/clang/build.sh --mode lib   --target wasm  --out build/gamecore-wasm      # needs wasi-sdk 33
headless/tools/clang/build.sh --mode smoke --target linux --out build/gamecore-linux     # lib + end-to-end test
headless/tools/clang/build.sh --mode smoke --target wasm  --out build/gamecore-wasm      # same, run under node's WASI
```

- Requirements: bash, make, python3 (VFS overlay), clang 18+ (`CXX`/`CC` override), llvm-ar.
- wasm: `WASI_SDK` (path of a wasi-sdk 33 install; required, no default).
- Use one `--out` per target/configuration; objects are discarded when the flags change.
- smoke (wasm also needs node 22+, run with `--experimental-wasm-exnref`): `LUA_SRC` (Lua 5.1.5 `src/`, default `../civ5-reimagened/third_party/lua-5.1.5/src`) and
  `SQLITE_SRC` (directory with `sqlite3.c`, default `../civ5-reimagened/third_party/sqlite-3.53.2`).
- Incremental: the script writes `<out>/obj/build.mk` with header dependencies; rerunning recompiles only
  what changed.

Outputs in `<out>`:

| File | Content |
|---|---|
| `libgamecore.a` | gamecore + `headless/portable/{firaxis,host,abi}`. No Lua, no SQLite objects. |
| `build-info.txt` | target, compiler, commit, flags, defines, `archives:`, `stdlib:`, `lua headers:` line |
| `obj/` | objects, Makefile, VFS overlay |

## Consumer interface

- Archive: `libgamecore.a` (the only one; `archives: libgamecore.a` in build-info.txt).
- Header: `headless/portable/abi/gamecore_abi.h` (the civ5-reimagened server keeps a verbatim copy in
  `crates/shared/gamecore-sys/cpp/include/gamecore_abi.h`).
- Lua headers: `LEKMOD_DLL/CvGameCoreDLL_Expansion2/ThirdPartyLibs/Lua51/include` with
  `-DLEKMOD_LUA_INTEGER=int` (`lua_Integer` is 4 bytes, as `ptrdiff_t` is on Win32). The gamecore includes
  them inside `extern "C"`, so Lua 5.1.5 must be linked with C linkage (built as C++ through
  `extern "C" { #include "x.c" }` wrappers, as `lua-sys` does).
- SQLite: the sqlite3 C API 3.x, linked by the consumer (`SQLITE_THREADSAFE=0` on WASM is fine: the
  gamecore uses one thread). `headless/portable/third_party/sqlite/sqlite3.h` (3.46.0, public domain) is only
  used to compile.
- Linux link: `libgamecore.a` + Lua + sqlite3 + the C++ runtime of the build (`stdlib:` line) `-lm -lpthread`.
- WASM link: `libgamecore.a` + Lua + sqlite3 + `c++ c++abi unwind c` from `lib/wasm32-wasip1/eh` (a subset of
  the recipe of `build_support::link_wasm_runtime()`), exnref EH. The archive references no emulated WASI
  library; it is compiled with `-D_WASI_EMULATED_PROCESS_CLOCKS` like the consumer's C/C++.
- Static constructors: the archive has some (gamecore globals, `g_CRC32`, FLua); on WASM they run from
  `__wasm_call_ctors`, before the first gc_* call.
- `LekmodPortable_DllAttach()` / `LekmodPortable_DllDetach()` (`headless/portable/host/PortableHost.h`)
  replace `DllMain`; `gc_create`/`gc_destroy` call them, other hosts may call them directly.

`build-info.txt` example (Linux):

```
target: linux
compiler: Ubuntu clang version 18.1.3 (1ubuntu1)
compiler path: /usr/bin/clang++-18
git commit: <sha>
archives: libgamecore.a
stdlib: libstdc++
flags: -fPIC -std=gnu++03 -fms-extensions -fms-compatibility -fdelayed-template-parsing ... -O2
defines: FXS_IS_DLL WIN32 NDEBUG _WINDOWS _USRDLL CVGAMECOREDLL_EXPORTS FINAL_RELEASE _CRT_SECURE_NO_WARNINGS LEKMOD_PORTABLE LEKMOD_LUA_INTEGER=int
abi header: headless/portable/abi/gamecore_abi.h (GC_ABI_VERSION 0)
lua headers: LEKMOD_DLL/CvGameCoreDLL_Expansion2/ThirdPartyLibs/Lua51/include (extern "C", LEKMOD_LUA_INTEGER=int) lua_Integer = int
sqlite: ...
link: ...
```

## How the port works

- Dialect: `-std=gnu++03 -fms-extensions -fms-compatibility -fdelayed-template-parsing` reproduces the
  MSVC 2008 leniencies the sources rely on (unqualified lookup into dependent bases, lazily parsed template
  bodies, `__declspec`, `__int64`). `-fms-compatibility` drops clang's `__GNUC__` macros, which glibc,
  libstdc++ and FastDelegate need, so build.sh defines them again.
- No precompiled header: clang loses libstdc++'s `__is_pointer` keyword reversion across a PCH, which
  breaks delayed template parsing.
- Case-insensitive includes: `gen_vfs_overlay.py` maps every header onto itself in a clang VFS overlay with
  `case-sensitive: false` (`"Fireworks/FDataStream.h"` finds `FireWorks/FDataStream.h`); sources are untouched.
- `headless/portable/include`: `windows.h` (Win32 types with Win32 widths: `DWORD`/`LONG`/`ULONG` are 32 bit),
  interlocked ops, critical sections, stdio-backed file `HANDLE`s, UTF-8 code pages, heaps over malloc,
  `tchar.h`, `dbghelp.h`/minidump types (dumps always fail), and `lekmod_portable/`:
  - `prelude.h` (forced with `-include`): MSVC CRT extensions (`sprintf_s`, `_itoa_s`, `_stricmp`, ...),
    SAL annotations, `std::tr1::shared_ptr`, `FMemHooks.h` (pool enum used by template defaults).
  - `det_containers.h`: `stdext::hash_map/hash_set` and `std::tr1::unordered_map/unordered_set` as ordered
    trees, plus `std::tr1::array` (`<array>`).
- `headless/portable/include-wasm`: SSE headers reduced to the prefetch no-op, `alloca.h`.
- `headless/portable/firaxis`: the closed libraries (below). `headless/portable/host`: gDLL/UI objects
  (`gen/` is generated by `headless/tools/clang/gen_portable_engine.py` from the SDK headers and
  `headless/host/api/engine-api.csv`: unimplemented calls log once and return zero values).

## Verified

- Linux x86_64 (clang 18, libstdc++, -O2) and wasm32-wasip1 (wasi-sdk 33, clang 22, libc++, exnref EH):
  `build.sh --mode smoke` links `smoke.c` with the whole archive, Lua 5.1.5 and SQLite 3.53.2 without undefined
  symbols and passes all 28 checks. Snapshots are byte-identical on both targets
  (`4c475331 01000000 01000000 01000000 | 01000000 7e16dcd3 01000000 03000000 00`: header, then CvRandom
  version/seed/calls/resets/flag).

## Rewritten Firaxis libraries

All in `libgamecore.a`. About 300 symbols were missing after stage 1; `llvm-nm` now shows no undefined
symbol outside libc/libm/pthread, the C++ runtime, Lua and sqlite3, and the smoke test links the whole
archive with `--no-undefined`.

| Original | Portable file | Status |
|---|---|---|
| FireWorks FDataStream, FMemoryStream | `firaxis/FDataStream.cpp` | real; Win32 byte format (LE, 4-byte `long`/`size_t`, INT32-length strings, UTF-16 wide strings) |
| FireWorks FString (FStringA/W out-of-line members) | `firaxis/FString.cpp` | real (members the gamecore uses) |
| FAutoArchive, FAutoVariableBase, FSerialization | `firaxis/FireWorksMisc.cpp` | real (delta = UINT32 count + (index, value)) |
| FCriticalSection, FCRC/g_CRC32, FTimer, aligned alloc, TempHeap | `firaxis/FireWorksMisc.cpp` | real (TempHeap allocates from the C heap) |
| FILogFileMgr / FILogFile | `firaxis/FireWorksMisc.cpp` | real: gamecore logs go to `gc_host.log` (DEBUG) |
| FIFileSystem, FFileStream, FDebugHelper | `firaxis/FireWorksMisc.cpp`, `FDataStream.cpp` | stub: fail (no file system; game files come through the host) |
| FMemoryStreamRLE | `firaxis/FDataStream.cpp` | pass-through, not RLE-compatible (unused) |
| FLua (errors, protected calls, Value/Table/String refs, GetField, CreateLuaState) | `firaxis/FLua.cpp` | real |
| CvGameDatabase: Database::Connection, Results, SingleResult, ResultsCache, BinaryIO | `firaxis/Database.cpp` | real over sqlite3 (no statement cache; BinaryIO via the backup API; a SingleResult comes back positioned on its row) |
| CvLocalization: String, SharedString, StringDictionary, Lookup | `firaxis/CvLocalization.cpp` | partial: lookup in `Language_<type>(Tag, Text)`, `{N}`, `{N_Name}`, `{TXT_KEY_...}` substitution; plural/gender/number transforms not evaluated |
| CvGameCoreDLLUtil: CvEnumSerialization, CvStringUtils | `firaxis/CvEnumSerialization.cpp`, `Win32Compat.cpp` | real (enums as INT32; UTF-8 <-> wchar_t) |
| CvWorldBuilderMap (+ TypeDesc, TypeGroup, ModData) | `firaxis/CvWorldBuilderMap.cpp`, `firaxis/CvWorldBuilderMapLoad.cpp` | real: Load and PreviewPlayableCivCount of .Civ5Map versions 6-12 (whole file through `gc_host.read_file`, records decoded from the i386 layout), ModData; Save fails (logged). 91/91 install maps load (scenario `wbmapread`) |

## ABI (`abi/gc_abi.cpp`, `abi/gamecore_abi.h` v1)

A thin wrapper over `runtime/rt_session`. Each entry validates its arguments, runs in try/catch and
reports errors only through the return code and `gc_error`.
- `gc_create(host, err)`: `RtSession::Load` (D1-D2, B1-B4, I1-I8) through `host.read_file`; one instance per process.
- `gc_new_game(setup_json, err)`: `RtSetup::Parse` first (a bad setup is `GC_ERR_INVALID_ARG` and keeps the slot), then `RtSession::NewGame`.
- `gc_pump(max_slices, out, err)`: `RtSession::Pump`; emits `TURN_STARTED` per turn change and `GAME_OVER` once; `GC_ERR_STUCK` on a stuck turn.
- `gc_state_digest(viewer_team, out, err)`: `RtSession::StateDigest` (whole game until the viewer-filtered xxh64 of E2).
- Staged, `GC_ERR_NOT_IMPLEMENTED` after validation: `gc_load_game`, `gc_save` (G4), `gc_apply_command` (F2), `gc_query` (E2), `gc_lua_register`.
- Removed with v0: the replica mode, `gc_advance`, `gc_snapshot`, `gc_take_delta`, `gc_load_snapshot`, `gc_apply_delta`.
- `gc_buf` is filled by its producer (`release` set); the receiver calls `gc_buf_release` once.

## Edits to gamecore sources

All guarded by `LEKMOD_PORTABLE` unless they are MSVC-neutral:

- Whitespace only: `> >` instead of `>>` in nested template arguments (C++03), in CvBuildingClasses,
  CvCity, CvInfos, CvTraitClasses (.h/.cpp).
- `CvRandom.h/.cpp`: state words are `CvRandomWord` (32-bit `unsigned int` portable, `unsigned long` on MSVC).
- `CvMilitaryAI.cpp`: the target score keeps 32-bit wraparound (`unsigned int`).
- `CvCity.cpp`: `getRealPopulation` computes in 32-bit `int`.
- `CvPlayer.h`: a bare `#if !defined` (true for MSVC) spelled `#if defined(LEKMOD_MERCHANT_BUYOUT_NOT_NOANNEXING)`.
- `CvPreGame.h`: the extern declaration of `teamType` is dropped (the header's static definition is used).
- `CvCultureClasses.cpp`, `CvGame.cpp`, `CvPreGame.cpp`: `erase(const_iterator)` via `lekmod_portable::mutable_iterator`.
- `CvUnitClasses.cpp`: `GetUnitNames` returns `c_str()` (the CvString/NULL conditional is ambiguous).
- `CvInfosSerializationHelper.cpp`, `CvPromotionClasses.cpp`: FString passed as `GetCString()` to `Format`.
- `CvAStar.cpp`: explicit base-constructor call spelled `this->CvAStar::CvAStar()` (the AUI_WARNING_FIXES form).
- `CvDatabaseUtility.h`: parameter `default` renamed `tDefault`.
- `CvStartPositioner.h`: comparison operators made `const` (libc++ sort).
- Firaxis headers (part of the rewritten libraries): `FFireTypes.h` (`dword` 32 bit, `socklen_t`), `FDataStream.h`
  (`long` overloads, temporaries of `ArrayWrapper`), `FFastList.h`/`FFastAllocator.h` (typedef-named
  constructors/friends, a default naming a missing parameter), `FTimer.h` (no x86 asm), `FLuaCommon.h`
  (ErrorHandler stores static handlers apart from member pointers), `FLuaTypeExposure.h` (`<##CLASS##>`),
  `FDebugHelper.h`, `FInputDevice.h` (map allocator type), `CvLocalization.h` (`std::string` overloads),
  `CvLuaMethodWrapper.h` (forwarders for inherited member pointers).
- `ThirdPartyLibs/Lua51/include/luaconf.h`: `LUA_INTEGER` is `LEKMOD_LUA_INTEGER` when defined.

## Bitness and determinism risks

- `long` is 64 bit on Linux x86_64 and 32 bit on WASM and Win32. Fixed where it changes results
  (CvRandom, CvMilitaryAI, getRealPopulation); `FDataStream` writes `long`/`unsigned long`/`size_t` as 4 bytes.
  Remaining `long` uses are display or zero-initialized flags.
- `CvLuaArgTemplates.h` casts `lua_Integer` to pointer types (same nonsense on Win32; Lua scripts pass objects).
- `FBatchAllocate.h` computes alignment from a pointer truncated to 32 bits (unused by the gamecore).
- `wchar_t` is 4 bytes outside Win32: wide strings are converted to UTF-16 in streams and to UTF-8 for sqlite.
- Floating point: x86_64 SSE2 and WASM are IEEE with per-operation rounding; the Win32 build is x87 `/fp:strict`
  with 53-bit precision, so float expressions may round differently from Windows (Linux and WASM agree with
  each other). `-ffp-contract=off -fno-fast-math` keep clang from fusing or reassociating.
- Hash containers are ordered trees on every target, so iteration order is deterministic across Linux and WASM
  but differs from the MSVC build.
- `std::sort` and friends order equivalent elements differently in libstdc++ and libc++. For server/replica
  agreement build Linux with the same library as WASM (libc++): build.sh uses libc++ on Linux when its headers
  are installed (`/usr/lib/llvm-N/include/c++/v1`), libstdc++ otherwise, and records it in `stdlib:`.
- `CoCreateGuid` is a deterministic sequence (map GUIDs).
- libc++ versions differ (Linux: the host's libc++ 18; WASM: wasi-sdk's libc++ 22). Since libc++ 21, `multimap::find` returns
  whichever equal element the tree descent meets first, not the first one; the gamecore iterates from `find` to `upper_bound`
  (`CvTraitEntry::IsFreePromotionUnitCombat`, `CvPolicyEntry::IsFreePromotionUnitCombat`), so WASM lost trait promotions and
  diverged from Linux. The WASM build defines `_LIBCPP_USE_FROZEN_CXX03_HEADERS` (the gamecore is C++03), so wasi-sdk's frozen
  C++03 headers are used and `find` returns the lower bound there too, as libc++ 18 and MSVC do.
