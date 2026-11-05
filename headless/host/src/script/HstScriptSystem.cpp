//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstScriptSystem.cpp
//!  \brief     Script system for the DLL (design 3.1). Call* value semantics are GUESSes (R4):
//!				CallHook: OR of truthy returns; CallTestAll: AND (nil counts as false); CallTestAny: OR;
//!				CallAccumulator: sum. The out value is always assigned; the return value is "had listeners".
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#include "HstCommon.h"
#include "script/HstScriptSystem.h"
#include "script/HstScriptArgs.h"
#include "script/HstArenaAlloc.h"
#include "script/HstVfs.h"
#include "script/HstExeLua.h"
#include "platform/HstPlatform.h"
#include "platform/HstDetHeap.h"

static int HstLuaTraceback(lua_State* L)
{
	if(!lua_isstring(L, 1))
		return 1;
	lua_getfield(L, LUA_REGISTRYINDEX, "HstDebugTraceback");
	if(!lua_isfunction(L, -1))
	{
		lua_pop(L, 1);
		return 1;
	}
	lua_pushvalue(L, 1);
	lua_pushinteger(L, 2);
	lua_call(L, 2, 1);
	return 1;
}

static int HstLuaPanic(lua_State* L)
{
	const char* sz = lua_tostring(L, -1);
	HstFatal(3, "lua: PANIC (unprotected error): %s", sz ? sz : "?");
	return 0;
}

HstScriptSystem::HstScriptSystem()
	: m_pkMaster(NULL)
	, m_pkVfs(NULL)
	, m_pkDb(NULL)
	, m_pkCtx(NULL)
	, m_bStrict(false)
	, m_iErrors(0)
	, m_iListenersRef(LUA_NOREF)
	, m_pkLuaLog(NULL)
	, m_pkSaveData(NULL)
{
}

bool HstScriptSystem::Init(HstArenaAlloc* pkArena, HstVfs* pkVfs, Database::Connection* pkDb, ICvGameContext1* pkCtx, bool bStrict)
{
	m_pkVfs = pkVfs;
	m_pkDb = pkDb;
	m_pkCtx = pkCtx;
	m_bStrict = bStrict;
	{
		HstDetHeapPauseScope kPause;	// run-dir path: keep it out of the deterministic heap
		m_pkLuaLog = _wfopen(HstJoinPath(HstGetRunDir(), L"lua.log").c_str(), L"w");
	}

	m_pkMaster = pkArena ? lua_newstate(HstArenaAlloc::LuaAlloc, pkArena) : luaL_newstate();
	if(!m_pkMaster)
		return false;
	lua_atpanic(m_pkMaster, HstLuaPanic);
	luaL_openlibs(m_pkMaster);
	m_kThreadNames[m_pkMaster] = "Master";

	// Keep debug.traceback for error reports even if the sandbox hides 'debug' later.
	lua_getglobal(m_pkMaster, "debug");
	lua_getfield(m_pkMaster, -1, "traceback");
	lua_setfield(m_pkMaster, LUA_REGISTRYINDEX, "HstDebugTraceback");
	lua_pop(m_pkMaster, 1);

	// Sandbox, exe globals (include/print/DB/GameInfo/GameDefines/PreGame/Locale/...), event tables.
	if(!HstExeLuaRegister(this, m_pkMaster))
		return false;
	lua_getfield(m_pkMaster, LUA_REGISTRYINDEX, "HstGameEventListeners");
	if(!lua_istable(m_pkMaster, -1))
	{
		HstLog("lua: prelude did not create the GameEvents listener registry");
		return false;
	}
	m_iListenersRef = luaL_ref(m_pkMaster, LUA_REGISTRYINDEX);

	// 1.5: the DLL's libraries (GameInfoTypes, enums, Map, Game, Players, Teams, Fractal, ...). Not a per-call wrapper.
	ICvScriptSystemUtility1* pkUtility = pkCtx->GetScriptSystemUtility();
	pkUtility->RegisterScriptLibraries(m_pkMaster);
	lua_settop(m_pkMaster, 0);
	return true;
}

const char* HstScriptSystem::GetThreadName(lua_State* L) const
{
	std::map<lua_State*, std::string>::const_iterator it = m_kThreadNames.find(L);
	return it != m_kThreadNames.end() ? it->second.c_str() : "?";
}

