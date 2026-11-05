//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstExeLua.cpp
//!  \brief     Exe-provided Lua globals (design 3.2/3.3) and the Lua sandbox.
//!
//!		GameInfo = Database::Scripting::Lua::PushDatabase (exe RVA 0x7ccf0, registered per context by 0x311c80).
//!		DB.Query = PushDatabaseRow over a host-owned Results. Everything UI-side is an auto-stub that logs
//!		"Global.Name" once (fatal under --strict through the Lua error path is NOT applied: stubs are sinks).
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#include "HstCommon.h"
#include "LuaExposures.h"
#include "script/HstExeLua.h"
#include "engine/HstEngineUtility.h"
#include "engine/HstUserInterface.h"
#include "script/HstScriptSystem.h"
#include "script/HstVfs.h"
#include "platform/HstPlatform.h"

static std::set<std::string> s_kStubsSeen;
static unsigned int s_uiRandState = 1;	// MSVCRT rand() LCG state (the C default seed)
static unsigned int s_uiRandCalls = 0;
static double s_dVirtualClock = 0.0;
static const double s_dVirtualEpoch = 1356998400.0;	// 2013-01-01 00:00:00 UTC, fixed

static HstScriptSystem* HstSS(lua_State* L)
{
	return (HstScriptSystem*)lua_touserdata(L, lua_upvalueindex(1));
}

static void HstRegister(lua_State* L, int iTable, const char* szName, lua_CFunction pfn, HstScriptSystem* pkSS)
{
	lua_pushlightuserdata(L, pkSS);
	lua_pushcclosure(L, pfn, 1);
	lua_setfield(L, iTable, szName);
}

// ---- print / include ---------------------------------------------------------------------------------------------
static int lPrint(lua_State* L)
{
	std::string str;
	const int n = lua_gettop(L);
	for(int i = 1; i <= n; ++i)
	{
		if(i > 1)
			str += "\t";
		switch(lua_type(L, i))
		{
		case LUA_TSTRING:
		case LUA_TNUMBER:
			str += lua_tostring(L, i);
			break;
		case LUA_TBOOLEAN:
			str += lua_toboolean(L, i) ? "true" : "false";
			break;
		case LUA_TNIL:
			str += "nil";
			break;
		default:
			str += lua_typename(L, lua_type(L, i));	// no addresses: logs stay comparable across runs
			break;
		}
	}
	HstSS(L)->LogLua(L, str.c_str());
	return 0;
}

//! include(name): stem match through the VFS, executed in the caller's environment.
static int lInclude(lua_State* L)
{
	HstScriptSystem* pkSS = HstSS(L);
	const char* szName = luaL_checkstring(L, 1);
	std::wstring wstrPath;
	if(!pkSS->GetVfs()->ResolveInclude(szName, wstrPath))
	{
		std::string strMsg = std::string("include: file not found: ") + szName;
		HstLog("lua: WARNING %s", strMsg.c_str());
		pkSS->LogLua(L, strMsg.c_str());
		lua_pushboolean(L, 0);
		return 1;
	}
	std::string strSource;
	if(!HstVfs::ReadFile(wstrPath, strSource))
		return luaL_error(L, "include: cannot read %s", szName);
	size_t uiSlash = wstrPath.find_last_of(L"\\/");
	std::string strChunk = "@" + HstToUtf8(uiSlash == std::wstring::npos ? wstrPath : wstrPath.substr(uiSlash + 1));
	if(luaL_loadbuffer(L, strSource.data(), strSource.size(), strChunk.c_str()) != 0)
		return lua_error(L);
	lua_Debug kAr;
	if(lua_getstack(L, 1, &kAr) && lua_getinfo(L, "f", &kAr))
	{
		lua_getfenv(L, -1);
		lua_remove(L, -2);
	}
	else
		lua_pushvalue(L, LUA_GLOBALSINDEX);
	lua_setfenv(L, -2);
	lua_call(L, 0, 0);
	lua_pushboolean(L, 1);
	return 1;
}

// ---- DB.Query ------------------------------------------------------------------------------------------------------
struct HstDbQuery
{
	Database::Results* pkResults;
};

static int lDbQueryGc(lua_State* L)
{
	HstDbQuery* pkQuery = (HstDbQuery*)lua_touserdata(L, 1);
	if(pkQuery && pkQuery->pkResults)
	{
		delete pkQuery->pkResults;
		pkQuery->pkResults = NULL;
	}
	return 0;
}

static int lDbQueryIter(lua_State* L)
{
	HstDbQuery* pkQuery = (HstDbQuery*)lua_touserdata(L, lua_upvalueindex(1));
	if(!pkQuery || !pkQuery->pkResults)
		return 0;
	if(pkQuery->pkResults->Step())
	{
		Database::Scripting::Lua::PushDatabaseRow(L, pkQuery->pkResults);
		return 1;
	}
	delete pkQuery->pkResults;
	pkQuery->pkResults = NULL;
	return 0;
}

static int lDbQuery(lua_State* L)
{
	HstScriptSystem* pkSS = HstSS(L);
	const char* szSql = luaL_checkstring(L, 1);
	const int nParams = lua_gettop(L) - 1;
	HstDbQuery* pkQuery = (HstDbQuery*)lua_newuserdata(L, sizeof(HstDbQuery));
	pkQuery->pkResults = NULL;
	luaL_getmetatable(L, "HstDbQuery");
	lua_setmetatable(L, -2);
	pkQuery->pkResults = new Database::Results();
	if(!pkSS->GetDatabase()->Execute(*pkQuery->pkResults, szSql))
	{
		delete pkQuery->pkResults;
		pkQuery->pkResults = NULL;
		return luaL_error(L, "DB.Query: %s (%s)", pkSS->GetDatabase()->ErrorMessage(), szSql);
	}
	for(int i = 1; i <= nParams; ++i)
	{
		const int iIdx = i + 1;
		switch(lua_type(L, iIdx))
		{
		case LUA_TNUMBER:
		{
			const double d = lua_tonumber(L, iIdx);
			const int iValue = (int)d;
			if((double)iValue == d)
				pkQuery->pkResults->Bind(i, iValue);
			else
				pkQuery->pkResults->Bind(i, d);
			break;
		}
		case LUA_TSTRING:
		{
			size_t uiLen = 0;
			const char* sz = lua_tolstring(L, iIdx, &uiLen);
			pkQuery->pkResults->Bind(i, sz, (int)uiLen, true);
			break;
		}
		case LUA_TBOOLEAN:
			pkQuery->pkResults->Bind(i, lua_toboolean(L, iIdx) ? 1 : 0);
			break;
		default:
			pkQuery->pkResults->BindNULL(i);
			break;
		}
	}
	lua_pushcclosure(L, lDbQueryIter, 1);	// upvalue: the query userdata (keeps the Results alive)
	return 1;
}

// ---- Locale --------------------------------------------------------------------------------------------------------
static int lLocaleConvertTextKey(lua_State* L)
{
	const char* szKey = lua_tostring(L, 1);
	if(!szKey)
	{
		lua_pushstring(L, "");
		return 1;
	}
	Localization::String kString = Localization::Lookup(szKey);
	const int n = lua_gettop(L);
	for(int i = 2; i <= n; ++i)
	{
		switch(lua_type(L, i))
		{
		case LUA_TNUMBER:
		{
			const double d = lua_tonumber(L, i);
			const int iValue = (int)d;
			if((double)iValue == d)
				kString.PushArgument(iValue);
			else
				kString.PushArgument(d);
			break;
		}
		case LUA_TSTRING:
		{
			size_t uiLen = 0;
			const char* sz = lua_tolstring(L, i, &uiLen);
			kString.PushArgument(sz, uiLen);
			break;
		}
		case LUA_TBOOLEAN:
			kString.PushArgument(lua_toboolean(L, i) ? 1 : 0);
			break;
		default:
			kString.PushArgument("");
			break;
		}
	}
	const char* szOut = kString.toUTF8();
	lua_pushstring(L, szOut ? szOut : "");
	return 1;
}

