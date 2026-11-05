//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstScriptSystem.h
//!  \brief     ICvEngineScriptSystem1 on lua51_Win32.dll (design section 3.1): master state, per-context
//!				threads, file loading through the VFS, protected calls and GameEvents dispatch.
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#pragma once
#ifndef HSTSCRIPTSYSTEM_H
#define HSTSCRIPTSYSTEM_H

#include "engine/gen/HstEngineScriptSystem1Gen.h"

class HstArenaAlloc;
class HstVfs;
class HstScriptArgs;

class HstScriptSystem : public HstEngineScriptSystem1Gen
{
public:
	HstScriptSystem();

	//! Create the master state (arena allocator), open the libraries, install the sandbox, the exe API and the
	//! event tables, then let the DLL register its libraries (RegisterScriptLibraries; must run after ctx->Init()).
	bool Init(HstArenaAlloc* pkArena, HstVfs* pkVfs, Database::Connection* pkDb, ICvGameContext1* pkCtx, bool bStrict);

	lua_State* GetMasterState() const { return m_pkMaster; }
	HstVfs* GetVfs() const { return m_pkVfs; }
	//! include()/LoadFile/LoadContext resolve through this VFS (map generation and gameplay contexts use different roots).
	void SetVfs(HstVfs* pkVfs) { m_pkVfs = pkVfs; }
	ICvGameContext1* GetContext() const { return m_pkCtx; }
	Database::Connection* GetDatabase() const { return m_pkDb; }
	const char* GetThreadName(lua_State* L) const;
	int GetErrorCount() const { return m_iErrors; }
	//! Log a Lua error (and abort under --strict).
	void ReportError(const char* szWhere, const char* szMessage);
	//! Write a line to <run>\lua.log ("<context>: text").
	void LogLua(lua_State* L, const char* szText);
	//! Run a chunk of Lua source in L's globals (host prelude, tests).
	bool RunString(lua_State* L, const char* szSource, const char* szChunkName);
	//! Load and execute a file (absolute path) in L's globals.
	bool RunFile(lua_State* L, const std::wstring& wstrPath, const char* szChunkName);

	// ---- gameplay contexts (design 3.4), Events (L9), Modding.OpenSaveData ----
	//! One Lua thread with its own globals and ContextPtr; the file is the stem szName resolved through the VFS
	//! include roots and is executed once. Returns the thread, or NULL when the file is missing.
	lua_State* LoadContext(const char* szName);
	struct ContextInfo
	{
		std::string strName;
		std::string strFile;	// resolved path (UTF-8), empty when not found
		bool bLoaded;			// file found and executed without error
		lua_State* pkThread;	// NULL when not found
	};
	const std::vector<ContextInfo>& GetContexts() const { return m_akContexts; }
	//! Fire a UI event (Events.<szName>) from the host: every listener under pcall, in registration order.
	//! Returns the number of listeners called.
	int FireEvent(const char* szName);
	//! <run>\events-registered.json: contexts, every Events/GameEvents/LuaEvents.X.Add with its context, fire counts.
	void WriteEventsRegistered(const std::wstring& wstrPath);
	//! Connection to <cache>Civ5SavedGameDatabase.db for Modding.OpenSaveData (opened on first use; NULL on failure).
	Database::Connection* GetSaveData();
	//! Close the save-data connection (before CvGame::reset deletes/recreates the file, i.e. before Init/Read).
	void CloseSaveData();
	//! Drop every context thread and the event listeners they registered (end of a game / before a load).
	void ResetContexts();

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

protected:
	enum HookMode { HOOK_OR, HOOK_AND, HOOK_ANY, HOOK_SUM_INT, HOOK_SUM_FLOAT };
	//! Calls every GameEvents[szName] listener; returns false when there are none.
	bool DispatchGameEvent(const char* szName, ICvEngineScriptSystemArgs1* pkArgs, HookMode eMode, bool& bValue, double& dSum);
	//! pcall with a traceback handler; on error logs, pops the message and pushes nRet nils.
	bool ProtectedCall(lua_State* L, int nArgs, int nRet, const char* szWhere);
	//! lua_newthread + own globals (__index = master _G); registers the globals in __HstContextOf.
	lua_State* NewThread(const char* szName);

	std::vector<ContextInfo> m_akContexts;
	Database::Connection* m_pkSaveData;
	std::map<std::string, int> m_kHostFires;	// Events fired by the host (FireEvent)

	lua_State* m_pkMaster;
	HstVfs* m_pkVfs;
	Database::Connection* m_pkDb;
	ICvGameContext1* m_pkCtx;
	bool m_bStrict;
	int m_iErrors;
	int m_iListenersRef;	// registry ref: table GameEvents name -> listener array (filled by the Lua prelude)
	FILE* m_pkLuaLog;
	std::map<lua_State*, int> m_kThreadRefs;
	std::map<lua_State*, std::string> m_kThreadNames;
	std::vector<HstScriptArgs*> m_apFreeArgs;
};

#endif // HSTSCRIPTSYSTEM_H