void HstScriptSystem::LogLua(lua_State* L, const char* szText)
{
	if(m_pkLuaLog)
	{
		fprintf(m_pkLuaLog, "%s: %s\n", GetThreadName(L), szText);
		fflush(m_pkLuaLog);
	}
}

void HstScriptSystem::ReportError(const char* szWhere, const char* szMessage)
{
	++m_iErrors;
	HstLog("lua: ERROR in %s: %s", szWhere, szMessage ? szMessage : "?");
	if(m_pkLuaLog)
	{
		fprintf(m_pkLuaLog, "Runtime Error (%s): %s\n", szWhere, szMessage ? szMessage : "?");
		fflush(m_pkLuaLog);
	}
	if(m_bStrict)
		HstFatal(6, "lua: FATAL (--strict) Lua error");
}

bool HstScriptSystem::ProtectedCall(lua_State* L, int nArgs, int nRet, const char* szWhere)
{
	const int iBase = lua_gettop(L) - nArgs;	// index of the function
	lua_pushcfunction(L, HstLuaTraceback);
	lua_insert(L, iBase);
	const int iResult = lua_pcall(L, nArgs, nRet, iBase);
	HST_FPU_CHECK(szWhere);	// --fpu-check: did Lua (or the game code it called) change the x87 control word?
	lua_remove(L, iBase);
	if(iResult != 0)
	{
		ReportError(szWhere, lua_tostring(L, -1));
		lua_pop(L, 1);
		for(int i = 0; i < nRet; ++i)
			lua_pushnil(L);
		return false;
	}
	return true;
}

bool HstScriptSystem::RunString(lua_State* L, const char* szSource, const char* szChunkName)
{
	if(luaL_loadbuffer(L, szSource, strlen(szSource), szChunkName) != 0)
	{
		ReportError(szChunkName, lua_tostring(L, -1));
		lua_pop(L, 1);
		return false;
	}
	return ProtectedCall(L, 0, 0, szChunkName);
}

bool HstScriptSystem::RunFile(lua_State* L, const std::wstring& wstrPath, const char* szChunkName)
{
	std::string strSource;
	if(!HstVfs::ReadFile(wstrPath, strSource))
	{
		ReportError(szChunkName, "cannot read file");
		return false;
	}
	std::string strChunk = std::string("@") + szChunkName;
	if(luaL_loadbuffer(L, strSource.data(), strSource.size(), strChunk.c_str()) != 0)
	{
		ReportError(szChunkName, lua_tostring(L, -1));
		lua_pop(L, 1);
		return false;
	}
	return ProtectedCall(L, 0, 0, szChunkName);
}

// ---------------------------------------------------------------------------------------------------------------------
// Threads: a fresh globals table per thread whose metatable __index is the master _G (CvMapGenerator.cpp:35).
lua_State* HstScriptSystem::CreateLuaThread(const char* szName)
{
	HST_API(kHstApi_ScriptSystem_CreateLuaThread);
	return NewThread(szName);
}

lua_State* HstScriptSystem::NewThread(const char* szName)
{
	lua_State* T = lua_newthread(m_pkMaster);
	const int iRef = luaL_ref(m_pkMaster, LUA_REGISTRYINDEX);
	lua_newtable(T);					// globals
	lua_newtable(T);					// metatable
	lua_pushvalue(T, LUA_GLOBALSINDEX);	// master globals (inherited by the new thread)
	lua_setfield(T, -2, "__index");
	lua_setmetatable(T, -2);
	lua_pushvalue(T, -1);
	lua_setfield(T, -2, "_G");
	// __HstContextOf[globals] = name: the event registration log names the context of each listener by its environment.
	lua_getfield(T, LUA_REGISTRYINDEX, "HstContextOf");
	lua_pushvalue(T, -2);
	lua_pushstring(T, szName ? szName : "");
	lua_rawset(T, -3);
	lua_pop(T, 1);
	lua_replace(T, LUA_GLOBALSINDEX);
	m_kThreadRefs[T] = iRef;
	m_kThreadNames[T] = szName ? szName : "";
	return T;
}