static int lLocaleHasTextKey(lua_State* L)
{
	size_t uiLen = 0;
	const char* szKey = lua_tolstring(L, 1, &uiLen);
	lua_pushboolean(L, szKey && Localization::GetStringDictionary().HasKey(szKey, (unsigned int)uiLen));
	return 1;
}

static int lLocaleCase(lua_State* L, bool bUpper)
{
	size_t uiLen = 0;
	const char* sz = luaL_checklstring(L, 1, &uiLen);
	std::string str(sz, uiLen);
	for(size_t i = 0; i < str.size(); ++i)
	{
		unsigned char c = (unsigned char)str[i];
		if(c < 0x80)
			str[i] = (char)(bUpper ? toupper(c) : tolower(c));
	}
	lua_pushlstring(L, str.data(), str.size());
	return 1;
}
static int lLocaleToUpper(lua_State* L) { return lLocaleCase(L, true); }
static int lLocaleToLower(lua_State* L) { return lLocaleCase(L, false); }

// ---- PreGame (thin wrappers over ICvPreGame1, added on demand) ---------------------------------------------------------
#define HST_PREGAME() std::auto_ptr<ICvPreGame1> pkPreGame(HstSS(L)->GetContext()->GetPreGame())
#define HST_PREGAME_INT(fn, expr) static int fn(lua_State* L) { HST_PREGAME(); lua_pushinteger(L, (int)(expr)); return 1; }
#define HST_PREGAME_BOOL(fn, expr) static int fn(lua_State* L) { HST_PREGAME(); lua_pushboolean(L, (expr) ? 1 : 0); return 1; }
#define HST_P ((PlayerTypes)luaL_checkint(L, 1))

HST_PREGAME_INT(lPreGameGetWorldSize, pkPreGame->worldSize())
HST_PREGAME_INT(lPreGameGetEra, pkPreGame->era())
HST_PREGAME_INT(lPreGameGetGameSpeed, pkPreGame->gameSpeed())
HST_PREGAME_INT(lPreGameGetClimate, pkPreGame->climate())
HST_PREGAME_INT(lPreGameGetSeaLevel, pkPreGame->seaLevel())
HST_PREGAME_INT(lPreGameGetNumMinorCivs, pkPreGame->numMinorCivs())
HST_PREGAME_INT(lPreGameGetGameType, pkPreGame->gameType())
HST_PREGAME_INT(lPreGameGetMaxTurns, pkPreGame->maxTurns())
HST_PREGAME_INT(lPreGameGetSlotStatus, pkPreGame->slotStatus(HST_P))
HST_PREGAME_INT(lPreGameGetSlotClaim, pkPreGame->slotClaim(HST_P))
HST_PREGAME_INT(lPreGameGetCivilization, pkPreGame->civilization(HST_P))
HST_PREGAME_INT(lPreGameGetLeaderHead, pkPreGame->leaderHead(HST_P))
HST_PREGAME_INT(lPreGameGetTeam, pkPreGame->teamType(HST_P))
HST_PREGAME_INT(lPreGameGetHandicap, pkPreGame->handicap(HST_P))
HST_PREGAME_INT(lPreGameGetMinorCivType, pkPreGame->minorCivType(HST_P))
HST_PREGAME_BOOL(lPreGameIsMinorCiv, pkPreGame->isMinorCiv(HST_P))
HST_PREGAME_BOOL(lPreGameIsHuman, pkPreGame->isHuman(HST_P))
HST_PREGAME_BOOL(lPreGameIsMultiplayerGame, pkPreGame->isNetworkMultiplayerGame())
HST_PREGAME_BOOL(lPreGameIsHotSeatGame, pkPreGame->isHotSeatGame())
HST_PREGAME_BOOL(lPreGameIsInternetGame, pkPreGame->isInternetGame())
HST_PREGAME_BOOL(lPreGameGetLoadWBScenario, pkPreGame->loadWBScenario())
HST_PREGAME_BOOL(lPreGameIsVictory, pkPreGame->isVictory((VictoryTypes)luaL_checkint(L, 1)))

static int lPreGameGetMapScript(lua_State* L)
{
	HST_PREGAME();
	CvString str = pkPreGame->mapScriptName();
	lua_pushstring(L, str.c_str());
	return 1;
}

static int lPreGameGetMapOption(lua_State* L)
{
	HST_PREGAME();
	int iValue = 0;
	if(lua_type(L, 1) == LUA_TNUMBER)
	{
		char szName[32];
		_snprintf_s(szName, sizeof(szName), _TRUNCATE, "%d", (int)lua_tointeger(L, 1));
		if(!pkPreGame->GetMapOption(szName, iValue))
			return 0;
	}
	else if(!pkPreGame->GetMapOption(luaL_checkstring(L, 1), iValue))
		return 0;
	lua_pushinteger(L, iValue);
	return 1;
}

static int lPreGameGetGameOption(lua_State* L)
{
	HST_PREGAME();
	int iValue = -1;
	bool bFound = false;
	if(lua_type(L, 1) == LUA_TNUMBER)
		bFound = pkPreGame->GetGameOption((GameOptionTypes)lua_tointeger(L, 1), iValue);
	else
		bFound = pkPreGame->GetGameOption(luaL_checkstring(L, 1), iValue);
	lua_pushinteger(L, bFound ? iValue : -1);
	return 1;
}

// ---- sandbox helpers -----------------------------------------------------------------------------------------------
static int lStubCalled(lua_State* L)
{
	std::string strKey = std::string(luaL_optstring(L, 1, "?")) + "." + luaL_optstring(L, 2, "?");
	if(s_kStubsSeen.insert(strKey).second)
	{
		HstLog("lua: auto-stub %s", strKey.c_str());
		std::string strMsg = "[civhost] auto-stub " + strKey;
		HstSS(L)->LogLua(L, strMsg.c_str());
	}
	return 0;
}

static int lVirtualClock(lua_State* L)
{
	s_dVirtualClock += 0.001;
	lua_pushnumber(L, s_dVirtualClock);
	return 1;
}

static int lVirtualTime(lua_State* L)
{
	if(lua_istable(L, 1))
		return luaL_error(L, "os.time(table) is not supported in civhost");
	lua_pushnumber(L, s_dVirtualEpoch);
	return 1;
}

//! MSVCRT rand() sequence with a host-owned state (deterministic; every use is counted and the first logged).
static int lRandom(lua_State* L)
{
	if(s_uiRandCalls++ == 0)
		HstLog("lua: math.random used (deterministic host LCG)");
	s_uiRandState = s_uiRandState * 214013u + 2531011u;
	const int iRand = (int)((s_uiRandState >> 16) & 0x7fff);
	const lua_Number r = (lua_Number)(iRand % 32767) / (lua_Number)32767;	// lmathlib: (rand()%RAND_MAX)/RAND_MAX
	switch(lua_gettop(L))
	{
	case 0:
		lua_pushnumber(L, r);
		break;
	case 1:
	{
		int u = luaL_checkint(L, 1);
		luaL_argcheck(L, 1 <= u, 1, "interval is empty");
		lua_pushnumber(L, floor(r * u) + 1);
		break;
	}
	case 2:
	{
		int l = luaL_checkint(L, 1);
		int u = luaL_checkint(L, 2);
		luaL_argcheck(L, l <= u, 2, "interval is empty");
		lua_pushnumber(L, floor(r * (u - l + 1)) + l);
		break;
	}
	default:
		return luaL_error(L, "wrong number of arguments");
	}
	return 1;
}

