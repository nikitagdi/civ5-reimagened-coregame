// Scenario `lua` (run.sh --scenario lua): the exe-Lua of rt_lua inside the gamecore archive. Runs
// headless/tools/clang/runtime/lua/test_main.lua (checks raise Lua errors, which the host counts) and checks the
// host side: RtEngine::SetScriptSystem, CallHook / CallAccumulator / FireEvent, LoadContext, print capture.
#include "CvGameCoreDLLPCH.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "CvLocalization.h"
#include "CvPreGame.h"
#include "rt_engine.h"
#include "rt_fs.h"
#include "rt_lua.h"
#include "rt_selftest.h"
#include "lekmod_portable/host_log.h"

using namespace lekmod_portable;

namespace {

struct Check
{
	std::string json;
	int failed;
	Check() : failed(0) {}

	void add(const char* name, bool ok, const char* fmt, ...)
	{
		char detail[256];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(detail, sizeof(detail), fmt, ap);
		va_end(ap);
		char line[400];
		snprintf(line, sizeof(line), "%s{\"check\":\"%s\",\"ok\":%s,\"detail\":\"%s\"}", json.empty() ? "" : ",", name, ok ? "true" : "false", detail);
		json += line;
		printf("lua: %-4s %s: %s\n", ok ? "ok" : "FAIL", name, detail);
		if (!ok) ++failed;
	}
};

const char* const kTestSchema =
	"CREATE TABLE Language_en_US(Tag TEXT PRIMARY KEY, Text TEXT);"
	"INSERT INTO Language_en_US VALUES('TXT_KEY_RT_TEST_TURN', 'Turn {1_Num} on {2_World}');"
	"CREATE TABLE Defines(Name TEXT PRIMARY KEY, Value);"
	"INSERT INTO Defines VALUES('RT_TEST_INT', 17);"
	"INSERT INTO Defines VALUES('RT_TEST_TEXT', 'abc');";

bool printed(const RtScriptSystem& ss, const char* text)
{
	const std::vector<std::string>& lines = ss.PrintedLines();
	for (size_t i = 0; i < lines.size(); ++i)
		if (lines[i] == text) return true;
	return false;
}

}  // namespace