// ---------------------------------------------------------------------------------------------------------------------
// Gameplay contexts (design 3.4). The exe loads InGame.lua, whose ContextPtr:LoadNewContext lines name the Lekmod
// gameplay scripts; each context has its own globals (master _G behind __index) and ContextPtr.
lua_State* HstScriptSystem::LoadContext(const char* szName)
{
	ContextInfo kInfo;
	kInfo.strName = szName ? szName : "";
	kInfo.bLoaded = false;
	kInfo.pkThread = NULL;
	std::wstring wstrPath;
	if(!m_pkVfs->ResolveInclude(kInfo.strName.c_str(), wstrPath))
	{
		HstLog("lua: context '%s': file not found (skipped)", kInfo.strName.c_str());
		m_akContexts.push_back(kInfo);
		return NULL;
	}
	kInfo.strFile = HstToUtf8(wstrPath);
	const size_t uiIndex = m_akContexts.size();
	m_akContexts.push_back(kInfo);

	lua_State* T = NewThread(kInfo.strName.c_str());
	m_akContexts[uiIndex].pkThread = T;
	lua_getfield(T, LUA_GLOBALSINDEX, "HstMakeContextPtr");
	lua_pushstring(T, kInfo.strName.c_str());
	if(ProtectedCall(T, 1, 1, "HstMakeContextPtr"))
		lua_setfield(T, LUA_GLOBALSINDEX, "ContextPtr");
	else
		lua_pop(T, 1);
	const int iErrors = m_iErrors;
	size_t uiSlash = wstrPath.find_last_of(L"\\/");
	std::string strChunk = HstToUtf8(uiSlash == std::wstring::npos ? wstrPath : wstrPath.substr(uiSlash + 1));
	RunFile(T, wstrPath, strChunk.c_str());
	lua_settop(T, 0);
	m_akContexts[uiIndex].bLoaded = (m_iErrors == iErrors);
	return T;
}

int HstScriptSystem::FireEvent(const char* szName)
{
	++m_kHostFires[szName];
	lua_State* L = m_pkMaster;
	const int iTop = lua_gettop(L);
	lua_getfield(L, LUA_REGISTRYINDEX, "HstEventsListeners");
	lua_getfield(L, -1, szName);
	const int iCount = lua_istable(L, -1) ? (int)lua_objlen(L, -1) : 0;
	const int iListeners = lua_gettop(L);
	std::string strWhere = std::string("Events.") + szName;
	int iCalled = 0;
	for(int i = 1; i <= iCount; ++i)
	{
		lua_rawgeti(L, iListeners, i);
		if(!lua_isfunction(L, -1))
		{
			lua_pop(L, 1);
			continue;
		}
		ProtectedCall(L, 0, 0, strWhere.c_str());
		++iCalled;
	}
	lua_settop(L, iTop);
	return iCalled;
}

Database::Connection* HstScriptSystem::GetSaveData()
{
	if(m_pkSaveData)
		return m_pkSaveData;
	// Relative to the cache folder (= CWD = run dir; HstEngineUtility::GetCacheFolderPath is ".\"), like the DLL's own
	// CvGame::reset; a relative name keeps SQLite's heap traffic independent of the run dir (finding 12).
	const std::string strPath = ".\\Civ5SavedGameDatabase.db";
	Database::Connection* pkDb = new Database::Connection();
	if(!pkDb->Open(strPath.c_str(), Database::OPEN_READWRITE | Database::OPEN_FULLMUTEX))
	{
		HstLog("lua: Modding.OpenSaveData: cannot open %s: %s", strPath.c_str(), pkDb->ErrorMessage());
		delete pkDb;
		return NULL;
	}
	m_pkSaveData = pkDb;
	return m_pkSaveData;
}

void HstScriptSystem::CloseSaveData()
{
	if(m_pkSaveData)
	{
		m_pkSaveData->Close();
		delete m_pkSaveData;
		m_pkSaveData = NULL;
	}
}