static int lRandomSeed(lua_State* L)
{
	HstLog("lua: math.randomseed(%d)", luaL_checkint(L, 1));
	s_uiRandState = (unsigned int)luaL_checkint(L, 1);
	return 0;
}

// ---- library extensions the exe registers (strings next to the table/math lib names in CivilizationV_DX11.exe) ----
//! table.fill(value, n) -> new { value x n }; table.fill(t, value, n) fills t[1..n] in place and returns t.
//! 1-based (HBAssignStartingPlots comments: index 0 of a filled table is nil).
static int lTableFill(lua_State* L)
{
	int iValue = 1;
	int n = 0;
	if(lua_gettop(L) >= 3 && lua_istable(L, 1))
	{
		iValue = 2;
		n = luaL_checkint(L, 3);
		lua_pushvalue(L, 1);
	}
	else
	{
		luaL_checkany(L, 1);
		n = luaL_checkint(L, 2);
		lua_createtable(L, n > 0 ? n : 0, 0);
	}
	for(int i = 1; i <= n; ++i)
	{
		lua_pushvalue(L, iValue);
		lua_rawseti(L, -2, i);
	}
	return 1;
}

//! table.count(t) -> number of entries (pairs).
static int lTableCount(lua_State* L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	int n = 0;
	lua_pushnil(L);
	while(lua_next(L, 1) != 0)
	{
		++n;
		lua_pop(L, 1);
	}
	lua_pushinteger(L, n);
	return 1;
}

//! math.clamp(x, lo, hi)
static int lMathClamp(lua_State* L)
{
	lua_Number x = luaL_checknumber(L, 1);
	const lua_Number lo = luaL_checknumber(L, 2);
	const lua_Number hi = luaL_checknumber(L, 3);
	if(x < lo)
		x = lo;
	if(x > hi)
		x = hi;
	lua_pushnumber(L, x);
	return 1;
}

int HstExeLuaGetStubCount()
{
	return (int)s_kStubsSeen.size();
}

void HstExeLuaResetGameState()
{
	s_uiRandState = 1;
	s_uiRandCalls = 0;
	s_dVirtualClock = 0.0;
}

// ---- Modding.OpenSaveData (design 3.3: SimpleValues in <cache>Civ5SavedGameDatabase.db) --------------------------
//! The save-data object: GetValue/SetValue accept both call styles (Lekmod_bolivia.lua uses "Lek_SaveData.GetValue(name)",
//! most mods use "data:GetValue(name)"): a leading table argument is skipped.
static int HstSaveDataArgBase(lua_State* L)
{
	return lua_istable(L, 1) ? 2 : 1;
}

static int lSaveDataGetValue(lua_State* L)
{
	HstScriptSystem* pkSS = HstSS(L);
	const int iBase = HstSaveDataArgBase(L);
	const char* szName = luaL_checkstring(L, iBase);
	Database::Connection* pkDb = pkSS->GetSaveData();
	if(!pkDb)
		return 0;
	Database::Results kResults;
	if(!pkDb->Execute(kResults, "SELECT Value FROM SimpleValues WHERE Name = ? LIMIT 1"))
		return luaL_error(L, "SaveData.GetValue: %s", pkDb->ErrorMessage());
	kResults.Bind(1, szName);
	if(!kResults.Step())
		return 0;
	switch(kResults.ColumnType(0))
	{
	case Database::COLTYPE_INT:
		lua_pushnumber(L, (lua_Number)kResults.GetInt64(0));
		return 1;
	case Database::COLTYPE_FLOAT:
		lua_pushnumber(L, kResults.GetDouble(0));
		return 1;
	case Database::COLTYPE_TEXT:
		lua_pushstring(L, kResults.GetText(0));
		return 1;
	default:
		return 0;
	}
}

static int lSaveDataSetValue(lua_State* L)
{
	HstScriptSystem* pkSS = HstSS(L);
	const int iBase = HstSaveDataArgBase(L);
	const char* szName = luaL_checkstring(L, iBase);
	Database::Connection* pkDb = pkSS->GetSaveData();
	if(!pkDb)
		return 0;
	Database::Results kResults;
	if(!pkDb->Execute(kResults, "INSERT OR REPLACE INTO SimpleValues(Name, Value) VALUES(?, ?)"))
		return luaL_error(L, "SaveData.SetValue: %s", pkDb->ErrorMessage());
	kResults.Bind(1, szName);
	const int iValue = iBase + 1;
	switch(lua_type(L, iValue))
	{
	case LUA_TNUMBER:
	{
		const double d = lua_tonumber(L, iValue);
		const int i = (int)d;
		if((double)i == d)
			kResults.Bind(2, i);
		else
			kResults.Bind(2, d);
		break;
	}
	case LUA_TBOOLEAN:
		kResults.Bind(2, lua_toboolean(L, iValue) ? 1 : 0);
		break;
	case LUA_TSTRING:
	{
		size_t uiLen = 0;
		const char* sz = lua_tolstring(L, iValue, &uiLen);
		kResults.Bind(2, sz, (int)uiLen, true);
		break;
	}
	default:
		kResults.BindNULL(2);
		break;
	}
	if(!kResults.Execute())
		return luaL_error(L, "SaveData.SetValue(%s): %s", szName, pkDb->ErrorMessage());
	return 0;
}

static int lModdingOpenSaveData(lua_State* L)
{
	HstScriptSystem* pkSS = HstSS(L);
	lua_getfield(L, LUA_REGISTRYINDEX, "HstSaveDataObject");
	if(lua_istable(L, -1))
		return 1;
	lua_pop(L, 1);
	lua_newtable(L);
	const int t = lua_gettop(L);
	HstRegister(L, t, "GetValue", lSaveDataGetValue, pkSS);
	HstRegister(L, t, "SetValue", lSaveDataSetValue, pkSS);
	lua_pushvalue(L, t);
	lua_setfield(L, LUA_REGISTRYINDEX, "HstSaveDataObject");
	return 1;
}

// ---- contexts / errors (host side lives in HstScriptSystem) -----------------------------------------------------------
//! HstLoadContext(name) -> the new context's ContextPtr, or nil (ContextPtr:LoadNewContext).
static int lLoadContext(lua_State* L)
{
	HstScriptSystem* pkSS = HstSS(L);
	const char* szName = luaL_checkstring(L, 1);
	lua_State* T = pkSS->LoadContext(szName);
	if(!T)
	{
		lua_pushnil(L);
		return 1;
	}
	lua_getfield(T, LUA_GLOBALSINDEX, "ContextPtr");
	lua_xmove(T, L, 1);
	return 1;
}

//! HstReportError(where, message): a listener error caught by the Lua-side event dispatch.
static int lReportError(lua_State* L)
{
	HstSS(L)->ReportError(luaL_optstring(L, 1, "?"), luaL_optstring(L, 2, "?"));
	return 0;
}

// ---- hex helpers (exe globals; pure math, used for popup positions) ---------------------------------------------------
//! ToHexFromGrid(Vector2/{x,y}) -> {x = gx - floor(gy / 2), y = gy} (Civ5 offset -> axial).
static int lToHexFromGrid(lua_State* L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	lua_getfield(L, 1, "x");
	lua_getfield(L, 1, "y");
	const int x = (int)lua_tonumber(L, -2);
	const int y = (int)lua_tonumber(L, -1);
	lua_newtable(L);
	lua_pushinteger(L, x - (y >= 0 ? y / 2 : -((-y + 1) / 2)));
	lua_setfield(L, -2, "x");
	lua_pushinteger(L, y);
	lua_setfield(L, -2, "y");
	return 1;
}

