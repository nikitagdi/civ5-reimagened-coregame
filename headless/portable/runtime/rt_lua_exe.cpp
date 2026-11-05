// Exe-provided Lua globals and the Lua sandbox. Ported from headless/host/src/script/HstExeLua.cpp.
// Ported: print, include, Locale, PreGame, Modding.OpenSaveData, GameDefines, SlotStatus/SlotClaim, hex helpers,
// table.fill/count, math.clamp, virtual os.clock/os.time, deterministic math.random, event tables, ContextPtr, prelude.
// Not ported: Network.* and the UI-bot bindings (UI.SelectUnit, trade screen, notifications: all auto-stubs here, see
// rt_lua_prelude.cpp). GameInfo / DB.Query: rt_lua_gameinfo.cpp (a rewrite of the closed CvGameDatabase code).
#include "CvGameCoreDLLPCH.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "CvLocalization.h"
#include "CvPreGame.h"
#include "rt_lua.h"
#include "rt_lua_gameinfo.h"
#include "rt_ui_replica.h"
#include "lekmod_portable/host_log.h"

namespace lekmod_portable {

namespace {

RtScriptSystem* SS(lua_State* L) { return (RtScriptSystem*)lua_touserdata(L, lua_upvalueindex(1)); }

void Register(lua_State* L, int table, const char* name, lua_CFunction fn, RtScriptSystem* ss)
{
	lua_pushlightuserdata(L, ss);
	lua_pushcclosure(L, fn, 1);
	lua_setfield(L, table, name);
}

// ---- print / include ---------------------------------------------------------------------------------------------
int lPrint(lua_State* L)
{
	std::string str;
	const int n = lua_gettop(L);
	for (int i = 1; i <= n; ++i)
	{
		if (i > 1) str += "\t";
		switch (lua_type(L, i))
		{
		case LUA_TSTRING:
		case LUA_TNUMBER: str += lua_tostring(L, i); break;
		case LUA_TBOOLEAN: str += lua_toboolean(L, i) ? "true" : "false"; break;
		case LUA_TNIL: str += "nil"; break;
		default: str += lua_typename(L, lua_type(L, i)); break;  // no addresses: logs stay comparable across runs
		}
	}
	SS(L)->LogLua(L, str.c_str());
	return 0;
}

// include(name): stem match through the source, executed in the caller's environment.
int lInclude(lua_State* L)
{
	RtScriptSystem* ss = SS(L);
	const char* name = luaL_checkstring(L, 1);
	std::string path;
	if (!ss->GetSource() || !ss->GetSource()->ResolveInclude(name, path))
	{
		if (ss->GetSource() && ss->GetSource()->IsKnownMissing(name))
		{
			lua_pushboolean(L, 0);
			return 1;
		}
		std::string msg = std::string("include: file not found: ") + name;
		Log(LOG_WARN, "lua: %s", msg.c_str());
		ss->LogLua(L, msg.c_str());
		lua_pushboolean(L, 0);
		return 1;
	}
	std::string source;
	if (!ss->GetSource()->ReadSource(path, source)) return luaL_error(L, "include: cannot read %s", name);
	size_t slash = path.find_last_of("\\/");
	std::string chunk = "@" + (slash == std::string::npos ? path : path.substr(slash + 1));
	if (luaL_loadbuffer(L, source.data(), source.size(), chunk.c_str()) != 0) return lua_error(L);
	lua_Debug ar;
	if (lua_getstack(L, 1, &ar) && lua_getinfo(L, "f", &ar))
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

// ---- Locale --------------------------------------------------------------------------------------------------------
int lLocaleConvertTextKey(lua_State* L)
{
	const char* key = lua_tostring(L, 1);
	if (!key)
	{
		lua_pushstring(L, "");
		return 1;
	}
	Localization::String text = Localization::Lookup(key);
	const int n = lua_gettop(L);
	for (int i = 2; i <= n; ++i)
	{
		switch (lua_type(L, i))
		{
		case LUA_TNUMBER:
		{
			const double d = lua_tonumber(L, i);
			const int iv = (int)d;
			if ((double)iv == d) text.PushArgument(iv); else text.PushArgument(d);
			break;
		}
		case LUA_TSTRING:
		{
			size_t len = 0;
			const char* s = lua_tolstring(L, i, &len);
			text.PushArgument(s, len);
			break;
		}
		case LUA_TBOOLEAN: text.PushArgument(lua_toboolean(L, i) ? 1 : 0); break;
		default: text.PushArgument(""); break;
		}
	}
	const char* out = text.toUTF8();
	lua_pushstring(L, out ? out : "");
	return 1;
}

int lLocaleHasTextKey(lua_State* L)
{
	size_t len = 0;
	const char* key = lua_tolstring(L, 1, &len);
	lua_pushboolean(L, key && Localization::GetStringDictionary().HasKey(key, (unsigned int)len));
	return 1;
}

int LocaleCase(lua_State* L, bool upper)
{
	size_t len = 0;
	const char* s = luaL_checklstring(L, 1, &len);
	std::string str(s, len);
	for (size_t i = 0; i < str.size(); ++i)
	{
		unsigned char c = (unsigned char)str[i];
		if (c < 0x80) str[i] = (char)(upper ? toupper(c) : tolower(c));
	}
	lua_pushlstring(L, str.data(), str.size());
	return 1;
}
int lLocaleToUpper(lua_State* L) { return LocaleCase(L, true); }
int lLocaleToLower(lua_State* L) { return LocaleCase(L, false); }

// ---- PreGame (thin wrappers over CvPreGame, which is what ICvPreGame1 forwards to) ---------------------------------------
PlayerTypes PlayerArg(lua_State* L)
{
	const int p = luaL_checkint(L, 1);
	luaL_argcheck(L, p >= 0 && p < MAX_PLAYERS, 1, "player index out of range");
	return (PlayerTypes)p;
}

#define RT_PREGAME_INT(fn, expr) int fn(lua_State* L) { lua_pushinteger(L, (int)(expr)); return 1; }
#define RT_PREGAME_BOOL(fn, expr) int fn(lua_State* L) { lua_pushboolean(L, (expr) ? 1 : 0); return 1; }

RT_PREGAME_INT(lPreGameGetWorldSize, CvPreGame::worldSize())
RT_PREGAME_INT(lPreGameGetEra, CvPreGame::era())
RT_PREGAME_INT(lPreGameGetGameSpeed, CvPreGame::gameSpeed())
RT_PREGAME_INT(lPreGameGetClimate, CvPreGame::climate())
RT_PREGAME_INT(lPreGameGetSeaLevel, CvPreGame::seaLevel())
RT_PREGAME_INT(lPreGameGetNumMinorCivs, CvPreGame::numMinorCivs())
RT_PREGAME_INT(lPreGameGetGameType, CvPreGame::gameType())
RT_PREGAME_INT(lPreGameGetMaxTurns, CvPreGame::maxTurns())
RT_PREGAME_INT(lPreGameGetSlotStatus, CvPreGame::slotStatus(PlayerArg(L)))
RT_PREGAME_INT(lPreGameGetSlotClaim, CvPreGame::slotClaim(PlayerArg(L)))
RT_PREGAME_INT(lPreGameGetCivilization, CvPreGame::civilization(PlayerArg(L)))
RT_PREGAME_INT(lPreGameGetLeaderHead, CvPreGame::leaderHead(PlayerArg(L)))
RT_PREGAME_INT(lPreGameGetTeam, CvPreGame::teamType(PlayerArg(L)))
RT_PREGAME_INT(lPreGameGetHandicap, CvPreGame::handicap(PlayerArg(L)))
RT_PREGAME_INT(lPreGameGetMinorCivType, CvPreGame::minorCivType(PlayerArg(L)))
RT_PREGAME_BOOL(lPreGameIsMinorCiv, CvPreGame::isMinorCiv(PlayerArg(L)))
RT_PREGAME_BOOL(lPreGameIsHuman, CvPreGame::isHuman(PlayerArg(L)))
RT_PREGAME_BOOL(lPreGameIsMultiplayerGame, CvPreGame::isNetworkMultiplayerGame())
RT_PREGAME_BOOL(lPreGameIsHotSeatGame, CvPreGame::isHotSeatGame())
RT_PREGAME_BOOL(lPreGameIsInternetGame, CvPreGame::isInternetGame())
RT_PREGAME_BOOL(lPreGameGetLoadWBScenario, CvPreGame::loadWBScenario())
RT_PREGAME_BOOL(lPreGameIsVictory, CvPreGame::isVictory((VictoryTypes)luaL_checkint(L, 1)))

int lPreGameGetMapScript(lua_State* L)
{
	lua_pushstring(L, CvPreGame::mapScriptName().c_str());
	return 1;
}

int lPreGameGetMapOption(lua_State* L)
{
	int value = 0;
	if (lua_type(L, 1) == LUA_TNUMBER)
	{
		char name[32];
		snprintf(name, sizeof(name), "%d", (int)lua_tointeger(L, 1));
		if (!CvPreGame::GetMapOption(name, value)) return 0;
	}
	else if (!CvPreGame::GetMapOption(luaL_checkstring(L, 1), value))
		return 0;
	lua_pushinteger(L, value);
	return 1;
}

int lPreGameGetGameOption(lua_State* L)
{
	int value = -1;
	bool found = false;
	if (lua_type(L, 1) == LUA_TNUMBER)
		found = CvPreGame::GetGameOption((GameOptionTypes)lua_tointeger(L, 1), value);
	else
		found = CvPreGame::GetGameOption(luaL_checkstring(L, 1), value);
	lua_pushinteger(L, found ? value : -1);
	return 1;
}

// PreGame members that read the pregame of the game (shared by the core's Lua and a replica's UI state).
struct PreGameFn
{
	const char* name;
	lua_CFunction fn;
};
const PreGameFn kPreGameFns[] = {
	{"GetWorldSize", lPreGameGetWorldSize},
	{"GetEra", lPreGameGetEra},
	{"GetGameSpeed", lPreGameGetGameSpeed},
	{"GetClimate", lPreGameGetClimate},
	{"GetSeaLevel", lPreGameGetSeaLevel},
	{"GetNumMinorCivs", lPreGameGetNumMinorCivs},
	{"GetGameType", lPreGameGetGameType},
	{"GetMaxTurns", lPreGameGetMaxTurns},
	{"GetSlotStatus", lPreGameGetSlotStatus},
	{"GetSlotClaim", lPreGameGetSlotClaim},
	{"GetCivilization", lPreGameGetCivilization},
	{"GetLeaderHead", lPreGameGetLeaderHead},
	{"GetTeam", lPreGameGetTeam},
	{"GetHandicap", lPreGameGetHandicap},
	{"GetMinorCivType", lPreGameGetMinorCivType},
	{"IsMinorCiv", lPreGameIsMinorCiv},
	{"IsHuman", lPreGameIsHuman},
	{"IsMultiplayerGame", lPreGameIsMultiplayerGame},
	{"IsHotSeatGame", lPreGameIsHotSeatGame},
	{"IsInternetGame", lPreGameIsInternetGame},
	{"GetLoadWBScenario", lPreGameGetLoadWBScenario},
	{"IsVictory", lPreGameIsVictory},
	{"GetMapScript", lPreGameGetMapScript},
	{"GetMapOption", lPreGameGetMapOption},
	{"GetGameOption", lPreGameGetGameOption},
};

void FillPreGame(lua_State* L, int table, RtScriptSystem* ss, bool onlyMissing)
{
	for (size_t i = 0; i < sizeof(kPreGameFns) / sizeof(kPreGameFns[0]); ++i)
	{
		if (onlyMissing)
		{
			lua_getfield(L, table, kPreGameFns[i].name);
			const bool present = !lua_isnil(L, -1);
			lua_pop(L, 1);
			if (present) continue;
		}
		Register(L, table, kPreGameFns[i].name, kPreGameFns[i].fn, ss);
	}
}

// ---- sandbox helpers -----------------------------------------------------------------------------------------------
int lStubCalled(lua_State* L)
{
	std::string key = std::string(luaL_optstring(L, 1, "?")) + "." + luaL_optstring(L, 2, "?");
	if (SS(L)->NoteStub(key))
	{
		Log(LOG_DEBUG, "lua: auto-stub %s", key.c_str());
		std::string msg = "[portable] auto-stub " + key;
		SS(L)->LogLua(L, msg.c_str());
	}
	return 0;
}

int lVirtualClock(lua_State* L)
{
	lua_pushnumber(L, SS(L)->NextVirtualClock());
	return 1;
}

int lVirtualTime(lua_State* L)
{
	if (lua_istable(L, 1)) return luaL_error(L, "os.time(table) is not supported");
	lua_pushnumber(L, 1356998400.0);  // 2013-01-01 00:00:00 UTC, fixed
	return 1;
}

// MSVCRT rand() sequence with a host-owned state (deterministic; every use is counted and the first logged).
int lRandom(lua_State* L)
{
	RtScriptSystem* ss = SS(L);
	if (ss->CountRandCall() == 0) Log(LOG_DEBUG, "lua: math.random used (deterministic host LCG)");
	const int rnd = (int)((ss->NextRand() >> 16) & 0x7fff);
	const lua_Number r = (lua_Number)(rnd % 32767) / (lua_Number)32767;  // lmathlib: (rand()%RAND_MAX)/RAND_MAX
	switch (lua_gettop(L))
	{
	case 0: lua_pushnumber(L, r); break;
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
	default: return luaL_error(L, "wrong number of arguments");
	}
	return 1;
}

int lRandomSeed(lua_State* L)
{
	Log(LOG_DEBUG, "lua: math.randomseed(%d)", luaL_checkint(L, 1));
	SS(L)->SeedRand((unsigned int)luaL_checkint(L, 1));
	return 0;
}

// ---- library extensions the exe registers ------------------------------------------------------------------------------
// table.fill(value, n) -> new { value x n }; table.fill(t, value, n) fills t[1..n] in place and returns t. 1-based.
int lTableFill(lua_State* L)
{
	int valueIdx = 1;
	int n = 0;
	if (lua_gettop(L) >= 3 && lua_istable(L, 1))
	{
		valueIdx = 2;
		n = luaL_checkint(L, 3);
		lua_pushvalue(L, 1);
	}
	else
	{
		luaL_checkany(L, 1);
		n = luaL_checkint(L, 2);
		lua_createtable(L, n > 0 ? n : 0, 0);
	}
	for (int i = 1; i <= n; ++i)
	{
		lua_pushvalue(L, valueIdx);
		lua_rawseti(L, -2, i);
	}
	return 1;
}

// table.count(t) -> number of entries (pairs).
int lTableCount(lua_State* L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	int n = 0;
	lua_pushnil(L);
	while (lua_next(L, 1) != 0)
	{
		++n;
		lua_pop(L, 1);
	}
	lua_pushinteger(L, n);
	return 1;
}

int lMathClamp(lua_State* L)
{
	lua_Number x = luaL_checknumber(L, 1);
	const lua_Number lo = luaL_checknumber(L, 2);
	const lua_Number hi = luaL_checknumber(L, 3);
	if (x < lo) x = lo;
	if (x > hi) x = hi;
	lua_pushnumber(L, x);
	return 1;
}

// ---- Modding.OpenSaveData (SimpleValues in ./Civ5SavedGameDatabase.db, served from memory by rt_fs) ----------------------
// GetValue/SetValue accept both call styles ("Lek_SaveData.GetValue(name)" and "data:GetValue(name)"): a leading table
// argument is skipped.
int SaveDataArgBase(lua_State* L) { return lua_istable(L, 1) ? 2 : 1; }

int lSaveDataGetValue(lua_State* L)
{
	const int base = SaveDataArgBase(L);
	const char* name = luaL_checkstring(L, base);
	Database::Connection* db = SS(L)->GetSaveData();
	if (!db) return 0;
	Database::Results results;
	if (!db->Execute(results, "SELECT Value FROM SimpleValues WHERE Name = ? LIMIT 1"))
		return luaL_error(L, "SaveData.GetValue: %s", db->ErrorMessage());
	results.Bind(1, name);
	if (!results.Step()) return 0;
	switch (results.ColumnType(0))
	{
	case Database::COLTYPE_INT: lua_pushnumber(L, (lua_Number)results.GetInt64(0)); return 1;
	case Database::COLTYPE_FLOAT: lua_pushnumber(L, results.GetDouble(0)); return 1;
	case Database::COLTYPE_TEXT: lua_pushstring(L, results.GetText(0)); return 1;
	default: return 0;
	}
}

int lSaveDataSetValue(lua_State* L)
{
	const int base = SaveDataArgBase(L);
	const char* name = luaL_checkstring(L, base);
	Database::Connection* db = SS(L)->GetSaveData();
	if (!db) return 0;
	Database::Results results;
	if (!db->Execute(results, "INSERT OR REPLACE INTO SimpleValues(Name, Value) VALUES(?, ?)"))
		return luaL_error(L, "SaveData.SetValue: %s", db->ErrorMessage());
	results.Bind(1, name);
	const int vi = base + 1;
	switch (lua_type(L, vi))
	{
	case LUA_TNUMBER:
	{
		const double d = lua_tonumber(L, vi);
		const int i = (int)d;
		if ((double)i == d) results.Bind(2, i); else results.Bind(2, d);
		break;
	}
	case LUA_TBOOLEAN: results.Bind(2, lua_toboolean(L, vi) ? 1 : 0); break;
	case LUA_TSTRING:
	{
		size_t len = 0;
		const char* s = lua_tolstring(L, vi, &len);
		results.Bind(2, s, (int)len, true);
		break;
	}
	default: results.BindNULL(2); break;
	}
	if (!results.Execute()) return luaL_error(L, "SaveData.SetValue(%s): %s", name, db->ErrorMessage());
	return 0;
}

int lModdingOpenSaveData(lua_State* L)
{
	RtScriptSystem* ss = SS(L);
	lua_getfield(L, LUA_REGISTRYINDEX, "HstSaveDataObject");
	if (lua_istable(L, -1)) return 1;
	lua_pop(L, 1);
	lua_newtable(L);
	const int t = lua_gettop(L);
	Register(L, t, "GetValue", lSaveDataGetValue, ss);
	Register(L, t, "SetValue", lSaveDataSetValue, ss);
	lua_pushvalue(L, t);
	lua_setfield(L, LUA_REGISTRYINDEX, "HstSaveDataObject");
	return 1;
}

// ---- contexts / errors -----------------------------------------------------------------------------------------------
// HstLoadContext(name) -> the new context's ContextPtr, or nil (ContextPtr:LoadNewContext).
int lLoadContext(lua_State* L)
{
	lua_State* T = SS(L)->LoadContext(luaL_checkstring(L, 1));
	if (!T)
	{
		lua_pushnil(L);
		return 1;
	}
	lua_getfield(T, LUA_GLOBALSINDEX, "ContextPtr");
	lua_xmove(T, L, 1);
	return 1;
}

// HstUiEvent(set, name, ...): Events.X(...) / GameEvents.X(...) called from the core's Lua, after its listeners ran (the prelude). A replica's UI gets a copy (rt_ui_replica.h).
int lUiEvent(lua_State* L)
{
	RtUiReplica* rep = SS(L)->UiReplica();
	if (!rep || !rep->Active() || lua_gettop(L) < 2) return 0;
	const char* set = lua_tostring(L, 1);
	const char* name = lua_tostring(L, 2);
	if (!set || !name) return 0;
	if (strcmp(set, "Events") == 0)
		rep->EmitLuaValues(GC_UI_EV_LUA_EVENT, name, L, 3, lua_gettop(L));
	else if (strcmp(set, "GameEvents") == 0)
		rep->EmitLuaValues(GC_UI_EV_GAME_EVENT, name, L, 3, lua_gettop(L));
	return 0;
}

// HstReportError(where, message): a listener error caught by the Lua-side event dispatch.
int lReportError(lua_State* L)
{
	SS(L)->ReportError(luaL_optstring(L, 1, "?"), luaL_optstring(L, 2, "?"));
	return 0;
}

// ---- hex helpers (exe globals; pure math, used for popup positions) -------------------------------------------------------
// ToHexFromGrid(Vector2/{x,y}) -> {x = gx - floor(gy / 2), y = gy} (Civ5 offset -> axial).
int lToHexFromGrid(lua_State* L)
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

int lToGridFromHex(lua_State* L)
{
	const int x = luaL_checkint(L, 1);
	const int y = luaL_checkint(L, 2);
	lua_pushinteger(L, x + (y >= 0 ? y / 2 : -((-y + 1) / 2)));
	lua_pushinteger(L, y);
	return 2;
}

// HexToWorld(hex) -> {x, y, z}; the world position only feeds UI calls (popup text), so any finite value is fine.
int lHexToWorld(lua_State* L)
{
	double x = 0.0, y = 0.0;
	if (lua_istable(L, 1))
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

void SetEnum(lua_State* L, const char* global, const char* const* names, int count)
{
	lua_newtable(L);
	for (int i = 0; i < count; ++i)
	{
		lua_pushinteger(L, i);
		lua_setfield(L, -2, names[i]);
	}
	lua_setglobal(L, global);
}

}  // namespace

// GameDefines: Name -> Value from Defines (a new global table).
void RegisterExeGameDefines(lua_State* L, Database::Connection* db)
{
	lua_newtable(L);
	if (db)
	{
		Database::Results defines;
		if (db->Execute(defines, "SELECT Name, Value FROM Defines"))
		{
			while (defines.Step())
			{
				const char* name = defines.GetText(0);
				if (!name) continue;
				switch (defines.ColumnType(1))
				{
				case Database::COLTYPE_INT: lua_pushinteger(L, defines.GetInt(1)); break;
				case Database::COLTYPE_FLOAT: lua_pushnumber(L, defines.GetDouble(1)); break;
				case Database::COLTYPE_TEXT: lua_pushstring(L, defines.GetText(1)); break;
				default: lua_pushnil(L); break;
				}
				lua_setfield(L, -2, name);
			}
		}
	}
	lua_setglobal(L, "GameDefines");
}

// B4 M2 WO-1 (gc_lua_attach_ui): the pregame reads in a UI Lua state. A PreGame table the host made already keeps its members (the client's: GetNickName, IsMultiplayerGame, ...).
void RegisterExePreGame(lua_State* L)
{
	lua_getglobal(L, "PreGame");
	if (!lua_istable(L, -1))
	{
		lua_pop(L, 1);
		lua_newtable(L);
		lua_pushvalue(L, -1);
		lua_setglobal(L, "PreGame");
	}
	FillPreGame(L, lua_gettop(L), NULL, true);
	lua_pop(L, 1);
}

bool RtScriptSystem::RegisterExeGlobals(lua_State* L)
{
	const int top = lua_gettop(L);
	RtScriptSystem* ss = this;

	Register(L, LUA_GLOBALSINDEX, "print", lPrint, ss);
	Register(L, LUA_GLOBALSINDEX, "include", lInclude, ss);
	Register(L, LUA_GLOBALSINDEX, "HstStubCalled", lStubCalled, ss);
	Register(L, LUA_GLOBALSINDEX, "HstVirtualClock", lVirtualClock, ss);
	Register(L, LUA_GLOBALSINDEX, "HstVirtualTime", lVirtualTime, ss);
	Register(L, LUA_GLOBALSINDEX, "HstRandom", lRandom, ss);
	Register(L, LUA_GLOBALSINDEX, "HstRandomSeed", lRandomSeed, ss);

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

	// DB.Query and GameInfo (rt_lua_gameinfo.cpp), before GameDefines as in civhost.
	if (m_db) RegisterGameInfo(L, m_db);

	RegisterExeGameDefines(L, m_db);

	// Locale
	lua_newtable(L);
	{
		const int t = lua_gettop(L);
		Register(L, t, "ConvertTextKey", lLocaleConvertTextKey, ss);
		Register(L, t, "Lookup", lLocaleConvertTextKey, ss);
		Register(L, t, "HasTextKey", lLocaleHasTextKey, ss);
		Register(L, t, "ToUpper", lLocaleToUpper, ss);
		Register(L, t, "ToLower", lLocaleToLower, ss);
	}
	lua_setglobal(L, "Locale");

	// PreGame
	lua_newtable(L);
	FillPreGame(L, lua_gettop(L), ss, false);
	lua_setglobal(L, "PreGame");

	// Modding: OpenSaveData (real); everything else is an auto-stub (metatable set by the prelude).
	lua_newtable(L);
	Register(L, lua_gettop(L), "OpenSaveData", lModdingOpenSaveData, ss);
	lua_setglobal(L, "Modding");

	// Exe enums that Lekmod reads (LekmodUtilities:is_civilization_active uses SlotStatus["SS_TAKEN"]); CvEnums.h values.
	static const char* const slotStatus[] = {"SS_OPEN", "SS_COMPUTER", "SS_CLOSED", "SS_TAKEN", "SS_OBSERVER"};
	static const char* const slotClaim[] = {"SLOTCLAIM_UNASSIGNED", "SLOTCLAIM_RESERVED", "SLOTCLAIM_ASSIGNED"};
	SetEnum(L, "SlotStatus", slotStatus, (int)(sizeof(slotStatus) / sizeof(slotStatus[0])));
	SetEnum(L, "SlotClaim", slotClaim, (int)(sizeof(slotClaim) / sizeof(slotClaim[0])));

	Register(L, LUA_GLOBALSINDEX, "ToHexFromGrid", lToHexFromGrid, ss);
	Register(L, LUA_GLOBALSINDEX, "ToGridFromHex", lToGridFromHex, ss);
	Register(L, LUA_GLOBALSINDEX, "HexToWorld", lHexToWorld, ss);
	Register(L, LUA_GLOBALSINDEX, "HstLoadContext", lLoadContext, ss);
	Register(L, LUA_GLOBALSINDEX, "HstReportError", lReportError, ss);
	Register(L, LUA_GLOBALSINDEX, "HstUiEvent", lUiEvent, ss);

	// Event listener registries, read by the prelude and by DispatchGameEvent / FireEvent.
	static const char* const registries[][2] = {
		{"HstGameEventListeners", "__HstGameEventListeners"},
		{"HstEventsListeners", "__HstEventsListeners"},
		{"HstLuaEventsListeners", "__HstLuaEventsListeners"},
		{"HstEventLog", "__HstEventLog"},
		{"HstFireCounts", "__HstFireCounts"},
		{"HstContextOf", "__HstContextOf"},
	};
	for (size_t i = 0; i < sizeof(registries) / sizeof(registries[0]); ++i)
	{
		lua_newtable(L);
		if (strcmp(registries[i][0], "HstContextOf") == 0)
		{
			lua_newtable(L);
			lua_pushstring(L, "k");
			lua_setfield(L, -2, "__mode");
			lua_setmetatable(L, -2);
		}
		lua_pushvalue(L, -1);
		lua_setfield(L, LUA_REGISTRYINDEX, registries[i][0]);
		lua_setglobal(L, registries[i][1]);
	}

	lua_settop(L, top);
	return RunString(L, kRtLuaPrelude, "=portable prelude");
}

}  // namespace lekmod_portable