void HstScriptSystem::ResetContexts()
{
	// Context threads (their globals die with them); the master tables keep only the master's own listeners.
	for(size_t i = 0; i < m_akContexts.size(); ++i)
		if(m_akContexts[i].pkThread)
			FreeLuaThread(m_akContexts[i].pkThread);
	m_akContexts.clear();
	static const char* s_aszListeners[] = { "HstGameEventListeners", "HstEventsListeners", "HstLuaEventsListeners" };
	lua_State* L = m_pkMaster;
	for(int i = 0; i < (int)HST_ARRAY_SIZE(s_aszListeners); ++i)
	{
		lua_getfield(L, LUA_REGISTRYINDEX, s_aszListeners[i]);
		lua_pushnil(L);
		while(lua_next(L, -2) != 0)
		{
			// Clear each listener array in place (the event objects hold references to these tables).
			if(lua_istable(L, -1))
			{
				for(int j = (int)lua_objlen(L, -1); j >= 1; --j)
				{
					lua_pushnil(L);
					lua_rawseti(L, -2, j);
				}
			}
			lua_pop(L, 1);
		}
		lua_pop(L, 1);
	}
	lua_gc(L, LUA_GCCOLLECT, 0);
}

static std::string HstJsonStr(const std::string& str)
{
	std::string strOut = "\"";
	for(size_t i = 0; i < str.size(); ++i)
	{
		const char c = str[i];
		if(c == '\\' || c == '"')
		{
			strOut += '\\';
			strOut += c;
		}
		else if((unsigned char)c < 0x20)
		{
			char szBuf[8];
			_snprintf_s(szBuf, sizeof(szBuf), _TRUNCATE, "\\u%04x", (unsigned int)(unsigned char)c);
			strOut += szBuf;
		}
		else
			strOut += c;
	}
	return strOut + "\"";
}

void HstScriptSystem::WriteEventsRegistered(const std::wstring& wstrPath)
{
	// Collect from Lua first (Lua strings / tables live in the arena; the det heap only sees the host's std containers).
	typedef std::map<std::string, std::vector<std::string> > EventMap;
	std::map<std::string, EventMap> kSets;	// set -> event -> contexts (registration order)
	std::map<std::string, int> kLuaFires;
	lua_State* L = m_pkMaster;
	const int iTop = lua_gettop(L);
	lua_getfield(L, LUA_REGISTRYINDEX, "HstEventLog");
	const int iCount = lua_istable(L, -1) ? (int)lua_objlen(L, -1) : 0;
	for(int i = 1; i <= iCount; ++i)
	{
		lua_rawgeti(L, -1, i);
		lua_rawgeti(L, -1, 1);
		lua_rawgeti(L, -2, 2);
		lua_rawgeti(L, -3, 3);
		const char* szSet = lua_tostring(L, -3);
		const char* szEvent = lua_tostring(L, -2);
		const char* szContext = lua_tostring(L, -1);
		kSets[szSet ? szSet : "?"][szEvent ? szEvent : "?"].push_back(szContext ? szContext : "?");
		lua_pop(L, 4);
	}
	lua_settop(L, iTop);
	lua_getfield(L, LUA_REGISTRYINDEX, "HstFireCounts");
	lua_pushnil(L);
	while(lua_next(L, -2) != 0)
	{
		if(lua_type(L, -2) == LUA_TSTRING)
			kLuaFires[lua_tostring(L, -2)] = (int)lua_tonumber(L, -1);
		lua_pop(L, 1);
	}
	lua_settop(L, iTop);

	HstDetHeapPauseScope kPause;
	FILE* pkFile = _wfopen(wstrPath.c_str(), L"w");
	if(!pkFile)
		return;
	fprintf(pkFile, "{\n  \"contexts\": [\n");
	for(size_t i = 0; i < m_akContexts.size(); ++i)
		fprintf(pkFile, "    {\"name\": %s, \"file\": %s, \"loaded\": %s}%s\n", HstJsonStr(m_akContexts[i].strName).c_str(),
			m_akContexts[i].strFile.empty() ? "null" : HstJsonStr(m_akContexts[i].strFile).c_str(), m_akContexts[i].bLoaded ? "true" : "false",
			i + 1 < m_akContexts.size() ? "," : "");
	fprintf(pkFile, "  ],\n");
	static const char* s_aszSets[] = { "Events", "GameEvents", "LuaEvents" };
	for(int s = 0; s < (int)HST_ARRAY_SIZE(s_aszSets); ++s)
	{
		fprintf(pkFile, "  \"%s\": {", s_aszSets[s]);
		const EventMap& kEvents = kSets[s_aszSets[s]];
		bool bFirst = true;
		for(EventMap::const_iterator it = kEvents.begin(); it != kEvents.end(); ++it)
		{
			fprintf(pkFile, "%s\n    %s: [", bFirst ? "" : ",", HstJsonStr(it->first).c_str());
			for(size_t j = 0; j < it->second.size(); ++j)
				fprintf(pkFile, "%s%s", j ? ", " : "", HstJsonStr(it->second[j]).c_str());
			fprintf(pkFile, "]");
			bFirst = false;
		}
		fprintf(pkFile, "%s},\n", bFirst ? "" : "\n  ");
	}
	fprintf(pkFile, "  \"fired_by_host\": {");
	bool bFirst = true;
	for(std::map<std::string, int>::const_iterator it = m_kHostFires.begin(); it != m_kHostFires.end(); ++it, bFirst = false)
		fprintf(pkFile, "%s%s: %d", bFirst ? "" : ", ", HstJsonStr("Events." + it->first).c_str(), it->second);
	fprintf(pkFile, "},\n  \"fired_from_lua\": {");
	bFirst = true;
	for(std::map<std::string, int>::const_iterator it = kLuaFires.begin(); it != kLuaFires.end(); ++it, bFirst = false)
		fprintf(pkFile, "%s%s: %d", bFirst ? "" : ", ", HstJsonStr(it->first).c_str(), it->second);
	fprintf(pkFile, "}\n}\n");
	fclose(pkFile);
}

