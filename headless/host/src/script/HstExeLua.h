//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstExeLua.h
//!  \brief     Globals the exe provides to Lua (design 3.2/3.3): sandbox, include, print, DB, GameInfo,
//!				GameDefines, PreGame, Locale, GameEvents/Events/LuaEvents, auto-stubbed UI tables.
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#pragma once
#ifndef HSTEXELUA_H
#define HSTEXELUA_H

#include "HstCommon.h"

class HstScriptSystem;
class HstEngineUtility;
class HstUserInterface;

//! Engine and UI the Lua Network.* / UI.* bindings call into (set by HstSession after I1-I8).
extern HstEngineUtility* g_pkHstLuaEngine;
extern HstUserInterface* g_pkHstLuaUI;

//! Installs everything into the master state L. Creates registry["HstGameEventListeners"].
bool HstExeLuaRegister(HstScriptSystem* pkScriptSystem, lua_State* L);
//! Number of distinct auto-stub globals touched (Global.Name), for the run summary.
int HstExeLuaGetStubCount();
//! New game in the same process (warm start): math.random LCG and the virtual os.clock back to their start values.
void HstExeLuaResetGameState();

#endif // HSTEXELUA_H