extern "C" int rt_selftest_lua(const char* lua_dir, char* report, size_t report_len)
{
	Check c;

	// Environment the script reads: localization, Defines, PreGame, the in-memory saved-game DB.
	Database::Connection db;
	bool dbOk = db.Open(NULL, Database::OPEN_READWRITE | Database::OPEN_CREATE) && db.ExecuteMultiple(kTestSchema);
	c.add("test_database", dbOk, "in-memory DB with Language_en_US and Defines");
	if (dbOk) Localization::InitializeDictionary(db, "Languages", "en_US", true);
	CvPreGame::setNumMinorCivs(7);
	CvPreGame::setEra((EraTypes)2);
	CvPreGame::setMaxTurns(123);

	c.add("fs_install", rt_fs::Install(), "memory VFS for ./Civ5SavedGameDatabase.db");
	rt_fs::Reset();
	{
		Database::Connection save;
		bool ok = save.Open("./Civ5SavedGameDatabase.db", Database::OPEN_CREATE | Database::OPEN_READWRITE | Database::OPEN_FULLMUTEX) &&
		          save.Execute("CREATE TABLE SimpleValues(Name TEXT Primary Key, Value VARIANT)");
		c.add("save_db", ok, "SimpleValues created in rt_fs");
	}

	RtDirSource source;
	source.AddRoot(lua_dir, true);
	RtEngine engine;
	RtScriptSystem* ss = new RtScriptSystem();
	ss->SetCapturePrints(true);
	bool inited = ss->Init(&source, &db, NULL);
	c.add("init", inited, "Lua 5.1 master state, exe globals, prelude (include dir: %s)", lua_dir);
	engine.SetScriptSystem(ss);
	c.add("install", engine.GetScriptSystem() == ss, "RtEngine::GetScriptSystem returns the rt_lua object");

	if (inited)
	{
		lua_State* L = ss->GetMasterState();
		std::string path;
		bool found = source.ResolveInclude("test_main", path);
		c.add("find_script", found, "%s", path.c_str());
		bool ran = found && ss->RunFile(L, path, "test_main.lua");
		c.add("run_test_main", ran && ss->GetErrorCount() == 0, "test_main.lua: %d Lua errors", ss->GetErrorCount());
		c.add("print", printed(*ss, "hello\t1\ttrue\tnil\ttable"), "print joins its arguments with tabs, no addresses");
		bool passed = false;
		for (size_t i = 0; i < ss->PrintedLines().size(); ++i)
			if (ss->PrintedLines()[i].compare(0, 18, "lua checks passed:") == 0) passed = true;
		c.add("checks_passed", passed, "test_main.lua reached its last line");

		// Number -> int as the game's lua51_Win32.dll (fistp: round half to even; luaconf.h, LEKMOD): civhost printed
		// cdefgh, bcdefgh, defgh, fgh, 67 for these (A8; Lekmap's BuildRidges((100 * 2) / 3) depends on it).
		lua_pushnumber(L, 2.6);
		const int r26 = (int)lua_tointeger(L, -1);
		lua_pushnumber(L, 2.5);
		const int r25 = (int)lua_tointeger(L, -1);
		lua_pushnumber(L, 3.5);
		const int r35 = (int)lua_tointeger(L, -1);
		lua_pushnumber(L, -2.6);
		const int rm26 = (int)lua_tointeger(L, -1);
		lua_pushnumber(L, 200.0 / 3.0);
		const int r667 = (int)lua_tointeger(L, -1);
		lua_pop(L, 5);
		c.add("number_to_int_rounds", r26 == 3 && r25 == 2 && r35 == 4 && rm26 == -3 && r667 == 67,
			"2.6 -> %d, 2.5 -> %d, 3.5 -> %d, -2.6 -> %d, 200/3 -> %d (expect 3, 2, 4, -3, 67)", r26, r25, r35, rm26, r667);

		// GameDefines from the DB
		lua_getglobal(L, "GameDefines");
		lua_getfield(L, -1, "RT_TEST_INT");
		lua_getfield(L, -2, "RT_TEST_TEXT");
		c.add("game_defines", lua_tointeger(L, -2) == 17 && lua_isstring(L, -1) && strcmp(lua_tostring(L, -1), "abc") == 0, "Defines -> GameDefines");
		lua_pop(L, 3);

		// the DLL side: GameEvents hooks through ICvEngineScriptSystem1
		ICvEngineScriptSystem1* iss = engine.GetScriptSystem();
		ICvEngineScriptSystemArgs1* args = iss->CreateArgs();
		args->Push(2);
		args->Push(3);
		bool hook = false;
		bool had = iss->CallHook("RtTestHook", args, hook);
		c.add("call_hook", had && hook, "CallHook(RtTestHook, 2, 3) = %d", (int)hook);
		int acc = 0;
		bool hadAcc = iss->CallAccumulator("RtTestAccum", args, acc);
		c.add("call_accumulator", hadAcc && acc == 10, "CallAccumulator = %d (expect 10)", acc);
		bool all = false;
		iss->CallTestAll("RtTestHook", args, all);
		c.add("call_test_all", all, "CallTestAll");
		bool none = true;
		bool hadNone = iss->CallHook("RtNoListeners", args, none);
		c.add("call_hook_no_listeners", !hadNone && !none, "no listeners -> false");
		iss->DestroyArgs(args);

		// Events fired by the host
		int called = ss->FireEvent("RtHostFired");
		lua_getglobal(L, "RT_HOST_FIRED");
		c.add("fire_event", called == 1 && lua_tointeger(L, -1) == 1, "FireEvent(RtHostFired) called %d", called);
		lua_pop(L, 1);
		ss->ClearPrintedLines();

		// contexts: own globals, ContextPtr
		lua_State* T = ss->LoadContext("rt_test_context");
		lua_State* none_t = ss->LoadContext("rt_no_such_context");
		bool contextOk = false;
		if (T)
		{
			lua_getglobal(T, "RT_CONTEXT_GLOBAL");
			contextOk = lua_isstring(T, -1) && strcmp(lua_tostring(T, -1), "rt_test_context") == 0;
			lua_pop(T, 1);
		}
		lua_getglobal(L, "RT_CONTEXT_GLOBAL");
		bool leaked = !lua_isnil(L, -1);
		lua_pop(L, 1);
		c.add("load_context", T && !none_t && contextOk && !leaked && ss->ContextCount() == 2, "context globals stay in the context");

		// a thread the DLL creates (CvMapGenerator): LoadFile runs the chunk in the thread's globals
		lua_State* mt = iss->CreateLuaThread("MapGenerator");
		std::string mapPath;
		bool loadFile = mt && iss->LoadFile(mt, "rt_test_map.lua");
		lua_getglobal(mt, "RT_MAP_LOADED");
		bool inThread = lua_tointeger(mt, -1) == 42;
		lua_pop(mt, 1);
		lua_getglobal(L, "RT_MAP_LOADED");
		bool inMaster = !lua_isnil(L, -1);
		lua_pop(L, 1);
		c.add("load_file", loadFile && inThread && !inMaster, "LoadFile executes in the thread's own globals");
		if (mt) iss->FreeLuaThread(mt);

		ss->ResetContexts();
		c.add("reset_contexts", ss->ContextCount() == 0 && ss->FireEvent("RtHostFired") == 0, "listeners dropped");
		c.add("stubs_logged", ss->GetStubCount() > 0, "%d distinct auto-stubs called", ss->GetStubCount());
		c.add("no_lua_errors_at_end", ss->GetErrorCount() == 0, "%d Lua errors in total", ss->GetErrorCount());
	}

	engine.SetScriptSystem(NULL);
	delete ss;
	if (dbOk) Localization::ShutdownDictionary();
	rt_fs::Reset();

	char head[96];
	snprintf(head, sizeof(head), "{\"failed\":%d,\"checks\":[", c.failed);
	std::string out = std::string(head) + c.json + "]}";
	if (report && report_len)
	{
		strncpy(report, out.c_str(), report_len - 1);
		report[report_len - 1] = '\0';
	}
	return c.failed == 0 ? 0 : 1;
}