void HstScriptSystem::FreeLuaThread(lua_State* p0)
{
	HST_API(kHstApi_ScriptSystem_FreeLuaThread);
	std::map<lua_State*, int>::iterator it = m_kThreadRefs.find(p0);
	if(it == m_kThreadRefs.end())
		return;
	luaL_unref(m_pkMaster, LUA_REGISTRYINDEX, it->second);
	m_kThreadRefs.erase(it);
	m_kThreadNames.erase(p0);
}

// The chunk is executed: the DLL looks up GenerateMap / GetMapInitData right after (CvMapGenerator.cpp:36).
bool HstScriptSystem::LoadFile(lua_State* L, const char* szFilename)
{
	HST_API(kHstApi_ScriptSystem_LoadFile);
	std::wstring wstrPath;
	if(!m_pkVfs->ResolvePath(szFilename, wstrPath))
	{
		ReportError("LoadFile", (std::string("file not found: ") + (szFilename ? szFilename : "")).c_str());
		return false;
	}
	// The script's own folder is searched first by include() (Lekmap ships its HB* libraries next to it).
	size_t uiSlash = wstrPath.find_last_of(L"\\/");
	if(uiSlash != std::wstring::npos)
		m_pkVfs->PushFrontIncludeRoot(wstrPath.substr(0, uiSlash), false);
	HstLog("lua: LoadFile %s -> %s", szFilename, HstToUtf8(wstrPath).c_str());
	std::string strName = HstToUtf8(wstrPath.substr(uiSlash == std::wstring::npos ? 0 : uiSlash + 1));
	return RunFile(L, wstrPath, strName.c_str());
}

ICvEngineScriptSystemArgs1* HstScriptSystem::CreateArgs()
{
	HST_API(kHstApi_ScriptSystem_CreateArgs_1);
	return CreateArgs(8);
}

ICvEngineScriptSystemArgs1* HstScriptSystem::CreateArgs(uint uiReserve)
{
	HST_API(kHstApi_ScriptSystem_CreateArgs_2);
	HstScriptArgs* pkArgs = NULL;
	if(!m_apFreeArgs.empty())
	{
		pkArgs = m_apFreeArgs.back();
		m_apFreeArgs.pop_back();
	}
	else
		pkArgs = new HstScriptArgs();
	pkArgs->Reset(uiReserve);
	return pkArgs;
}

void HstScriptSystem::DestroyArgs(ICvEngineScriptSystemArgs1* pkArgs)
{
	HST_API(kHstApi_ScriptSystem_DestroyArgs);
	if(pkArgs)
		m_apFreeArgs.push_back(static_cast<HstScriptArgs*>(pkArgs));
}

