/*	-------------------------------------------------------------------------------------------------------
	© 1991-2012 Take-Two Interactive Software and its subsidiaries.  Developed by Firaxis Games.  
	Sid Meier's Civilization V, Civ, Civilization, 2K Games, Firaxis Games, Take-Two Interactive Software 
	and their respective logos are all trademarks of Take-Two interactive Software, Inc.  
	All other marks and trademarks are the property of their respective owners.  
	All rights reserved. 
	------------------------------------------------------------------------------------------------------- */
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		CvLuaSupport.cpp
//!  \brief     Private implementation of the Gamecore Lua framework.
//!
//!		This file includes methods for registering game data w/ Lua.
//!
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#include <CvGameCoreDLLPCH.h>
#include "CvLuaSupport.h"
#include "CvLuaEnums.h"
#include "CvLuaFractal.h"
#include "CvLuaGameInfo.h"
#include "CvLuaMap.h"
#include "CvLuaGame.h"
#include "CvLuaPlayer.h"
#include "CvLuaTeam.h"

//------------------------------------------------------------------------------
// Utility methods
//------------------------------------------------------------------------------
bool luaL_optbool(lua_State* L, int idx, bool bdefault)
{
	if(lua_isnoneornil(L, idx))
	{
		return bdefault;
	}
	else
	{
		return (bool)lua_toboolean(L, idx);
	}
}

//------------------------------------------------------------------------------
void LuaSupport::RegisterScriptData(lua_State* L)
{
	CvLuaEnums::Register(L);
	CvLuaFractal::Register(L);
	CvLuaGameInfo::Register(L);
	CvLuaMap::Register(L);
	CvLuaGame::Register(L);
	CvLuaPlayer::Register(L);
	CvLuaTeam::Register(L);
}

//------------------------------------------------------------------------------
void LuaSupport::DumpCallStack(lua_State* L)
{
	for(int i = 1; i < 10; ++i)
	{
		lua_Debug ar;
		if(lua_getstack(L, i, &ar))
		{
			lua_getinfo(L, "Sl", &ar);
			if(ar.currentline > 0)
			{
				FStringFixedBuffer(szTemp, 512);
				szTemp.Format("%s:%d\n", (ar.source)?ar.source:"", ar.currentline);
				OutputDebugString(szTemp.c_str());
			}
		}
	}
}

#if defined(LEKMOD_HEADLESS_OPT_YIELD_DEFICIENT_MEMO)
// OPT-0042: g_uiHeadlessLuaListenerEpoch moves after every GameEvents hook call that had listeners (Lua code ran and may have
// changed any game state); g_iHeadlessLuaHookDepth counts the hook calls in progress. Memos of pure DLL queries compare both.
// Not game state, never serialised; the hook calls themselves are unchanged.
unsigned int g_uiHeadlessLuaListenerEpoch = 1;
int g_iHeadlessLuaHookDepth = 0;
#define HEADLESS_LUA_HOOK_ENTER()			++g_iHeadlessLuaHookDepth
#define HEADLESS_LUA_HOOK_LEAVE(bResult)	do { --g_iHeadlessLuaHookDepth; if(bResult) ++g_uiHeadlessLuaListenerEpoch; } while(0)
#else
#define HEADLESS_LUA_HOOK_ENTER()
#define HEADLESS_LUA_HOOK_LEAVE(bResult)
#endif

//------------------------------------------------------------------------------
#ifdef AUI_WARNING_FIXES
bool LuaSupport::CallHook(_In_ ICvEngineScriptSystem1* pkScriptSystem, _In_z_ const char* szName, _In_opt_ ICvEngineScriptSystemArgs1* args, bool& value)
#else
bool LuaSupport::CallHook(ICvEngineScriptSystem1* pkScriptSystem, const char* szName, ICvEngineScriptSystemArgs1* args, bool& value)
#endif
{
#if defined(LEKMOD_HEADLESS_DIAG)
	HEADLESS_ZONE(HEADLESS_ZONE_LUA_HOOK);
#endif
	// Must release our lock so that if the main thread has the Lua lock and is waiting for the Game Core lock, we don't freeze
	bool bHadLock = gDLL->HasGameCoreLock();
	if(bHadLock)
		gDLL->ReleaseGameCoreLock();
	HEADLESS_LUA_HOOK_ENTER();	// OPT-0042
	bool bResult = pkScriptSystem->CallHook(szName, args, value);
	HEADLESS_LUA_HOOK_LEAVE(bResult);	// OPT-0042
	if(bHadLock)
		gDLL->GetGameCoreLock();
	return bResult;
}

