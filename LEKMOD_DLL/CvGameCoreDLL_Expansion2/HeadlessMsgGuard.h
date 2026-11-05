#pragma once
// HYG-0013 (LEKMOD_HEADLESS_HYG_MESSAGES_TO_HUMANS, portable build only; headless/improvements/hyg.json): a UI message branch the golden code runs only for the
// active player. HEADLESS_MSG_TO(eActive, ePlayer) is the golden guard "ePlayer is the active player"; with the flag it also holds for every human ePlayer, so each
// human of a network game gets the lines the active player gets (the server seat is the active player and no human). The branches it guards only call AddMessage and
// friends. HEADLESS_ACTIVE_SET() replaces the golden "GC.getGame().getActivePlayer()" used as a boolean (true for every active player but slot 0).
// Not part of HeadlessImprovements.h: that header is only included in LEKMOD_HEADLESS_BUILD builds, and the golden DLL compiles these files too; without the flag the macros expand to the golden code.
// Included by CvUnitCombat.cpp, CvPlayer.cpp, CvUnit.cpp, CvTeam.cpp, CvCity.cpp and CvPlot.cpp.
#if defined(LEKMOD_HEADLESS_HYG_MESSAGES_TO_HUMANS)
#define HEADLESS_MSG_TO(eActive, ePlayer) (((eActive) == (ePlayer)) || ((ePlayer) >= 0 && (ePlayer) < MAX_PLAYERS && GET_PLAYER((PlayerTypes)(ePlayer)).isHuman()))
#define HEADLESS_ACTIVE_SET() (true)
#else
#define HEADLESS_MSG_TO(eActive, ePlayer) ((eActive) == (ePlayer))
#define HEADLESS_ACTIVE_SET() (GC.getGame().getActivePlayer())
#endif