bool HstScriptSystem::CallCFunction(lua_State* L, lua_CFunction func, void* args)
{
	HST_API(kHstApi_ScriptSystem_CallCFunction);
	const int iResult = lua_cpcall(L, func, args);
	HST_FPU_CHECK("CallCFunction");
	if(iResult != 0)
	{
		ReportError("CallCFunction", lua_tostring(L, -1));
		lua_pop(L, 1);
		return false;
	}
	return true;
}

bool HstScriptSystem::CallFunction(lua_State* L, unsigned int nArgs, unsigned int nRet)
{
	HST_API(kHstApi_ScriptSystem_CallFunction);
	return ProtectedCall(L, (int)nArgs, (int)nRet, GetThreadName(L));
}

// ---------------------------------------------------------------------------------------------------------------------
bool HstScriptSystem::DispatchGameEvent(const char* szName, ICvEngineScriptSystemArgs1* pkArgs, HookMode eMode, bool& bValue, double& dSum)
{
	lua_State* L = m_pkMaster;
	const int iTop = lua_gettop(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, m_iListenersRef);
	lua_getfield(L, -1, szName);
	const int iCount = lua_istable(L, -1) ? (int)lua_objlen(L, -1) : 0;
	if(iCount == 0)
	{
		lua_settop(L, iTop);
		return false;
	}
	const int iListeners = lua_gettop(L);
	HstScriptArgs* pkHstArgs = static_cast<HstScriptArgs*>(pkArgs);
	for(int i = 1; i <= iCount; ++i)
	{
		lua_rawgeti(L, iListeners, i);
		if(!lua_isfunction(L, -1))
		{
			lua_pop(L, 1);
			continue;
		}
		const int nArgs = pkHstArgs ? pkHstArgs->PushToLua(L) : 0;
		if(!ProtectedCall(L, nArgs, 1, szName))
		{
			lua_pop(L, 1);
			continue;
		}
		switch(eMode)
		{
		case HOOK_OR:
		case HOOK_ANY:
			if(lua_toboolean(L, -1))
				bValue = true;
			break;
		case HOOK_AND:
			if(!lua_toboolean(L, -1))
				bValue = false;
			break;
		case HOOK_SUM_INT:
		case HOOK_SUM_FLOAT:
			dSum += lua_tonumber(L, -1);
			break;
		}
		lua_pop(L, 1);
	}
	lua_settop(L, iTop);
	return true;
}

bool HstScriptSystem::CallHook(const char* szName, ICvEngineScriptSystemArgs1* args, bool& value)
{
	HST_API(kHstApi_ScriptSystem_CallHook);
	value = false;
	double dSum = 0.0;
	return DispatchGameEvent(szName, args, HOOK_OR, value, dSum);
}

bool HstScriptSystem::CallAccumulator(const char* szName, ICvEngineScriptSystemArgs1* args, int& value)
{
	HST_API(kHstApi_ScriptSystem_CallAccumulator_1);
	bool bDummy = false;
	double dSum = 0.0;
	const bool bHad = DispatchGameEvent(szName, args, HOOK_SUM_INT, bDummy, dSum);
	value = (int)dSum;
	return bHad;
}

bool HstScriptSystem::CallAccumulator(const char* szName, ICvEngineScriptSystemArgs1* args, float& value)
{
	HST_API(kHstApi_ScriptSystem_CallAccumulator_2);
	bool bDummy = false;
	double dSum = 0.0;
	const bool bHad = DispatchGameEvent(szName, args, HOOK_SUM_FLOAT, bDummy, dSum);
	value = (float)dSum;
	return bHad;
}

bool HstScriptSystem::CallTestAll(const char* szName, ICvEngineScriptSystemArgs1* args, bool& value)
{
	HST_API(kHstApi_ScriptSystem_CallTestAll);
	value = true;
	double dSum = 0.0;
	return DispatchGameEvent(szName, args, HOOK_AND, value, dSum);
}

bool HstScriptSystem::CallTestAny(const char* szName, ICvEngineScriptSystemArgs1* args, bool& value)
{
	HST_API(kHstApi_ScriptSystem_CallTestAny);
	value = false;
	double dSum = 0.0;
	return DispatchGameEvent(szName, args, HOOK_ANY, value, dSum);
}
