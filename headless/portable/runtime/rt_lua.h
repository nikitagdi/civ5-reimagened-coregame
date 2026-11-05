// rt_lua: ICvEngineScriptSystem1 on Lua 5.1 and the exe-provided Lua globals (rt_lua).
// Ported from headless/host/src/script/{HstScriptSystem,HstScriptArgs,HstExeLua}.cpp without the arena allocator,
// file logs, Network bindings and UI-bot bindings. GameInfo / DB.Query are task A5 (rt_lua_gameinfo.cpp).
#pragma once

#include <stddef.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "../host/gen/PortableEngineScriptSystem1Gen.h"
#include "CvGameDatabase.h"

namespace lekmod_portable {

class RtEventQueue;
class RtUiReplica;

// Hook argument container (HstScriptArgs).
class RtScriptArgs : public ICvEngineScriptSystemArgs1
{
public:
	RtScriptArgs() : m_count(0) {}
	virtual ~RtScriptArgs() {}

	virtual unsigned int Push(bool value);
	virtual unsigned int Push(const char* value, size_t len);
	virtual unsigned int Push(int value);
	virtual unsigned int Push(float value);
	virtual unsigned int PushNULL();

	virtual unsigned int Count();
	virtual ArgType GetType(unsigned int index);

	virtual bool GetBool(unsigned int index, bool& value);
	virtual bool GetFloat(unsigned int index, float& value);
	virtual bool GetInt(unsigned int index, int& value);
	virtual bool GetString(unsigned int index, char*& value);

	void Reset(unsigned int reserve);
	// bool/string/integer/number/nil onto L; returns the count.
	int PushToLua(lua_State* L) const;

protected:
	struct Value
	{
		ArgType type;
		bool b;
		int i;
		float f;
		std::string s;
	};
	Value& Add(ArgType type);

	std::vector<Value> m_values;
	unsigned int m_count;
};

// Where include() and LoadFile() find Lua source. RtVfs (rt_vfs.h) implements this over the game-data manifest;
// RtDirSource below is the directory implementation used by the `lua` scenario.
class RtLuaSource
{
public:
	virtual ~RtLuaSource() {}
	// include(name): stem match (case-insensitive, directory and ".lua" ignored). path is an opaque id for ReadSource.
	virtual bool ResolveInclude(const char* name, std::string& path) = 0;
	// LoadFile(name): a path as the gamecore spells it.
	virtual bool ResolvePath(const char* name, std::string& path) = 0;
	virtual bool ReadSource(const std::string& path, std::string& out) = 0;
	// The folder of a script that was just loaded by LoadFile is searched first by include() (Lekmap's HB* libraries).
	virtual void PushFrontIncludeRoot(const std::string& path) { (void)path; }
	// include(name) of a file the original game also lacks (vfs-index.json known_missing): returns false without a log line.
	virtual bool IsKnownMissing(const char* name) { (void)name; return false; }
};

// Stopgap until rt_vfs (task A6): plain directories on the host file system (a WASI preopen on wasm), searched in
// order, files before subfolders, subfolders in name order (as HstVfs).
class RtDirSource : public RtLuaSource
{
public:
	void AddRoot(const std::string& dir, bool recursive);
	virtual bool ResolveInclude(const char* name, std::string& path);
	virtual bool ResolvePath(const char* name, std::string& path);
	virtual bool ReadSource(const std::string& path, std::string& out);
	virtual void PushFrontIncludeRoot(const std::string& path);

private:
	struct Root
	{
		std::string dir;
		bool recursive;
		bool indexed;
		std::map<std::string, std::string> stems;  // lowercase stem -> path
	};
	void IndexDir(Root& root, const std::string& dir);
	std::vector<Root> m_roots;
};

class RtScriptSystem : public PortableEngineScriptSystem1Gen
{
public:
	RtScriptSystem();
	~RtScriptSystem();

	// Master state, libraries, exe globals, prelude. source: include()/LoadFile lookup (not owned, may be NULL);
	// db: the game database for GameDefines / A5's GameInfo and DB.Query (not owned, may be NULL);
	// libs: gamecore's RegisterScriptLibraries (GameInfoTypes, Map, Game, Players, ...), NULL = none.
	bool Init(RtLuaSource* source, Database::Connection* db, ICvScriptSystemUtility1* libs);
	void Shutdown();

	lua_State* GetMasterState() const { return m_master; }
	RtLuaSource* GetSource() const { return m_source; }
	void SetSource(RtLuaSource* source) { m_source = source; }
	Database::Connection* GetDatabase() const { return m_db; }
	const char* GetThreadName(lua_State* L) const;
	int GetErrorCount() const { return m_errors; }
	// Every ReportError also becomes a GC_EVENT_LUA_ERROR in this queue (F7); NULL = none.
	void SetEvents(RtEventQueue* p) { m_events = p; }
	// B4 M2 WO-1: a replica's UI gets a copy of the GameEvents it subscribed to and of the Events.X(...) calls of the core's Lua (rt_ui_replica.h).
	void SetUiReplica(RtUiReplica* p) { m_uiRep = p; }
	RtUiReplica* UiReplica() const { return m_uiRep; }
	int GetStubCount() const { return (int)m_stubsSeen.size(); }

	// print() output goes to the log (LOG_INFO); with capture on it is also kept here (tests).
	void SetCapturePrints(bool on) { m_capture = on; }
	const std::vector<std::string>& PrintedLines() const { return m_printed; }
	void ClearPrintedLines() { m_printed.clear(); }
	void LogLua(lua_State* L, const char* text);

