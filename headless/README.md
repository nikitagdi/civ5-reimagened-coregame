# headless: the Lekmod gamecore without the game

This directory is the work of the civ5-reimagened project on top of [EnormousApplePie/Lekmod](https://github.com/EnormousApplePie/Lekmod)
(`v34.15 hotfix`, see [../NOTICE-REIMAGENED.md](../NOTICE-REIMAGENED.md)). It lets the Lekmod gamecore
(`LEKMOD_DLL/CvGameCoreDLL_Expansion2`, the C++ rules and AI of the Civilization V mod) run without the Civ5 executable:

| path | what |
|---|---|
| `portable/` | clang / WASM build of the gamecore: `libgamecore.a` plus the narrow C ABI `portable/abi/gamecore_abi.h`. The closed Firaxis libraries the DLL links against are reimplemented in `portable/firaxis`. Used by the civ5-reimagened game server (Linux) and replica (WASM). |
| `host/` | `civhost`, an MSVC 2008 host that loads the original (golden) Win32 gamecore DLL and plays scenarios with it. Used as the oracle for the determinism and equivalence checks. |
| `tools/civsim/` | Python 3.10 (standard library only) orchestration: scenario runner, determinism lab, perf lab, save inspection, agent command dictionary, lockstep peers. See [tools/README.md](tools/README.md). |
| `tools/clang/` | build scripts and self-tests of the portable build (`build.sh`, smoke test, scenario runner `runtime/run.sh`). |
| `improvements/`, `determinism/` | the registry of the gamecore changes (`LEKMOD_HEADLESS_OPT_*` optimizations, `FIX_*` fixes, `HYG_*` hygiene) and the catalog of desync sources. |
| `agent-api/`, `scenarios/`, `corpora/` | JSON command dictionary, scenario and corpus definitions. |

All changes to upstream files are additive instrumentation, optimizations and determinism fixes behind `LEKMOD_HEADLESS_*` defines
(see `LEKMOD_DLL/CvGameCoreDLL_Expansion2/HeadlessImprovements.h`). Without those defines the DLL sources compile as in upstream.

## No game assets

This repository contains no Civilization V or Lekmod game data, no saves and no maps. Everything that needs game data (the civhost
oracle, the portable runtime self-tests with `--game`, the save tools) reads it from **your own legally obtained copy of the game**:
point `CIV5_GAME_DIR` (or the `--game` argument of the tool) at the Civ5 install folder (the portable self-tests take a game-data export
made from that install, `db/gameplay.db`, `db/loc-en_us.db`, `vfs-index.json`), e.g.
`C:\Program Files (x86)\Steam\steamapps\common\Sid Meier's Civilization V`. The portable library build itself (`--mode lib`) needs no game files.

## Build path 1: portable library (Linux, WASM)

Toolchain (as tested): Ubuntu 24.04, clang 18.1.3 (`clang++-18`, `llvm-ar`), GNU make, bash, python3 (3.10+). WASM additionally needs
[wasi-sdk 33](https://github.com/WebAssembly/wasi-sdk) (`WASI_SDK`, required, no default) and Node.js 22 for the smoke run
(`--experimental-wasm-exnref`). Windows users build inside WSL.

```sh
# static library, native (writes <out>/libgamecore.a and <out>/build-info.txt)
headless/tools/clang/build.sh --mode lib --target linux --out build/gamecore-linux

# static library, WASM
WASI_SDK=$HOME/wasi-sdk-33 headless/tools/clang/build.sh --mode lib --target wasm --out build/gamecore-wasm

# optional: library + end-to-end smoke test (needs the Lua 5.1.5 and SQLite amalgamation sources)
LUA_SRC=/path/to/lua-5.1.5/src SQLITE_SRC=/path/to/sqlite-3.53.2 \
  headless/tools/clang/build.sh --mode smoke --target linux --out build/gamecore-linux
```

`LUA_SRC` / `SQLITE_SRC` default to `../civ5-reimagened/third_party/lua-5.1.5/src` and `../civ5-reimagened/third_party/sqlite-3.53.2`
(a checkout of civ5-reimagened next to this repository). Details, outputs and the C ABI: [portable/README.md](portable/README.md).

## Build path 2: Windows gamecore DLL and civhost (MSVC 2008)

Toolchain: Visual Studio 2008 (VC9) with Service Pack 1 and the Windows SDK v6.0A (`C:\Program Files (x86)\Microsoft Visual Studio 9.0`;
set `VCBUILD` to its `vcbuild.exe` if it lives elsewhere), PowerShell, Python 3.10+. The DLL links only the static libraries
that are part of the upstream tree. No installer is provided; upstream's `.github/workflows/ci.yaml` shows how its CI obtains Visual Studio 2008.

```powershell
# gamecore DLL variants -> headless\out\dll\<variant>\CvGameCore_Expansion2.dll (golden = sources as-is)
pwsh headless\tools\build-dll.ps1 -Variant golden
pwsh headless\tools\build-dll.ps1 -Variant optimized -FlagSet release -OutName release

# host -> headless\out\host\Release\civhost.exe
pwsh headless\tools\build-host.ps1
```

Running civhost and the Python tools needs the game data of your own Civ5 install (`CIV5_GAME_DIR`) and a data profile
(`Civ5DebugDatabase.db`, `Localization-Merged.db`) in `headless\out\oracle\cache-*`; see [tools/README.md](tools/README.md).

## Tests

```sh
cd headless/tools && PYTHONPATH=. python -m unittest discover -s tests          # civsim, no game files needed
```

The tests use a Python stand-in for civhost and synthetic saves. The portable self-tests (`tools/clang/runtime/run.sh --scenario ...`)
are listed in `tools/clang/runtime/run.sh`.

## Legal

Unofficial fan project, not affiliated with or endorsed by Firaxis Games, 2K, Aspyr or Take-Two Interactive.
Sid Meier's Civilization V, Civ, Civilization, 2K Games, Firaxis Games, Take-Two Interactive Software and their logos are trademarks of
Take-Two Interactive Software, Inc. This repository contains no game assets; you need your own legally obtained copy of the game.

The files under `headless/` that are our own work are under the MIT License ([LICENSE](LICENSE)). Everything outside `headless/`, and
our modifications to those files, stays under the terms of the upstream Lekmod project; see [../NOTICE-REIMAGENED.md](../NOTICE-REIMAGENED.md)
and [portable/NOTICE.md](portable/NOTICE.md).