//------------------------------------------------------------------------------
#ifdef AUI_WARNING_FIXES
bool LuaSupport::CallTestAll(_In_ ICvEngineScriptSystem1* pkScriptSystem, _In_z_ const char* szName, _In_opt_ ICvEngineScriptSystemArgs1* args, bool& value)
#else
bool LuaSupport::CallTestAll(ICvEngineScriptSystem1* pkScriptSystem, const char* szName, ICvEngineScriptSystemArgs1* args, bool& value)
#endif
{
#if defined(LEKMOD_HEADLESS_DIAG)
	HEADLESS_ZONE(HEADLESS_ZONE_LUA_HOOK);
#endif
	// Must release our lock so that if the main thread has the Lua lock and is waiting for the Game Core lock, we don't freeze
	bool bHadLock = gDLL->HasGameCoreLock();
	if(bHadLock)
		gDLL->ReleaseGameCoreLock();
	HEADLESS_LUA_HOOK_ENTER();	// OPT-0042
	bool bResult = pkScriptSystem->CallTestAll(szName, args, value);
	HEADLESS_LUA_HOOK_LEAVE(bResult);	// OPT-0042
	if(bHadLock)
		gDLL->GetGameCoreLock();
	return bResult;
}

//------------------------------------------------------------------------------
#ifdef AUI_WARNING_FIXES
bool LuaSupport::CallTestAny(_In_ ICvEngineScriptSystem1* pkScriptSystem, _In_z_ const char* szName, _In_opt_ ICvEngineScriptSystemArgs1* args, bool& value)
#else
bool LuaSupport::CallTestAny(ICvEngineScriptSystem1* pkScriptSystem, const char* szName, ICvEngineScriptSystemArgs1* args, bool& value)
#endif
{
#if defined(LEKMOD_HEADLESS_DIAG)
	HEADLESS_ZONE(HEADLESS_ZONE_LUA_HOOK);
#endif
	// Must release our lock so that if the main thread has the Lua lock and is waiting for the Game Core lock, we don't freeze
	bool bHadLock = gDLL->HasGameCoreLock();
	if(bHadLock)
		gDLL->ReleaseGameCoreLock();
	HEADLESS_LUA_HOOK_ENTER();	// OPT-0042
	bool bResult = pkScriptSystem->CallTestAny(szName, args, value);
	HEADLESS_LUA_HOOK_LEAVE(bResult);	// OPT-0042
	if(bHadLock)
		gDLL->GetGameCoreLock();
	return bResult;
}

//------------------------------------------------------------------------------
bool LuaSupport::CallAccumulator(_In_ ICvEngineScriptSystem1* pkScriptSystem, _In_z_ const char* szName, _In_opt_ ICvEngineScriptSystemArgs1* args, int& value)
{
#if defined(LEKMOD_HEADLESS_DIAG)
	HEADLESS_ZONE(HEADLESS_ZONE_LUA_HOOK);
#endif
	// Must release our lock so that if the main thread has the Lua lock and is waiting for the Game Core lock, we don't freeze
	bool bHadLock = gDLL->HasGameCoreLock();
	if(bHadLock)
		gDLL->ReleaseGameCoreLock();
	HEADLESS_LUA_HOOK_ENTER();	// OPT-0042
	bool bResult = pkScriptSystem->CallAccumulator(szName, args, value);
	HEADLESS_LUA_HOOK_LEAVE(bResult);	// OPT-0042
	if(bHadLock)
		gDLL->GetGameCoreLock();
	return bResult;
}

//------------------------------------------------------------------------------
bool LuaSupport::CallAccumulator(_In_ ICvEngineScriptSystem1* pkScriptSystem, _In_z_ const char* szName, _In_opt_ ICvEngineScriptSystemArgs1* args, float& value)
{
#if defined(LEKMOD_HEADLESS_DIAG)
	HEADLESS_ZONE(HEADLESS_ZONE_LUA_HOOK);
#endif
	// Must release our lock so that if the main thread has the Lua lock and is waiting for the Game Core lock, we don't freeze
	bool bHadLock = gDLL->HasGameCoreLock();
	if(bHadLock)
		gDLL->ReleaseGameCoreLock();
	HEADLESS_LUA_HOOK_ENTER();	// OPT-0042
	bool bResult = pkScriptSystem->CallAccumulator(szName, args, value);
	HEADLESS_LUA_HOOK_LEAVE(bResult);	// OPT-0042
	if(bHadLock)
		gDLL->GetGameCoreLock();
	return bResult;
}

//------------------------------------------------------------------------------
