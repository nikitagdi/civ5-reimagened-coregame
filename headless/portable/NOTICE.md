# Notices for headless/portable

## Firaxis library interfaces

The upstream Lekmod tree links against closed Firaxis libraries (CvGameDatabase, CvLocalization, FireWorks, Lua glue and others) that it
ships only as Win32 `.lib` files. The files under `headless/portable/firaxis` are reimplementations of the interfaces of those libraries
for the portable (clang) build. They are written for this project against the public headers in `LEKMOD_DLL/CvGameCoreDLL_Expansion2`
and the behaviour of the shipped game, studied by reverse engineering for interoperability; they do not copy Firaxis library code.
`firaxis/CvWorldBuilderMap.cpp` and `firaxis/CvWorldBuilderMapLoad.cpp` read the WorldBuilder `.Civ5Map` file format; their structure
follows the game's own map loader as reconstructed from the shipped binary.

The Firaxis SDK headers and sources in `LEKMOD_DLL` (the `Cv*` classes, `FirePlace`, `ThirdPartyLibs`) are copyright Take-Two Interactive
Software, Inc. / Firaxis Games and come from the upstream Lekmod repository; see `NOTICE-REIMAGENED.md` in the repository root.

## Third-party code

- `third_party/sqlite/sqlite3.h`: SQLite 3.46.0 public header (compile-time only). SQLite is in the public domain; see
  <https://www.sqlite.org/copyright.html>. The consumer of `libgamecore.a` links SQLite itself.
- Lua 5.1.5 headers in `LEKMOD_DLL/CvGameCoreDLL_Expansion2/ThirdPartyLibs/Lua51` come from upstream (MIT License, PUC-Rio).
- `include/` holds minimal stand-ins for Windows / MSVC headers (`windows.h`, `intrin.h`, ...) so that the gamecore compiles outside
  Windows. They declare only what the gamecore uses.

## Trademarks

Unofficial fan project, not affiliated with or endorsed by Firaxis Games, 2K, Aspyr or Take-Two Interactive.
Sid Meier's Civilization V, Civ, Civilization, 2K Games, Firaxis Games, Take-Two Interactive Software and their logos are trademarks of
Take-Two Interactive Software, Inc. This repository contains no game assets; you need your own legally obtained copy of the game.
