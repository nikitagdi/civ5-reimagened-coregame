# civ5-reimagened branch of Lekmod

This branch (`reimagened/v34.15`) is based on
[EnormousApplePie/Lekmod](https://github.com/EnormousApplePie/Lekmod) at commit `bf3f4e9d3f9cd0bf4fc989a28680751c10ca7200`
("v34.15 hotfix", 2026-04-19). The upstream history up to that commit is kept unchanged; our work is the commits on top of it.
It was created for the civ5-reimagened project, which uses the Lekmod gamecore as the rules engine of a server. It is not an official
Lekmod release and is not endorsed by the Lekmod authors.

## Permission

EnormousApplePie, the author of Lekmod, gave written permission on 2026-11-05 to publish this fork with our changes and to name it as
we see fit. The permission covers this fork; it is not a license for anyone else to redistribute Lekmod.

## Licensing

- **Upstream files** (everything outside `headless/`) and **our modifications to them** remain under the terms of the upstream Lekmod
  project. At commit `bf3f4e9d` upstream carries no license file. Later upstream versions carry the "LEKMOD CONTRIBUTOR &
  MODIFICATION LICENSE"; read the upstream repository for the current terms before you reuse any of this code.
- **Firaxis SDK code**: `LEKMOD_DLL/CvGameCoreDLL_Expansion2` is derived from the Civilization V gamecore SDK, copyright Take-Two
  Interactive Software, Inc. / Firaxis Games, as distributed by upstream. Third-party libraries in that tree (Lua, and others) keep
  their own licenses.
- **Our own work** under `headless/` is under the MIT License, see [headless/LICENSE](headless/LICENSE). That license does not extend to
  the upstream files, and `headless/portable/third_party/` keeps its own terms, see [headless/portable/NOTICE.md](headless/portable/NOTICE.md).

## What this branch changes in upstream files

Additive, flag-guarded instrumentation, optimizations and determinism fixes (`LEKMOD_HEADLESS_*`, `FIX-` / `OPT-` / `HYG-` markers), and
the changes needed to compile the gamecore with clang outside Windows (`LEKMOD_PORTABLE`).

- `LEKMOD_DLL/CvGameCoreDLL_Expansion2`: 106 existing files modified (among them CvAStar, CvCity, CvGame, CvPlayer, CvUnit, the AI
  classes CvDiplomacyAI / CvMilitaryAI / CvTacticalAI / CvHomelandAI / CvEconomicAI, the Lua wrappers under `Lua/`, the FirePlace
  headers under `FirePlace/include/FireWorks`, `CvLocalization.h`, `CvLuaMethodWrapper.h`, `_Defines.h` and
  `ThirdPartyLibs/Lua51/include/luaconf.h`), and 7 new headers (`HeadlessDiag.h`, `HeadlessFpuGuard.h`, `HeadlessImprovements.h`,
  `HeadlessInlineAccessors.h`, `HeadlessMsgGuard.h`, `HeadlessParPool.h`, `HeadlessSparseLists.h`).
- `LEKMOD/Lua`: `Civilizations/Lekmod_italy.lua`, `Lekmod_global_dummies.lua`, `Lekmod_policies.lua` (synchronization fixes).
- `.gitignore`: entries for build outputs.
- New: everything under `headless/`, see [headless/README.md](headless/README.md).

The root `readme.md` and all other upstream files are untouched.

## Trademarks

Unofficial fan project, not affiliated with or endorsed by Firaxis Games, 2K, Aspyr or Take-Two Interactive.
Sid Meier's Civilization V, Civ, Civilization, 2K Games, Firaxis Games, Take-Two Interactive Software and their logos are trademarks of
Take-Two Interactive Software, Inc. This repository contains no game assets of ours; you need your own legally obtained copy of the game.
(The upstream Lekmod tree itself contains the mod's own art, Lua and XML.)