static int lToGridFromHex(lua_State* L)
{
	const int x = luaL_checkint(L, 1);
	const int y = luaL_checkint(L, 2);
	lua_pushinteger(L, x + (y >= 0 ? y / 2 : -((-y + 1) / 2)));
	lua_pushinteger(L, y);
	return 2;
}

//! HexToWorld(hex) -> {x, y, z}; the world position only feeds UI calls (popup text), so any finite value is fine.
static int lHexToWorld(lua_State* L)
{
	double x = 0.0, y = 0.0;
	if(lua_istable(L, 1))
	{
		lua_getfield(L, 1, "x");
		lua_getfield(L, 1, "y");
		x = lua_tonumber(L, -2);
		y = lua_tonumber(L, -1);
		lua_pop(L, 2);
	}
	lua_newtable(L);
	lua_pushnumber(L, x * 64.0 + y * 32.0);
	lua_setfield(L, -2, "x");
	lua_pushnumber(L, y * 55.4256);
	lua_setfield(L, -2, "y");
	lua_pushnumber(L, 0.0);
	lua_setfield(L, -2, "z");
	return 1;
}

// ---- Network.* (exe Lua bindings, registered by the exe at 0x76c000..0x76f000) -------------------------------------------
// Each exe binding reads its arguments with lua_tointeger / lua_toboolean (luaL_checkinteger where noted) and calls the
// engine utility object (Sync_g_pApp+0x1460, the gDLL the DLL sees) at the vtable slot of the same-named send*; the RVA of
// every binding and its slot are in the host's bot bridge. So the bridge calls HstEngineUtility directly,
// which queues the same message a gDLL->send* from the DLL queues. Two bindings call exe-only vfuncs (+0x410, +0x414):
// they become the exe-only messages NetArchaeologyChoice / NetIdeologyChoice (ResponseArchaeologyChoice/IdeologyChoice).
HstEngineUtility* g_pkHstLuaEngine = NULL;
HstUserInterface* g_pkHstLuaUI = NULL;

static int Ni(lua_State* L, int i) { return (int)lua_tointeger(L, i); }
static int Nc(lua_State* L, int i) { return (int)luaL_checkinteger(L, i); }
static bool Nb(lua_State* L, int i) { return lua_toboolean(L, i) != 0; }
#define HST_NET(name, body) static int lNet##name(lua_State* L) { HstEngineUtility* e = g_pkHstLuaEngine; if(!e) return 0; body; return 0; }

HST_NET(SendResearch, e->sendResearch((TechTypes)Ni(L, 1), Ni(L, 2), (PlayerTypes)Ni(L, 3), Nb(L, 4)))					// 0x2e2f80 +0xb8
HST_NET(SendUpdatePolicies, e->sendUpdatePolicies(Nb(L, 2), Ni(L, 1), Nb(L, 3)))										// 0x148fe0 +0xdc (id, isPolicy, value)
HST_NET(SendDoTask, e->sendDoTask(Ni(L, 1), (TaskTypes)Ni(L, 2), Ni(L, 3), Ni(L, 4), Nb(L, 5), Nb(L, 6), Nb(L, 7), Nb(L, 8)))	// 0x22e1e0 +0xb4
HST_NET(SendGoodyChoice, e->sendGoodyChoice((PlayerTypes)Ni(L, 1), Ni(L, 2), Ni(L, 3), (GoodyTypes)Ni(L, 4), Ni(L, 5)))	// 0x2bb440 +0x174
HST_NET(SendIdeologyChoice, e->SendExeMessage(kHstNet_IdeologyChoice, (PlayerTypes)Ni(L, 1), Ni(L, 2), 0))				// 0x1de7d0 +0x414
HST_NET(SendArchaeologyChoice, e->SendExeMessage(kHstNet_ArchaeologyChoice, (PlayerTypes)Ni(L, 1), Ni(L, 2), Ni(L, 3)))	// 0x227c00 +0x410
HST_NET(SendLeagueVoteEnact, e->SendLeagueVoteEnact((LeagueTypes)Nc(L, 1), Nc(L, 2), (PlayerTypes)Nc(L, 3), Nc(L, 4), Nc(L, 5)))	// 0x1fcda0 +0x3b0
HST_NET(SendLeagueVoteRepeal, e->SendLeagueVoteRepeal((LeagueTypes)Nc(L, 1), Nc(L, 2), (PlayerTypes)Nc(L, 3), Nc(L, 4), Nc(L, 5)))	// 0x18d360 (GUESS: as Enact)
HST_NET(SendLeagueProposeEnact, e->SendLeagueProposeEnact((LeagueTypes)Nc(L, 1), (ResolutionTypes)Nc(L, 2), (PlayerTypes)Nc(L, 3), Nc(L, 4)))	// 0x13b7f0 +0x3bc
HST_NET(SendLeagueProposeRepeal, e->SendLeagueProposeRepeal((LeagueTypes)Nc(L, 1), Nc(L, 2), (PlayerTypes)Nc(L, 3)))	// 0x325030 (GUESS: signature order)
HST_NET(SendLeagueVoteAbstain, e->SendLeagueVoteAbstain((LeagueTypes)Nc(L, 1), (PlayerTypes)Nc(L, 2), Nc(L, 3)))		// 0x1890f0 +0x3b8
HST_NET(SendMoveSpy, e->SendMoveSpy((PlayerTypes)Nc(L, 1), Nc(L, 2), Nc(L, 3), Nc(L, 4), Nb(L, 5)))					// 0x6d630 +0x6c
HST_NET(SendStageCoup, e->SendStageCoup((PlayerTypes)Nc(L, 1), Nc(L, 2)))												// 0x1d480 +0x70
HST_NET(SendFoundPantheon, e->SendFoundPantheon((PlayerTypes)Nc(L, 1), (BeliefTypes)Nc(L, 2)))							// 0x39e7a0 +0x60
HST_NET(SendFoundReligion, e->SendFoundReligion((PlayerTypes)Nc(L, 1), (ReligionTypes)Nc(L, 2), lua_tostring(L, 3), (BeliefTypes)Nc(L, 4),
	(BeliefTypes)Nc(L, 5), (BeliefTypes)Nc(L, 6), (BeliefTypes)Nc(L, 7), Nc(L, 8), Nc(L, 9)))							// 0xbc020 +0x64
HST_NET(SendEnhanceReligion, e->SendEnhanceReligion((PlayerTypes)Nc(L, 1), (ReligionTypes)Nc(L, 2), lua_tostring(L, 3), (BeliefTypes)Nc(L, 4),
	(BeliefTypes)Nc(L, 5), Nc(L, 6), Nc(L, 7)))																			// 0x64da0 +0x68