	// A Lua error caught by the host: logged at LOG_ERROR and counted.
	void ReportError(const char* where, const char* message);
	bool RunString(lua_State* L, const char* source, const char* chunkName);
	bool RunFile(lua_State* L, const std::string& path, const char* chunkName);

	// ---- gameplay contexts, Events, Modding.OpenSaveData ----
	// One Lua thread with its own globals and ContextPtr; the file is the include stem szName. NULL when missing.
	lua_State* LoadContext(const char* name);
	int ContextCount() const { return (int)m_contexts.size(); }
	// Contexts whose file was found and ran without a Lua error.
	int LoadedContextCount() const
	{
		int n = 0;
		for (size_t i = 0; i < m_contexts.size(); ++i) n += m_contexts[i].loaded ? 1 : 0;
		return n;
	}
	// Fire Events.<name> from the host: every listener under pcall in registration order; returns the number called.
	int FireEvent(const char* name);
	// Connection to ./Civ5SavedGameDatabase.db for Modding.OpenSaveData (opened on first use; NULL on failure).
	Database::Connection* GetSaveData();
	void CloseSaveData();
	// Drop every context thread and the listeners they registered (end of a game / before a load).
	void ResetContexts();

	// Deterministic math.random / os.* state (host-owned, reset with the game).
	void ResetGameState();
	unsigned int RandState() const { return m_randState; }

	// ---- ICvEngineScriptSystem1 ----
	virtual lua_State* CreateLuaThread(const char* szName);
	virtual void FreeLuaThread(lua_State* p0);
	virtual bool LoadFile(lua_State* L, const char* szFilename);
	virtual ICvEngineScriptSystemArgs1* CreateArgs();
	virtual ICvEngineScriptSystemArgs1* CreateArgs(uint uiReserve);
	virtual void DestroyArgs(ICvEngineScriptSystemArgs1* pkArgs);
	virtual bool CallCFunction(lua_State* L, lua_CFunction func, void* args);
	virtual bool CallFunction(lua_State* L, unsigned int nArgs, unsigned int nRet);
	virtual bool CallHook(const char* szName, ICvEngineScriptSystemArgs1* args, bool& value);
	virtual bool CallAccumulator(const char* szName, ICvEngineScriptSystemArgs1* args, int& value);
	virtual bool CallAccumulator(const char* szName, ICvEngineScriptSystemArgs1* args, float& value);
	virtual bool CallTestAll(const char* szName, ICvEngineScriptSystemArgs1* args, bool& value);
	virtual bool CallTestAny(const char* szName, ICvEngineScriptSystemArgs1* args, bool& value);

	// exe-Lua internals (rt_lua_exe.cpp / rt_lua_gameinfo.cpp)
	bool RegisterExeGlobals(lua_State* L);
	bool NoteStub(const std::string& key) { return m_stubsSeen.insert(key).second; }
	double NextVirtualClock() { m_virtualClock += 0.001; return m_virtualClock; }
	unsigned int NextRand() { m_randState = m_randState * 214013u + 2531011u; return m_randState; }
	unsigned int CountRandCall() { return m_randCalls++; }
	void SeedRand(unsigned int seed) { m_randState = seed; }

private:
	friend struct RtSaveAccess;  // rt_save.cpp (G4a): the runtime state a save carries
	enum HookMode { HOOK_OR, HOOK_AND, HOOK_ANY, HOOK_SUM_INT, HOOK_SUM_FLOAT };
	bool DispatchGameEvent(const char* name, ICvEngineScriptSystemArgs1* args, HookMode mode, bool& value, double& sum);
	bool DispatchGameEventCore(const char* name, ICvEngineScriptSystemArgs1* args, HookMode mode, bool& value, double& sum);
	bool ProtectedCall(lua_State* L, int nArgs, int nRet, const char* where);
	lua_State* NewThread(const char* name);

	struct ContextInfo
	{
		std::string name;
		std::string file;
		bool loaded;
		lua_State* thread;
	};

	std::vector<ContextInfo> m_contexts;
	Database::Connection* m_saveData;

	lua_State* m_master;
	RtLuaSource* m_source;
	Database::Connection* m_db;
	int m_errors;
	RtEventQueue* m_events;
	RtUiReplica* m_uiRep;
	int m_listenersRef;  // registry ref: GameEvents name -> listener array (filled by the prelude)
	bool m_capture;
	std::vector<std::string> m_printed;
	std::map<lua_State*, int> m_threadRefs;
	std::map<lua_State*, std::string> m_threadNames;
	std::vector<RtScriptArgs*> m_freeArgs;
	std::set<std::string> m_stubsSeen;
	unsigned int m_randState;
	unsigned int m_randCalls;
	double m_virtualClock;

	RtScriptSystem(const RtScriptSystem&);
	RtScriptSystem& operator=(const RtScriptSystem&);
};

// The pregame reads of the exe (PreGame.*) and GameDefines, as globals of L (rt_lua_exe.cpp). The core's Lua and, with gc_lua_attach_ui, a replica's UI state.
void RegisterExeGameDefines(lua_State* L, Database::Connection* db);
void RegisterExePreGame(lua_State* L);

// The Lua source of the prelude (rt_lua_prelude.cpp).
extern const char* const kRtLuaPrelude;

}  // namespace lekmod_portable
