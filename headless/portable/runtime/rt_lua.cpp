// Ported from headless/host/src/script/HstScriptSystem.cpp, HstScriptArgs.cpp and HstVfs.cpp (directory part),
// without the arena allocator, lua.log / events-registered.json files, --strict and the FPU check.
#include "CvGameCoreDLLPCH.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>

#include "rt_events.h"
#include "rt_lua.h"
#include "rt_ui_replica.h"
#include "lekmod_portable/host_log.h"

namespace lekmod_portable {

// ---- RtScriptArgs ----------------------------------------------------------------------------------------------------
void RtScriptArgs::Reset(unsigned int reserve)
{
	m_count = 0;
	if (m_values.size() < reserve) m_values.resize(reserve);
}

RtScriptArgs::Value& RtScriptArgs::Add(ArgType type)
{
	if (m_count >= m_values.size()) m_values.resize(m_count + 8);
	Value& v = m_values[m_count++];
	v.type = type;
	return v;
}

unsigned int RtScriptArgs::Push(bool value) { Add(ARGTYPE_BOOL).b = value; return m_count; }

unsigned int RtScriptArgs::Push(const char* value, size_t len)
{
	Value& v = Add(ARGTYPE_STRING);
	if (value) v.s.assign(value, len); else v.s.clear();
	return m_count;
}

unsigned int RtScriptArgs::Push(int value) { Add(ARGTYPE_INT).i = value; return m_count; }
unsigned int RtScriptArgs::Push(float value) { Add(ARGTYPE_FLOAT).f = value; return m_count; }
unsigned int RtScriptArgs::PushNULL() { Add(ARGTYPE_NULL); return m_count; }
unsigned int RtScriptArgs::Count() { return m_count; }

ICvEngineScriptSystemArgs1::ArgType RtScriptArgs::GetType(unsigned int index) { return index < m_count ? m_values[index].type : ARGTYPE_NONE; }

bool RtScriptArgs::GetBool(unsigned int index, bool& value)
{
	if (index >= m_count || m_values[index].type != ARGTYPE_BOOL) return false;
	value = m_values[index].b;
	return true;
}

bool RtScriptArgs::GetFloat(unsigned int index, float& value)
{
	if (index >= m_count || m_values[index].type != ARGTYPE_FLOAT) return false;
	value = m_values[index].f;
	return true;
}

bool RtScriptArgs::GetInt(unsigned int index, int& value)
{
	if (index >= m_count || m_values[index].type != ARGTYPE_INT) return false;
	value = m_values[index].i;
	return true;
}

bool RtScriptArgs::GetString(unsigned int index, char*& value)
{
	if (index >= m_count || m_values[index].type != ARGTYPE_STRING) return false;
	value = const_cast<char*>(m_values[index].s.c_str());
	return true;
}

int RtScriptArgs::PushToLua(lua_State* L) const
{
	for (unsigned int i = 0; i < m_count; ++i)
	{
		const Value& v = m_values[i];
		switch (v.type)
		{
		case ARGTYPE_BOOL: lua_pushboolean(L, v.b); break;
		case ARGTYPE_STRING: lua_pushlstring(L, v.s.c_str(), v.s.size()); break;
		case ARGTYPE_INT: lua_pushinteger(L, v.i); break;
		case ARGTYPE_FLOAT: lua_pushnumber(L, v.f); break;
		default: lua_pushnil(L); break;
		}
	}
	return (int)m_count;
}

// ---- RtDirSource -----------------------------------------------------------------------------------------------------
namespace {

std::string LowerA(const std::string& s)
{
	std::string out(s);
	for (size_t i = 0; i < out.size(); ++i) out[i] = (char)tolower((unsigned char)out[i]);
	return out;
}

std::string JoinPath(const std::string& a, const std::string& b)
{
	if (a.empty()) return b;
	if (a[a.size() - 1] == '/') return a + b;
	return a + "/" + b;
}

bool IsDirectory(const std::string& path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool IsFile(const std::string& path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

}  // namespace

void RtDirSource::AddRoot(const std::string& dir, bool recursive)
{
	Root r;
	r.dir = dir;
	r.recursive = recursive;
	r.indexed = false;
	m_roots.push_back(r);
}

void RtDirSource::PushFrontIncludeRoot(const std::string& path)
{
	size_t slash = path.find_last_of("\\/");
	std::string dir = slash == std::string::npos ? std::string(".") : path.substr(0, slash);
	for (size_t i = 0; i < m_roots.size(); ++i)
		if (m_roots[i].dir == dir) return;
	Root r;
	r.dir = dir;
	r.recursive = false;
	r.indexed = false;
	m_roots.insert(m_roots.begin(), r);
}

void RtDirSource::IndexDir(Root& root, const std::string& dir)
{
	DIR* d = opendir(dir.c_str());
	if (!d) return;
	std::vector<std::string> names;
	while (struct dirent* e = readdir(d)) names.push_back(e->d_name);
	closedir(d);
	std::sort(names.begin(), names.end());
	std::vector<std::string> subDirs;
	for (size_t i = 0; i < names.size(); ++i)
	{
		const std::string& name = names[i];
		if (name == "." || name == "..") continue;
		std::string full = JoinPath(dir, name);
		if (IsDirectory(full))
		{
			if (root.recursive) subDirs.push_back(full);
			continue;
		}
		std::string lower = LowerA(name);
		size_t dot = lower.find_last_of('.');
		if (dot == std::string::npos || lower.substr(dot) != ".lua") continue;
		std::string stem = lower.substr(0, dot);
		if (root.stems.find(stem) == root.stems.end()) root.stems[stem] = full;
	}
	// Files of a folder win over its subfolders.
	for (size_t i = 0; i < subDirs.size(); ++i) IndexDir(root, subDirs[i]);
}

bool RtDirSource::ResolveInclude(const char* name, std::string& path)
{
	std::string stem = LowerA(name ? name : "");
	size_t slash = stem.find_last_of("\\/");
	if (slash != std::string::npos) stem = stem.substr(slash + 1);
	if (stem.size() > 4 && stem.substr(stem.size() - 4) == ".lua") stem = stem.substr(0, stem.size() - 4);
	for (size_t i = 0; i < m_roots.size(); ++i)
	{
		Root& root = m_roots[i];
		if (!root.indexed)
		{
			root.indexed = true;
			IndexDir(root, root.dir);
		}
		std::map<std::string, std::string>::const_iterator it = root.stems.find(stem);
		if (it != root.stems.end())
		{
			path = it->second;
			return true;
		}
	}
	return false;
}

bool RtDirSource::ResolvePath(const char* name, std::string& path)
{
	if (!name || !*name) return false;
	std::string p(name);
	for (size_t i = 0; i < p.size(); ++i)
		if (p[i] == '\\') p[i] = '/';
	if (IsFile(p))
	{
		path = p;
		return true;
	}
	for (size_t i = 0; i < m_roots.size(); ++i)
	{
		std::string full = JoinPath(m_roots[i].dir, p);
		if (IsFile(full))
		{
			path = full;
			return true;
		}
	}
	return false;
}

bool RtDirSource::ReadSource(const std::string& path, std::string& out)
{
	FILE* f = fopen(path.c_str(), "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	out.resize(size > 0 ? (size_t)size : 0);
	size_t got = size > 0 ? fread(&out[0], 1, (size_t)size, f) : 0;
	fclose(f);
	out.resize(got);
	// Lua 5.1 does not accept a UTF-8 BOM.
	if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF) out.erase(0, 3);
	return true;
}

// ---- RtScriptSystem --------------------------------------------------------------------------------------------------
namespace {

int LuaTraceback(lua_State* L)
{
	if (!lua_isstring(L, 1)) return 1;
	lua_getfield(L, LUA_REGISTRYINDEX, "HstDebugTraceback");
	if (!lua_isfunction(L, -1))
	{
		lua_pop(L, 1);
		return 1;
	}
	lua_pushvalue(L, 1);
	lua_pushinteger(L, 2);
	lua_call(L, 2, 1);
	return 1;
}

int LuaPanic(lua_State* L)
{
	const char* s = lua_tostring(L, -1);
	Log(LOG_ERROR, "lua: PANIC (unprotected error): %s", s ? s : "?");
	return 0;
}

}  // namespace

RtScriptSystem::RtScriptSystem()
	: m_saveData(NULL), m_master(NULL), m_source(NULL), m_db(NULL), m_errors(0), m_events(NULL), m_uiRep(NULL), m_listenersRef(LUA_NOREF), m_capture(false),
	  m_randState(1), m_randCalls(0), m_virtualClock(0.0)
{
}

RtScriptSystem::~RtScriptSystem() { Shutdown(); }

void RtScriptSystem::Shutdown()
{
	CloseSaveData();
	m_contexts.clear();
	m_threadRefs.clear();
	m_threadNames.clear();
	if (m_master)
	{
		lua_close(m_master);
		m_master = NULL;
	}
	for (size_t i = 0; i < m_freeArgs.size(); ++i) delete m_freeArgs[i];
	m_freeArgs.clear();
}

bool RtScriptSystem::Init(RtLuaSource* source, Database::Connection* db, ICvScriptSystemUtility1* libs)
{
	Shutdown();
	m_source = source;
	m_db = db;
	m_errors = 0;
	ResetGameState();

	m_master = luaL_newstate();
	if (!m_master) return false;
	lua_atpanic(m_master, LuaPanic);
	luaL_openlibs(m_master);
	m_threadNames[m_master] = "Master";

	// Keep debug.traceback for error reports even if the sandbox hides 'debug' later.
	lua_getglobal(m_master, "debug");
	lua_getfield(m_master, -1, "traceback");
	lua_setfield(m_master, LUA_REGISTRYINDEX, "HstDebugTraceback");
	lua_pop(m_master, 1);

	// Sandbox, exe globals (include/print/GameDefines/PreGame/Locale/...), event tables, prelude.
	if (!RegisterExeGlobals(m_master)) return false;
	lua_getfield(m_master, LUA_REGISTRYINDEX, "HstGameEventListeners");
	if (!lua_istable(m_master, -1))
	{
		Log(LOG_ERROR, "lua: prelude did not create the GameEvents listener registry");
		return false;
	}
	m_listenersRef = luaL_ref(m_master, LUA_REGISTRYINDEX);

	// The DLL's libraries (GameInfoTypes, enums, Map, Game, Players, Teams, Fractal, ...), after the exe globals.
	if (libs) libs->RegisterScriptLibraries(m_master);
	lua_settop(m_master, 0);
	return true;
}

void RtScriptSystem::ResetGameState()
{
	m_randState = 1;
	m_randCalls = 0;
	m_virtualClock = 0.0;
}

const char* RtScriptSystem::GetThreadName(lua_State* L) const
{
	std::map<lua_State*, std::string>::const_iterator it = m_threadNames.find(L);
	return it != m_threadNames.end() ? it->second.c_str() : "?";
}

void RtScriptSystem::LogLua(lua_State* L, const char* text)
{
	Log(LOG_INFO, "lua[%s]: %s", GetThreadName(L), text);
	if (m_capture) m_printed.push_back(text);
}

void RtScriptSystem::ReportError(const char* where, const char* message)
{
	++m_errors;
	Log(LOG_ERROR, "lua: ERROR in %s: %s", where ? where : "?", message ? message : "?");
	if (m_events)
	{
		const std::string text = std::string(where ? where : "?") + ": " + (message ? message : "?");
		m_events->Push(GC_EVENT_LUA_ERROR, 0, 0, 0, 0, 0, 0, 0, text.c_str());
	}
}

bool RtScriptSystem::ProtectedCall(lua_State* L, int nArgs, int nRet, const char* where)
{
	const int base = lua_gettop(L) - nArgs;  // index of the function
	lua_pushcfunction(L, LuaTraceback);
	lua_insert(L, base);
	const int result = lua_pcall(L, nArgs, nRet, base);
	lua_remove(L, base);
	if (result != 0)
	{
		ReportError(where, lua_tostring(L, -1));
		lua_pop(L, 1);
		for (int i = 0; i < nRet; ++i) lua_pushnil(L);
		return false;
	}
	return true;
}

bool RtScriptSystem::RunString(lua_State* L, const char* source, const char* chunkName)
{
	if (luaL_loadbuffer(L, source, strlen(source), chunkName) != 0)
	{
		ReportError(chunkName, lua_tostring(L, -1));
		lua_pop(L, 1);
		return false;
	}
	return ProtectedCall(L, 0, 0, chunkName);
}

bool RtScriptSystem::RunFile(lua_State* L, const std::string& path, const char* chunkName)
{
	std::string source;
	if (!m_source || !m_source->ReadSource(path, source))
	{
		ReportError(chunkName, "cannot read file");
		return false;
	}
	std::string chunk = std::string("@") + chunkName;
	if (luaL_loadbuffer(L, source.data(), source.size(), chunk.c_str()) != 0)
	{
		ReportError(chunkName, lua_tostring(L, -1));
		lua_pop(L, 1);
		return false;
	}
	return ProtectedCall(L, 0, 0, chunkName);
}

namespace {
std::string BaseName(const std::string& path)
{
	size_t slash = path.find_last_of("\\/");
	return slash == std::string::npos ? path : path.substr(slash + 1);
}
}  // namespace

// ---- threads ---------------------------------------------------------------------------------------------------------
// A fresh globals table per thread whose metatable __index is the master _G (CvMapGenerator.cpp:35).
lua_State* RtScriptSystem::CreateLuaThread(const char* szName) { return NewThread(szName); }

lua_State* RtScriptSystem::NewThread(const char* name)
{
	lua_State* T = lua_newthread(m_master);
	const int ref = luaL_ref(m_master, LUA_REGISTRYINDEX);
	lua_newtable(T);                      // globals
	lua_newtable(T);                      // metatable
	lua_pushvalue(T, LUA_GLOBALSINDEX);   // master globals (inherited by the new thread)
	lua_setfield(T, -2, "__index");
	lua_setmetatable(T, -2);
	lua_pushvalue(T, -1);
	lua_setfield(T, -2, "_G");
	// __HstContextOf[globals] = name: the event registration log names the context of each listener by its environment.
	lua_getfield(T, LUA_REGISTRYINDEX, "HstContextOf");
	lua_pushvalue(T, -2);
	lua_pushstring(T, name ? name : "");
	lua_rawset(T, -3);
	lua_pop(T, 1);
	lua_replace(T, LUA_GLOBALSINDEX);
	m_threadRefs[T] = ref;
	m_threadNames[T] = name ? name : "";
	return T;
}

void RtScriptSystem::FreeLuaThread(lua_State* p0)
{
	std::map<lua_State*, int>::iterator it = m_threadRefs.find(p0);
	if (it == m_threadRefs.end()) return;
	luaL_unref(m_master, LUA_REGISTRYINDEX, it->second);
	m_threadRefs.erase(it);
	m_threadNames.erase(p0);
}

// Gameplay contexts: the exe loads InGame.lua, whose ContextPtr:LoadNewContext lines name the Lekmod gameplay
// scripts; each context has its own globals (master _G behind __index) and ContextPtr.
lua_State* RtScriptSystem::LoadContext(const char* name)
{
	ContextInfo info;
	info.name = name ? name : "";
	info.loaded = false;
	info.thread = NULL;
	std::string path;
	if (!m_source || !m_source->ResolveInclude(info.name.c_str(), path))
	{
		Log(LOG_WARN, "lua: context '%s': file not found (skipped)", info.name.c_str());
		m_contexts.push_back(info);
		return NULL;
	}
	info.file = path;
	const size_t index = m_contexts.size();
	m_contexts.push_back(info);

	lua_State* T = NewThread(info.name.c_str());
	m_contexts[index].thread = T;
	lua_getfield(T, LUA_GLOBALSINDEX, "HstMakeContextPtr");
	lua_pushstring(T, info.name.c_str());
	if (ProtectedCall(T, 1, 1, "HstMakeContextPtr"))
		lua_setfield(T, LUA_GLOBALSINDEX, "ContextPtr");
	else
		lua_pop(T, 1);
	const int errors = m_errors;
	RunFile(T, path, BaseName(path).c_str());
	lua_settop(T, 0);
	m_contexts[index].loaded = (m_errors == errors);
	return T;
}

int RtScriptSystem::FireEvent(const char* name)
{
	lua_State* L = m_master;
	const int top = lua_gettop(L);
	lua_getfield(L, LUA_REGISTRYINDEX, "HstEventsListeners");
	lua_getfield(L, -1, name);
	const int count = lua_istable(L, -1) ? (int)lua_objlen(L, -1) : 0;
	const int listeners = lua_gettop(L);
	std::string where = std::string("Events.") + name;
	int called = 0;
	for (int i = 1; i <= count; ++i)
	{
		lua_rawgeti(L, listeners, i);
		if (!lua_isfunction(L, -1))
		{
			lua_pop(L, 1);
			continue;
		}
		ProtectedCall(L, 0, 0, where.c_str());
		++called;
	}
	lua_settop(L, top);
	return called;
}

Database::Connection* RtScriptSystem::GetSaveData()
{
	if (m_saveData) return m_saveData;
	// Relative to the cache folder (rt_fs serves it from memory), like the DLL's own CvGame::reset.
	const char* path = ".\\Civ5SavedGameDatabase.db";
	Database::Connection* db = new Database::Connection();
	if (!db->Open(path, Database::OPEN_READWRITE | Database::OPEN_FULLMUTEX))
	{
		Log(LOG_WARN, "lua: Modding.OpenSaveData: cannot open %s: %s", path, db->ErrorMessage() ? db->ErrorMessage() : "?");
		delete db;
		return NULL;
	}
	m_saveData = db;
	return m_saveData;
}

void RtScriptSystem::CloseSaveData()
{
	if (m_saveData)
	{
		m_saveData->Close();
		delete m_saveData;
		m_saveData = NULL;
	}
}

void RtScriptSystem::ResetContexts()
{
	for (size_t i = 0; i < m_contexts.size(); ++i)
		if (m_contexts[i].thread) FreeLuaThread(m_contexts[i].thread);
	m_contexts.clear();
	static const char* const registries[] = {"HstGameEventListeners", "HstEventsListeners", "HstLuaEventsListeners"};
	lua_State* L = m_master;
	for (size_t i = 0; i < sizeof(registries) / sizeof(registries[0]); ++i)
	{
		lua_getfield(L, LUA_REGISTRYINDEX, registries[i]);
		lua_pushnil(L);
		while (lua_next(L, -2) != 0)
		{
			// Clear each listener array in place (the event objects hold references to these tables).
			if (lua_istable(L, -1))
			{
				for (int j = (int)lua_objlen(L, -1); j >= 1; --j)
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

// The chunk is executed: the DLL looks up GenerateMap / GetMapInitData right after (CvMapGenerator.cpp:36).
bool RtScriptSystem::LoadFile(lua_State* L, const char* szFilename)
{
	std::string path;
	// A bare file name (CvWorldBuilderMapLoader: "WorldBuilderRandomItems.lua") is found by name in the include roots, as the engine's
	// file system finds it anywhere in the game data.
	const bool bare = szFilename && !strchr(szFilename, '/') && !strchr(szFilename, '\\');
	if (!m_source || !(m_source->ResolvePath(szFilename, path) || (bare && m_source->ResolveInclude(szFilename, path))))
	{
		ReportError("LoadFile", (std::string("file not found: ") + (szFilename ? szFilename : "")).c_str());
		return false;
	}
	// The script's own folder is searched first by include() (Lekmap ships its HB* libraries next to it).
	m_source->PushFrontIncludeRoot(path);
	Log(LOG_DEBUG, "lua: LoadFile %s -> %s", szFilename, path.c_str());
	return RunFile(L, path, BaseName(path).c_str());
}

ICvEngineScriptSystemArgs1* RtScriptSystem::CreateArgs() { return CreateArgs(8); }

ICvEngineScriptSystemArgs1* RtScriptSystem::CreateArgs(uint uiReserve)
{
	RtScriptArgs* args = NULL;
	if (!m_freeArgs.empty())
	{
		args = m_freeArgs.back();
		m_freeArgs.pop_back();
	}
	else
		args = new RtScriptArgs();
	args->Reset(uiReserve);
	return args;
}

void RtScriptSystem::DestroyArgs(ICvEngineScriptSystemArgs1* pkArgs)
{
	if (pkArgs) m_freeArgs.push_back(static_cast<RtScriptArgs*>(pkArgs));
}

bool RtScriptSystem::CallCFunction(lua_State* L, lua_CFunction func, void* args)
{
	if (lua_cpcall(L, func, args) != 0)
	{
		ReportError("CallCFunction", lua_tostring(L, -1));
		lua_pop(L, 1);
		return false;
	}
	return true;
}

bool RtScriptSystem::CallFunction(lua_State* L, unsigned int nArgs, unsigned int nRet)
{
	return ProtectedCall(L, (int)nArgs, (int)nRet, GetThreadName(L));
}

// ---- GameEvents dispatch ---------------------------------------------------------------------------------------------
// Call* value semantics are GUESSes in civhost (R4): CallHook: OR of truthy returns; CallTestAll: AND (nil counts as
// false); CallTestAny: OR; CallAccumulator: sum. The out value is always assigned; the return value is "had listeners".
// The core's listeners, then (B4 M2 WO-1, a replica whose UI subscribed to the name) the same call for the UI: its results do not count (SIMP-exe-2).
bool RtScriptSystem::DispatchGameEvent(const char* name, ICvEngineScriptSystemArgs1* args, HookMode mode, bool& value, double& sum)
{
	const bool had = DispatchGameEventCore(name, args, mode, value, sum);
	if (m_uiRep && m_uiRep->Active()) m_uiRep->EmitGameEvent(name, static_cast<RtScriptArgs*>(args));
	return had;
}

bool RtScriptSystem::DispatchGameEventCore(const char* name, ICvEngineScriptSystemArgs1* args, HookMode mode, bool& value, double& sum)
{
	if (!m_master) return false;
	lua_State* L = m_master;
	const int top = lua_gettop(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, m_listenersRef);
	lua_getfield(L, -1, name);
	const int count = lua_istable(L, -1) ? (int)lua_objlen(L, -1) : 0;
	if (count == 0)
	{
		lua_settop(L, top);
		return false;
	}
	const int listeners = lua_gettop(L);
	RtScriptArgs* a = static_cast<RtScriptArgs*>(args);
	for (int i = 1; i <= count; ++i)
	{
		lua_rawgeti(L, listeners, i);
		if (!lua_isfunction(L, -1))
		{
			lua_pop(L, 1);
			continue;
		}
		const int nArgs = a ? a->PushToLua(L) : 0;
		if (!ProtectedCall(L, nArgs, 1, name))
		{
			lua_pop(L, 1);
			continue;
		}
		switch (mode)
		{
		case HOOK_OR:
		case HOOK_ANY:
			if (lua_toboolean(L, -1)) value = true;
			break;
		case HOOK_AND:
			if (!lua_toboolean(L, -1)) value = false;
			break;
		case HOOK_SUM_INT:
		case HOOK_SUM_FLOAT:
			sum += lua_tonumber(L, -1);
			break;
		}
		lua_pop(L, 1);
	}
	lua_settop(L, top);
	return true;
}

bool RtScriptSystem::CallHook(const char* szName, ICvEngineScriptSystemArgs1* args, bool& value)
{
	value = false;
	double sum = 0.0;
	return DispatchGameEvent(szName, args, HOOK_OR, value, sum);
}

bool RtScriptSystem::CallAccumulator(const char* szName, ICvEngineScriptSystemArgs1* args, int& value)
{
	bool dummy = false;
	double sum = 0.0;
	const bool had = DispatchGameEvent(szName, args, HOOK_SUM_INT, dummy, sum);
	value = (int)sum;
	return had;
}

bool RtScriptSystem::CallAccumulator(const char* szName, ICvEngineScriptSystemArgs1* args, float& value)
{
	bool dummy = false;
	double sum = 0.0;
	const bool had = DispatchGameEvent(szName, args, HOOK_SUM_FLOAT, dummy, sum);
	value = (float)sum;
	return had;
}

bool RtScriptSystem::CallTestAll(const char* szName, ICvEngineScriptSystemArgs1* args, bool& value)
{
	value = true;
	double sum = 0.0;
	return DispatchGameEvent(szName, args, HOOK_AND, value, sum);
}

bool RtScriptSystem::CallTestAny(const char* szName, ICvEngineScriptSystemArgs1* args, bool& value)
{
	value = false;
	double sum = 0.0;
	return DispatchGameEvent(szName, args, HOOK_ANY, value, sum);
}

}  // namespace lekmod_portable