HST_NET(SendFaithPurchase, e->SendFaithPurchase((PlayerTypes)Nc(L, 1), (FaithPurchaseTypes)Nc(L, 2), Nc(L, 3)))			// 0x20da40 +0x74
HST_NET(SendChangeWar, e->sendChangeWar((TeamTypes)Ni(L, 1), Nb(L, 2)))												// 0x92d60 +0xc8
HST_NET(SendIgnoreWarning, e->sendIgnoreWarning((TeamTypes)Ni(L, 1)))													// 0x212d50 +0xcc
HST_NET(SendPledgeMinorProtection, e->SendPledgeMinorProtection((PlayerTypes)Ni(L, 1), Nb(L, 2)))						// 0x2b5950 +0xd0
HST_NET(SendMinorNoUnitSpawning, e->SendMinorNoUnitSpawning((PlayerTypes)Ni(L, 1), Nb(L, 2)))							// 0x1125d0 +0xd4
HST_NET(SendLiberateMinor, e->SendLiberateMinor((PlayerTypes)Ni(L, 1), Ni(L, 2)))										// 0xc9620 +0xd8
HST_NET(SendDiploVote, e->SendDiploVote((PlayerTypes)Ni(L, 1)))															// 0x2a2a60 +0xe0
HST_NET(SendBarbarianRansom, e->sendBarbarianRansom(Ni(L, 1), Ni(L, 2)))												// 0x24fa50 +0xfc
HST_NET(SendGiftUnit, e->sendGiftUnit((PlayerTypes)Ni(L, 1), Ni(L, 2)))													// 0x28bcf0 +0x100
HST_NET(SendReturnCivilian, e->sendReturnCivilian(Nb(L, 1), (PlayerTypes)Ni(L, 2), Ni(L, 3)))							// 0x350390 +0x104
HST_NET(SendUpdateCityCitizens, e->SendUpdateCityCitizens(Ni(L, 1)))													// 0x120610 +0x108
HST_NET(SendSellBuilding, e->SendSellBuilding(Ni(L, 1), (BuildingTypes)Ni(L, 2)))										// 0xcf2d0 +0x10c
HST_NET(SendCityBuyPlot, e->sendCityBuyPlot(Ni(L, 1), Ni(L, 2), Ni(L, 3)))												// 0x38d410 +0x120
HST_NET(SendGreatPersonChoice, e->sendGreatPersonChoice((PlayerTypes)Ni(L, 1), (UnitTypes)Ni(L, 2)))					// 0x2ff870 +0x13c
HST_NET(SendMayaBonusChoice, e->sendMayaBonusChoice((PlayerTypes)Ni(L, 1), (UnitTypes)Ni(L, 2)))						// 0x3377e0 +0x140
HST_NET(SendFaithGreatPersonChoice, e->sendFaithGreatPersonChoice((PlayerTypes)Ni(L, 1), (UnitTypes)Ni(L, 2)))			// 0xd3ad0 +0x144
HST_NET(SendSetCityAIFocus, e->sendSetCityAIFocus(Ni(L, 1), (CityAIFocusTypes)Ni(L, 2)))								// 0x701b0 +0x148
HST_NET(SendSetCityAvoidGrowth, e->sendSetCityAvoidGrowth(Ni(L, 1), Nb(L, 2)))											// 0xd72e0 +0x14c
HST_NET(SendChangeIdeology, e->sendChangeIdeology())																	// 0x360870 +0x3c8
HST_NET(SendChat, (void)0)
static int lNetSendTurnUnready(lua_State* L) { lua_pushboolean(L, g_pkHstLuaEngine && g_pkHstLuaEngine->sendTurnUnready()); return 1; }	// 0xc1170 +0x94
static int lNetHasSentNetTurnComplete(lua_State* L) { lua_pushboolean(L, g_pkHstLuaEngine && g_pkHstLuaEngine->HasSentTurnComplete()); return 1; }	// 0x69900 +0x80
static int lNetHasSentNetTurnAllComplete(lua_State* L) { lua_pushboolean(L, g_pkHstLuaEngine && g_pkHstLuaEngine->HasSentTurnAllComplete()); return 1; }

// UI selection / popups (bot side; exe UI.SelectUnit etc. resolve to the engine UI's selection list)
static int lUiSelectUnit(lua_State* L) { if(g_pkHstLuaUI) g_pkHstLuaUI->HostSelectUnit(Ni(L, 1), lua_isnumber(L, 2) ? Ni(L, 2) : -1); return 0; }
static int lUiSelectCity(lua_State* L) { if(g_pkHstLuaUI) g_pkHstLuaUI->HostSelectCity(Ni(L, 1), lua_isnumber(L, 2) ? Ni(L, 2) : -1); return 0; }
static int lUiClearSelection(lua_State* L) { if(g_pkHstLuaUI) g_pkHstLuaUI->HostClearSelection(); return 0; }
static int lUiTakePopups(lua_State* L)
{
	std::vector<HstUserInterface::Popup> akPopups;
	if(g_pkHstLuaUI)
		g_pkHstLuaUI->TakePopups(akPopups);
	lua_newtable(L);
	for(size_t i = 0; i < akPopups.size(); ++i)
	{
		const HstUserInterface::Popup& k = akPopups[i];
		lua_newtable(L);
		lua_pushinteger(L, k.iType); lua_setfield(L, -2, "Type");
		lua_pushinteger(L, k.iData1); lua_setfield(L, -2, "Data1");
		lua_pushinteger(L, k.iData2); lua_setfield(L, -2, "Data2");
		lua_pushinteger(L, k.iData3); lua_setfield(L, -2, "Data3");
		lua_pushinteger(L, k.iFlags); lua_setfield(L, -2, "Flags");
		lua_pushboolean(L, k.bOption1); lua_setfield(L, -2, "Option1");
		lua_pushboolean(L, k.bOption2); lua_setfield(L, -2, "Option2");
		lua_rawseti(L, -2, (int)i + 1);
	}
	return 1;
}
// Trade screen (bot mode): the exe UI.* deal functions the Lua trade screen (TradeLogic.lua) calls. They work on the
// engine UI's scratch deal (HstUserInterface::HostScratchDeal) and call the same DLL entries as the exe: the deal AI of the
// deal's other player (ICvDealAI1). All of it runs on the bot's peer only, like the real UI; what reaches the other
// machines is what the DLL sends (sendNetDealAccepted / sendNetDemandAccepted / sendFromUIDiploEvent).
static ICvDeal1* HstUiScratch() { return g_pkHstLuaUI ? g_pkHstLuaUI->HostScratchDeal() : NULL; }
static ICvDealAI1* HstUiDealAIOf(PlayerTypes ePlayer)
{
	if(!g_pkHstLuaUI || !g_pkHstLuaUI->GetContext() || ePlayer < 0 || ePlayer >= MAX_MAJOR_CIVS)
		return NULL;
	std::auto_ptr<ICvGame1> pkGame(g_pkHstLuaUI->GetContext()->GetGame());
	std::auto_ptr<ICvPlayer1> pkPlayer(pkGame->GetPlayer(ePlayer));
	if(!pkPlayer.get() || pkPlayer->IsHuman())
		return NULL;	// human-human deals (NetPlayerDealProposed) are not modelled
	return pkPlayer->GetDealAI();
}
static int lUiGetScratchDeal(lua_State* L)
{
	std::auto_ptr<ICvDeal1> pkDeal(HstUiScratch());
	ICvScriptSystemUtility1* pkUtil = (pkDeal.get() && g_pkHstLuaUI->GetContext()) ? g_pkHstLuaUI->GetContext()->GetScriptSystemUtility() : NULL;
	if(!pkUtil)
	{
		lua_pushnil(L);
		return 1;
	}
	pkUtil->PushCvDealInstance(L, pkDeal.get());
	return 1;
}
static int lUiProposeDeal(lua_State* L)
{
	std::auto_ptr<ICvDeal1> pkDeal(HstUiScratch());
	std::auto_ptr<ICvDealAI1> pkAI(pkDeal.get() ? HstUiDealAIOf(pkDeal->GetToPlayer()) : NULL);
	lua_pushinteger(L, pkAI.get() ? pkAI->DoHumanOfferDealToThisAI(pkDeal.get()) : -1);
	return 1;
}
static int lUiDemand(lua_State* L)
{
	std::auto_ptr<ICvDeal1> pkDeal(HstUiScratch());
	std::auto_ptr<ICvDealAI1> pkAI(pkDeal.get() ? HstUiDealAIOf(pkDeal->GetToPlayer()) : NULL);
	lua_pushinteger(L, pkAI.get() ? pkAI->DoHumanDemand(pkDeal.get()) : -1);
	return 1;
}
//! mode 0 = UI.DoEqualizeDealWithHuman, 1 = UI.DoWhatDoesAIWant (AI's items fixed), 2 = UI.DoWhatWillAIGive (human's items
//! fixed). GUESS: the exe's flag choice per button (only the scratch deal changes; nothing synced).
static int lUiEqualizeDeal(lua_State* L)
{
	const int iMode = Ni(L, 1);
	std::auto_ptr<ICvDeal1> pkDeal(HstUiScratch());
	std::auto_ptr<ICvDealAI1> pkAI(pkDeal.get() ? HstUiDealAIOf(pkDeal->GetToPlayer()) : NULL);
	bool bGood = false, bCant = false, bOk = false;
	if(pkAI.get())
		bOk = pkAI->DoEqualizeDealWithHuman(pkDeal.get(), pkDeal->GetFromPlayer(), iMode == 1, iMode == 2, bGood, bCant);
	lua_pushboolean(L, bOk);
	return 1;
}
//! UI.RequestLeaveLeader: the leader screen closes; the exe then ends the active diplomacy request (CvDllPlayer::
//! ActiveDiplomacyRequestComplete on the local player).
static int lUiLeaveLeader(lua_State* L)
{
	if(!g_pkHstLuaUI || !g_pkHstLuaUI->GetContext() || !g_pkHstLuaEngine)
		return 0;
	std::auto_ptr<ICvGame1> pkGame(g_pkHstLuaUI->GetContext()->GetGame());
	std::auto_ptr<ICvPlayer1> pkPlayer(pkGame->GetPlayer(g_pkHstLuaEngine->GetActivePlayerForHost()));
	if(pkPlayer.get())
		pkPlayer->ActiveDiplomacyRequestComplete();
	return 0;
}
// ---- prelude (events, auto-stubs, sandbox) ----------------------------------------------------------------------------
// Events: Add() records (set, event, context) in __HstEventLog (-> events-registered.json); calling an event from Lua
// dispatches to its listeners under pcall (as the exe does) and is counted in __HstFireCounts. The context of a listener
// is the context whose globals table is the listener's environment (__HstContextOf, filled by CreateLuaThread).
static const char* s_szPrelude =
	"local gameEventListeners, eventsListeners, luaEventsListeners = __HstGameEventListeners, __HstEventsListeners, __HstLuaEventsListeners\n"
	"local eventLog, fireCounts, contextOf, reportError = __HstEventLog, __HstFireCounts, __HstContextOf, HstReportError\n"
	"__HstGameEventListeners, __HstEventsListeners, __HstLuaEventsListeners, __HstEventLog, __HstFireCounts, __HstContextOf, HstReportError = nil, nil, nil, nil, nil, nil, nil\n"
	"local function contextName(f)\n"
	"  local ok, env = pcall(getfenv, f)\n"
	"  return (ok and env and contextOf[env]) or 'Master'\n"
	"end\n"
	"local function HstMakeEventSet(setName, registry)\n"
	"  return setmetatable({}, { __index = function(t, name)\n"
	"    local listeners = registry[name] or {}\n"
	"    registry[name] = listeners\n"
	"    local key = setName .. '.' .. tostring(name)\n"
	"    local ev = {}\n"
	"    ev.Add = function(f)\n"
	"      if type(f) ~= 'function' then error(key .. '.Add: function expected', 2) end\n"
	"      listeners[#listeners + 1] = f\n"
	"      eventLog[#eventLog + 1] = { setName, name, contextName(f) }\n"
	"    end\n"
	"    ev.Remove = function(f)\n"
	"      for i = #listeners, 1, -1 do if listeners[i] == f then table.remove(listeners, i) end end\n"
	"    end\n"
	"    ev.RemoveAll = function() for i = #listeners, 1, -1 do listeners[i] = nil end end\n"
	"    ev.Count = function() return #listeners end\n"
	"    setmetatable(ev, { __call = function(self, ...)\n"
	"      fireCounts[key] = (fireCounts[key] or 0) + 1\n"
	"      for i = 1, #listeners do\n"
	"        local ok, err = pcall(listeners[i], ...)\n"
	"        if not ok then reportError(key, tostring(err)) end\n"
	"      end\n"
	"    end })\n"
	"    rawset(t, name, ev)\n"
	"    return ev\n"
	"  end })\n"
	"end\n"
	"GameEvents = HstMakeEventSet('GameEvents', gameEventListeners)\n"
	"Events = HstMakeEventSet('Events', eventsListeners)\n"
	"LuaEvents = HstMakeEventSet('LuaEvents', luaEventsListeners)\n"
	"local stubCalled = HstStubCalled\n"
	"HstStubCalled = nil\n"
	// A stub value is callable (logs Global.Name once, returns nil) and indexable (Controls.Box:SetHide(true)).
	"local function stubObject(globalName, key)\n"
	"  return setmetatable({}, {\n"
	"    __call = function(...) stubCalled(globalName, key) return nil end,\n"
	"    __index = function(t, k) local v = stubObject(globalName, key .. '.' .. tostring(k)) rawset(t, k, v) return v end })\n"
	"end\n"
	"function HstAutoStub(globalName)\n"
	"  return setmetatable({}, { __index = function(t, k)\n"
	"    local v = stubObject(globalName, tostring(k))\n"
	"    rawset(t, k, v)\n"
	"    return v\n"
	"  end })\n"
	"end\n"
	"for _, n in ipairs({ 'UI', 'Controls', 'Mouse', 'UIManager', 'OptionsManager', 'Steam', 'Network', 'ContextPtr', 'Path', 'InstanceManager' }) do\n"
	"  if rawget(_G, n) == nil then _G[n] = HstAutoStub(n) end\n"
	"end\n"
	"setmetatable(Locale, getmetatable(HstAutoStub('Locale')))\n"
	"setmetatable(Network, getmetatable(HstAutoStub('Network')))\n"
	// UI selection the DLL's Game.SelectionListGameNetMessage / SelectedCitiesGameNetMessage read (engine UI, bot mode only)
	"do\n"
	"  local su, sc, cl = HstUiSelectUnit, HstUiSelectCity, HstUiClearSelection\n"
	"  UI.SelectUnit = function(u) if u then su(u:GetOwner(), u:GetID()) end end\n"
	"  UI.SelectCity = function(c) if c then sc(c:GetOwner(), c:GetID()) else sc(-1, -1) end end\n"
	"  UI.ClearSelectionList = function() cl() end\n"
	"end\n"
	"HstUiSelectUnit, HstUiSelectCity, HstUiClearSelection = nil, nil, nil\n"
	// trade screen (bot mode): exe UI.* deal functions on the engine UI's scratch deal
	"do\n"
	"  local gs, pd, dm, eq, ll = HstUiGetScratchDeal, HstUiProposeDeal, HstUiDemand, HstUiEqualizeDeal, HstUiLeaveLeader\n"
	"  UI.GetScratchDeal = function() return gs() end\n"
	"  UI.DoProposeDeal = function() return pd() end\n"
	"  UI.DoDemand = function() return dm() end\n"
	"  UI.DoEqualizeDealWithHuman = function() return eq(0) end\n"
	"  UI.DoWhatDoesAIWant = function() return eq(1) end\n"
	"  UI.DoWhatWillAIGive = function() return eq(2) end\n"
	"  UI.RequestLeaveLeader = function() ll() end\n"
	"end\n"
	"HstUiGetScratchDeal, HstUiProposeDeal, HstUiDemand, HstUiEqualizeDeal, HstUiLeaveLeader = nil, nil, nil, nil, nil\n"
	"setmetatable(PreGame, getmetatable(HstAutoStub('PreGame')))\n"
	"setmetatable(Modding, getmetatable(HstAutoStub('Modding')))\n"
	"MapModData = MapModData or {}\n"
	// Per-context ContextPtr (design 3.3): LoadNewContext loads a gameplay context; UI handlers are stored, never called.
	"local loadContext = HstLoadContext\n"
	"HstLoadContext = nil\n"
	"function HstMakeContextPtr(name)\n"
	"  local cp, handlers = {}, {}\n"
	"  cp.LoadNewContext = function(self, child) return loadContext(child) end\n"
	"  cp.GetID = function(self) return name end\n"
	"  cp.IsHidden = function(self) return true end\n"
	"  cp.SetHide = function(self, b) end\n"
	"  cp.IsHotLoad = function(self) return false end\n"
	"  for _, k in ipairs({ 'SetUpdate', 'SetInputHandler', 'SetShutdown', 'SetShowHideHandler', 'SetPostInit' }) do\n"
	"    cp[k] = function(self, f) handlers[k] = f end\n"
	"  end\n"
	"  cp.ClearUpdate = function(self) handlers.SetUpdate = nil end\n"
	"  cp.LookUpControl = function(self, path) return HstAutoStub('LookUpControl') end\n"
	"  return setmetatable(cp, getmetatable(HstAutoStub('ContextPtr')))\n"
	"end\n"
	// sandbox (modiki "Lua in Civ5"): no file/process access; virtual clocks; deterministic math.random
	"dofile, loadfile, load, require, module, io, package = nil, nil, nil, nil, nil, nil, nil\n"
	"do\n"
	"  local realDate, difftime = os.date, os.difftime\n"
	"  local vclock, vtime = HstVirtualClock, HstVirtualTime\n"
	"  os = { clock = vclock, time = vtime, difftime = difftime,\n"
	"         date = function(fmt, t) fmt = fmt or '%c'; if fmt:sub(1, 1) ~= '!' then fmt = '!' .. fmt end; return realDate(fmt, t or vtime()) end }\n"
	"end\n"
	"HstVirtualClock, HstVirtualTime = nil, nil\n"
	"math.random, math.randomseed = HstRandom, HstRandomSeed\n"
	"HstRandom, HstRandomSeed = nil, nil\n";

bool HstExeLuaRegister(HstScriptSystem* pkSS, lua_State* L)
{
	const int iTop = lua_gettop(L);

	// Globals
	HstRegister(L, LUA_GLOBALSINDEX, "print", lPrint, pkSS);
	HstRegister(L, LUA_GLOBALSINDEX, "include", lInclude, pkSS);
	HstRegister(L, LUA_GLOBALSINDEX, "HstStubCalled", lStubCalled, pkSS);
	HstRegister(L, LUA_GLOBALSINDEX, "HstVirtualClock", lVirtualClock, pkSS);
	HstRegister(L, LUA_GLOBALSINDEX, "HstVirtualTime", lVirtualTime, pkSS);
	HstRegister(L, LUA_GLOBALSINDEX, "HstRandom", lRandom, pkSS);
	HstRegister(L, LUA_GLOBALSINDEX, "HstRandomSeed", lRandomSeed, pkSS);

	// table.fill / table.count / math.clamp
	lua_getglobal(L, "table");
	lua_pushcfunction(L, lTableFill);
	lua_setfield(L, -2, "fill");
	lua_pushcfunction(L, lTableCount);
	lua_setfield(L, -2, "count");
	lua_pop(L, 1);
	lua_getglobal(L, "math");
	lua_pushcfunction(L, lMathClamp);
	lua_setfield(L, -2, "clamp");
	lua_pop(L, 1);

	// DB
	luaL_newmetatable(L, "HstDbQuery");
	lua_pushcfunction(L, lDbQueryGc);
	lua_setfield(L, -2, "__gc");
	lua_pop(L, 1);
	lua_newtable(L);
	HstRegister(L, lua_gettop(L), "Query", lDbQuery, pkSS);
	lua_setglobal(L, "DB");

	// GameInfo (exe 0x7ccf0: PushDatabase on the app connection)
	const int iBefore = lua_gettop(L);
	Database::Scripting::Lua::PushDatabase(L, *pkSS->GetDatabase());
	if(lua_gettop(L) != iBefore + 1)
	{
		HstLog("lua: PushDatabase pushed %d values", lua_gettop(L) - iBefore);
		return false;
	}
	lua_setglobal(L, "GameInfo");

	// GameDefines: Name -> Value from Defines
	lua_newtable(L);
	{
		Database::Results kDefines;
		if(pkSS->GetDatabase()->Execute(kDefines, "SELECT Name, Value FROM Defines"))
		{
			while(kDefines.Step())
			{
				const char* szName = kDefines.GetText(0);
				if(!szName)
					continue;
				switch(kDefines.ColumnType(1))
				{
				case Database::COLTYPE_INT:
					lua_pushinteger(L, kDefines.GetInt(1));
					break;
				case Database::COLTYPE_FLOAT:
					lua_pushnumber(L, kDefines.GetDouble(1));
					break;
				case Database::COLTYPE_TEXT:
					lua_pushstring(L, kDefines.GetText(1));
					break;
				default:
					lua_pushnil(L);
					break;
				}
				lua_setfield(L, -2, szName);
			}
		}
	}
	lua_setglobal(L, "GameDefines");

	// Locale
	lua_newtable(L);
	{
		const int t = lua_gettop(L);
		HstRegister(L, t, "ConvertTextKey", lLocaleConvertTextKey, pkSS);
		HstRegister(L, t, "Lookup", lLocaleConvertTextKey, pkSS);
		HstRegister(L, t, "HasTextKey", lLocaleHasTextKey, pkSS);
		HstRegister(L, t, "ToUpper", lLocaleToUpper, pkSS);
		HstRegister(L, t, "ToLower", lLocaleToLower, pkSS);
	}
	lua_setglobal(L, "Locale");

	// PreGame
	lua_newtable(L);
	{
		const int t = lua_gettop(L);
		HstRegister(L, t, "GetWorldSize", lPreGameGetWorldSize, pkSS);
		HstRegister(L, t, "GetEra", lPreGameGetEra, pkSS);
		HstRegister(L, t, "GetGameSpeed", lPreGameGetGameSpeed, pkSS);
		HstRegister(L, t, "GetClimate", lPreGameGetClimate, pkSS);
		HstRegister(L, t, "GetSeaLevel", lPreGameGetSeaLevel, pkSS);
		HstRegister(L, t, "GetNumMinorCivs", lPreGameGetNumMinorCivs, pkSS);
		HstRegister(L, t, "GetGameType", lPreGameGetGameType, pkSS);
		HstRegister(L, t, "GetMaxTurns", lPreGameGetMaxTurns, pkSS);
		HstRegister(L, t, "GetSlotStatus", lPreGameGetSlotStatus, pkSS);
		HstRegister(L, t, "GetSlotClaim", lPreGameGetSlotClaim, pkSS);
		HstRegister(L, t, "GetCivilization", lPreGameGetCivilization, pkSS);
		HstRegister(L, t, "GetLeaderHead", lPreGameGetLeaderHead, pkSS);
		HstRegister(L, t, "GetTeam", lPreGameGetTeam, pkSS);
		HstRegister(L, t, "GetHandicap", lPreGameGetHandicap, pkSS);
		HstRegister(L, t, "GetMinorCivType", lPreGameGetMinorCivType, pkSS);
		HstRegister(L, t, "IsMinorCiv", lPreGameIsMinorCiv, pkSS);
		HstRegister(L, t, "IsHuman", lPreGameIsHuman, pkSS);
		HstRegister(L, t, "IsMultiplayerGame", lPreGameIsMultiplayerGame, pkSS);
		HstRegister(L, t, "IsHotSeatGame", lPreGameIsHotSeatGame, pkSS);
		HstRegister(L, t, "IsInternetGame", lPreGameIsInternetGame, pkSS);
		HstRegister(L, t, "GetLoadWBScenario", lPreGameGetLoadWBScenario, pkSS);
		HstRegister(L, t, "IsVictory", lPreGameIsVictory, pkSS);
		HstRegister(L, t, "GetMapScript", lPreGameGetMapScript, pkSS);
		HstRegister(L, t, "GetMapOption", lPreGameGetMapOption, pkSS);
		HstRegister(L, t, "GetGameOption", lPreGameGetGameOption, pkSS);
	}
	lua_setglobal(L, "PreGame");

	// Modding: OpenSaveData (sim); everything else is an auto-stub (metatable set by the prelude).
	lua_newtable(L);
	HstRegister(L, lua_gettop(L), "OpenSaveData", lModdingOpenSaveData, pkSS);
	lua_setglobal(L, "Modding");

	// Exe enums that Lekmod reads (LekmodUtilities:is_civilization_active uses SlotStatus["SS_TAKEN"]); CvEnums.h values.
	lua_newtable(L);
	{
		static const char* s_aszSlotStatus[] = { "SS_OPEN", "SS_COMPUTER", "SS_CLOSED", "SS_TAKEN", "SS_OBSERVER" };
		for(int i = 0; i < (int)HST_ARRAY_SIZE(s_aszSlotStatus); ++i)
		{
			lua_pushinteger(L, i);
			lua_setfield(L, -2, s_aszSlotStatus[i]);
		}
	}
	lua_setglobal(L, "SlotStatus");
	lua_newtable(L);
	{
		static const char* s_aszSlotClaim[] = { "SLOTCLAIM_UNASSIGNED", "SLOTCLAIM_RESERVED", "SLOTCLAIM_ASSIGNED" };
		for(int i = 0; i < (int)HST_ARRAY_SIZE(s_aszSlotClaim); ++i)
		{
			lua_pushinteger(L, i);
			lua_setfield(L, -2, s_aszSlotClaim[i]);
		}
	}
	lua_setglobal(L, "SlotClaim");
	HstRegister(L, LUA_GLOBALSINDEX, "ToHexFromGrid", lToHexFromGrid, pkSS);
	HstRegister(L, LUA_GLOBALSINDEX, "ToGridFromHex", lToGridFromHex, pkSS);
	HstRegister(L, LUA_GLOBALSINDEX, "HexToWorld", lHexToWorld, pkSS);
	HstRegister(L, LUA_GLOBALSINDEX, "HstLoadContext", lLoadContext, pkSS);
	HstRegister(L, LUA_GLOBALSINDEX, "HstReportError", lReportError, pkSS);
	// Network (exe bindings); unknown members stay auto-stubs (metatable set by the prelude)
	lua_newtable(L);
	{
		const int t = lua_gettop(L);
#define HST_NETREG(name) HstRegister(L, t, #name, lNet##name, pkSS)
		HST_NETREG(SendResearch); HST_NETREG(SendUpdatePolicies); HST_NETREG(SendDoTask); HST_NETREG(SendGoodyChoice);
		HST_NETREG(SendIdeologyChoice); HST_NETREG(SendArchaeologyChoice); HST_NETREG(SendLeagueVoteEnact); HST_NETREG(SendLeagueVoteRepeal);
		HST_NETREG(SendLeagueProposeEnact); HST_NETREG(SendLeagueProposeRepeal); HST_NETREG(SendLeagueVoteAbstain); HST_NETREG(SendMoveSpy);
		HST_NETREG(SendStageCoup); HST_NETREG(SendFoundPantheon); HST_NETREG(SendFoundReligion); HST_NETREG(SendEnhanceReligion);
		HST_NETREG(SendFaithPurchase); HST_NETREG(SendChangeWar); HST_NETREG(SendIgnoreWarning); HST_NETREG(SendPledgeMinorProtection);
		HST_NETREG(SendMinorNoUnitSpawning); HST_NETREG(SendLiberateMinor); HST_NETREG(SendDiploVote); HST_NETREG(SendBarbarianRansom);
		HST_NETREG(SendGiftUnit); HST_NETREG(SendReturnCivilian); HST_NETREG(SendUpdateCityCitizens); HST_NETREG(SendSellBuilding);
		HST_NETREG(SendCityBuyPlot); HST_NETREG(SendGreatPersonChoice); HST_NETREG(SendMayaBonusChoice); HST_NETREG(SendFaithGreatPersonChoice);
		HST_NETREG(SendSetCityAIFocus); HST_NETREG(SendSetCityAvoidGrowth); HST_NETREG(SendChangeIdeology); HST_NETREG(SendChat);
		HST_NETREG(SendTurnUnready); HST_NETREG(HasSentNetTurnComplete); HST_NETREG(HasSentNetTurnAllComplete);
#undef HST_NETREG
	}
	lua_setglobal(L, "Network");
	HstRegister(L, LUA_GLOBALSINDEX, "HstUiSelectUnit", lUiSelectUnit, pkSS);
	HstRegister(L, LUA_GLOBALSINDEX, "HstUiSelectCity", lUiSelectCity, pkSS);
	HstRegister(L, LUA_GLOBALSINDEX, "HstUiClearSelection", lUiClearSelection, pkSS);
	HstRegister(L, LUA_GLOBALSINDEX, "HstUiTakePopups", lUiTakePopups, pkSS);
	HstRegister(L, LUA_GLOBALSINDEX, "HstUiGetScratchDeal", lUiGetScratchDeal, pkSS);
	HstRegister(L, LUA_GLOBALSINDEX, "HstUiProposeDeal", lUiProposeDeal, pkSS);
	HstRegister(L, LUA_GLOBALSINDEX, "HstUiDemand", lUiDemand, pkSS);
	HstRegister(L, LUA_GLOBALSINDEX, "HstUiEqualizeDeal", lUiEqualizeDeal, pkSS);
	HstRegister(L, LUA_GLOBALSINDEX, "HstUiLeaveLeader", lUiLeaveLeader, pkSS);

	// Event listener registries, read by the prelude and by HstScriptSystem::DispatchGameEvent / the host.
	static const char* s_aszRegistries[][2] = {
		{ "HstGameEventListeners", "__HstGameEventListeners" },
		{ "HstEventsListeners", "__HstEventsListeners" },
		{ "HstLuaEventsListeners", "__HstLuaEventsListeners" },
		{ "HstEventLog", "__HstEventLog" },
		{ "HstFireCounts", "__HstFireCounts" },
		{ "HstContextOf", "__HstContextOf" },
	};
	for(int i = 0; i < (int)HST_ARRAY_SIZE(s_aszRegistries); ++i)
	{
		lua_newtable(L);
		if(strcmp(s_aszRegistries[i][0], "HstContextOf") == 0)
		{
			lua_newtable(L);
			lua_pushstring(L, "k");
			lua_setfield(L, -2, "__mode");
			lua_setmetatable(L, -2);
		}
		lua_pushvalue(L, -1);
		lua_setfield(L, LUA_REGISTRYINDEX, s_aszRegistries[i][0]);
		lua_setglobal(L, s_aszRegistries[i][1]);
	}

	lua_settop(L, iTop);
	return pkSS->RunString(L, s_szPrelude, "=civhost prelude");
}
