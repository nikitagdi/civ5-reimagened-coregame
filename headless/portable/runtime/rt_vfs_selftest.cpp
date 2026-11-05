// Scenario `vfs` (run.sh --scenario vfs): rt_vfs over (1) an in-memory fixture with base-name conflicts between roots
// (always) and (2) the real game-data export given with --game (when the host serves vfs-index.json).
#include "CvGameCoreDLLPCH.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "rt_json.h"
#include "rt_lua.h"
#include "rt_selftest.h"
#include "rt_vfs.h"
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
		char detail[320];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(detail, sizeof(detail), fmt, ap);
		va_end(ap);
		std::string esc;
		for (const char* p = detail; *p; ++p)
		{
			if (*p == '"' || *p == '\\') esc += '\\';
			esc += *p;
		}
		char line[512];
		snprintf(line, sizeof(line), "%s{\"check\":\"%s\",\"ok\":%s,\"detail\":\"%s\"}", json.empty() ? "" : ",", name, ok ? "true" : "false", esc.c_str());
		json += line;
		printf("vfs: %-4s %s: %s\n", ok ? "ok" : "FAIL", name, esc.c_str());
		if (!ok) ++failed;
	}
};

// ---- fixture host: files in memory ----
struct Fixture
{
	std::map<std::string, std::string> files;
	int reads;
	Fixture() : reads(0) {}
};

void ReleaseNone(gc_buf*) {}

int32_t FixtureRead(void* user, const char* path, gc_buf* out)
{
	Fixture* f = (Fixture*)user;
	std::map<std::string, std::string>::const_iterator it = f->files.find(path);
	if (it == f->files.end()) return GC_ERR_NOT_FOUND;
	++f->reads;
	out->data = (uint8_t*)it->second.data();
	out->len = it->second.size();
	out->release = ReleaseNone;
	out->owner = NULL;
	return GC_OK;
}

void FixtureLog(void*, int32_t, const char*) {}
void FixtureEvent(void*, const gc_event*) {}

// Roots are ordered so that the *later* root sorts first by name: a wrong (alphabetical or path-order) implementation fails.
const char* const kFixtureIndex =
	"{\"format\":1,\"include_roots\":[\"scripts/map\",\"zz/lua\",\"maps\",\"aa/ui\"],"
	"\"files\":["
	"{\"path\":\"scripts/map/alpha.lua\"},"
	"{\"path\":\"scripts/map/bom.lua\"},"
	"{\"path\":\"scripts/map/sub/subonly.lua\"},{\"path\":\"scripts/other/shared.lua\"},{\"path\":\"scripts/other/sub/deep.lua\"},"
	"{\"path\":\"zz/lua/shared.lua\"},"
	"{\"path\":\"aa/ui/shared.lua\"},"
	"{\"path\":\"aa/ui/Mixed.Lua\"},"
	"{\"path\":\"maps/asub/dup.lua\"},"
	"{\"path\":\"maps/dup.lua\"},"
	"{\"path\":\"maps/zsub/dup.lua\"},"
	"{\"path\":\"maps/b/two.lua\"},"
	"{\"path\":\"maps/a/two.lua\"},"
	"{\"path\":\"maps/readme.txt\"}],"
	"\"contexts\":[\"zz/lua/shared.lua\"],\"context_entry\":\"aa/ui/mixed.lua\","
	"\"db\":{\"gameplay\":\"data:g\",\"localization\":\"data:l\"},"
	"\"map_scripts\":{\"My_Map\":\"scripts/map/alpha.lua\"},\"known_missing\":[\"Gone.lua\"]}";

std::string Resolve(RtVfs& vfs, const char* name)
{
	std::string path;
	return vfs.ResolveInclude(name, path) ? path : std::string("<none>");
}

void RunFixture(Check& c)
{
	Fixture fx;
	fx.files["vfs-index.json"] = kFixtureIndex;
	fx.files["scripts/map/alpha.lua"] = "alpha_loaded = 1";
	fx.files["scripts/map/bom.lua"] = std::string("\xEF\xBB\xBF") + "bom_var = 7";
	fx.files["zz/lua/shared.lua"] = "shared_from = 'zz'";
	fx.files["aa/ui/mixed.lua"] = "mixed_loaded = 1";
	gc_host host = {&fx, FixtureLog, FixtureRead, FixtureEvent};

	RtVfs vfs;
	std::string err;
	bool loaded = vfs.Load(&host, err);
	c.add("fixture_load", loaded, "%s (%u files, %u roots)", err.c_str(), (unsigned)vfs.Files().size(), (unsigned)vfs.IncludeRoots().size());
	if (!loaded) return;

	c.add("fixture_fields", vfs.ContextEntry() == "aa/ui/mixed.lua" && vfs.Contexts().size() == 1 && vfs.DbGameplay() == "data:g" &&
	      vfs.MapScript("MY_MAP") == "scripts/map/alpha.lua" && vfs.MapScript("nope").empty(), "context_entry, contexts, db, map_scripts");

	// the same stem in two roots: the root earlier in include_roots wins, whatever the alphabetical order
	c.add("conflict_root_order", Resolve(vfs, "shared") == "zz/lua/shared.lua", "shared -> %s (zz/lua before aa/ui)", Resolve(vfs, "shared").c_str());
	// inside a root: files before subfolders, subfolders by name
	c.add("files_before_subfolders", Resolve(vfs, "dup") == "maps/dup.lua", "dup -> %s (maps/dup.lua before maps/asub/dup.lua)", Resolve(vfs, "dup").c_str());
	c.add("subfolders_by_name", Resolve(vfs, "two") == "maps/a/two.lua", "two -> %s", Resolve(vfs, "two").c_str());
	// the first root (script folder) is not recursive
	c.add("first_root_not_recursive", Resolve(vfs, "subonly") == "<none>", "subonly -> %s", Resolve(vfs, "subonly").c_str());
	// stem match: case, directory part, ".lua"
	c.add("stem_match", Resolve(vfs, "ALPHA") == "scripts/map/alpha.lua" && Resolve(vfs, "some/dir\\Alpha.LUA") == "scripts/map/alpha.lua" &&
	      Resolve(vfs, "MIXED") == "aa/ui/mixed.lua", "ALPHA, dir/Alpha.LUA, MIXED (index entry Mixed.Lua)");
	c.add("non_lua_ignored", Resolve(vfs, "readme") == "<none>", "readme.txt is not an include");
	c.add("known_missing", vfs.IsKnownMissing("GONE") && vfs.IsKnownMissing("x/gone.lua") && !vfs.IsKnownMissing("alpha") &&
	      Resolve(vfs, "gone") == "<none>", "known_missing: Gone.lua");

	std::string p;
	bool lf = vfs.ResolvePath("Scripts\\Map\\Alpha.lua", p) && p == "scripts/map/alpha.lua";
	lf = lf && vfs.ResolvePath("scripts/map/alpha.lua", p) && vfs.ResolvePath("scripts/map/Alpha.lua", p) && p == "scripts/map/alpha.lua";
	c.add("resolve_path", lf && !vfs.ResolvePath("scripts/map/nope.lua", p) && !vfs.ResolvePath("", p), "case, slashes, missing");
	std::string src;
	bool rd = vfs.ReadSource("scripts/map/bom.lua", src);
	c.add("read_source_bom", rd && src == "bom_var = 7", "UTF-8 BOM stripped (%u bytes)", (unsigned)src.size());

	// LoadFile of a script puts its folder first for include()
	bool front = Resolve(vfs, "shared") == "zz/lua/shared.lua";
	vfs.PushFrontIncludeRoot("zz/lua/other.lua");  // already a root: no change
	front = front && Resolve(vfs, "shared") == "zz/lua/shared.lua";
	vfs.PushFrontIncludeRoot("Scripts\\Other\\x.lua");
	front = front && Resolve(vfs, "shared") == "scripts/other/shared.lua" && Resolve(vfs, "deep") == "<none>";  // not recursive
	c.add("push_front_root", front, "script folder searched first");

	RtVfs vfs2;
	std::string e2;
	c.add("bad_index", !vfs2.Load(&host, e2, "missing.json") && !e2.empty(), "%s", e2.c_str());
	fx.files["bad.json"] = "{\"format\":1,";
	c.add("bad_json", !vfs2.Load(&host, e2, "bad.json"), "%s", e2.c_str());
	fx.files["bad2.json"] = "{\"format\":1}";
	c.add("bad_shape", !vfs2.Load(&host, e2, "bad2.json"), "%s", e2.c_str());

	// through Lua: include() and the known_missing silence
	RtVfs lv;
	lv.Load(&host, err);
	RtScriptSystem* ss = new RtScriptSystem();
	ss->SetCapturePrints(true);
	bool inited = ss->Init(&lv, NULL, NULL);
	c.add("lua_init", inited, "RtScriptSystem over RtVfs");
	if (inited)
	{
		lua_State* L = ss->GetMasterState();
		ss->RunString(L, "r_ok = include('Alpha'); r_bom = include('bom'); r_missing = include('Gone'); r_unknown = include('nope_nope')", "vfs_test");
		lua_getglobal(L, "r_missing");
		bool missingFalse = lua_isboolean(L, -1) && !lua_toboolean(L, -1);
		lua_pop(L, 1);
		lua_getglobal(L, "bom_var");
		bool bomOk = lua_tointeger(L, -1) == 7;
		lua_pop(L, 1);
		lua_getglobal(L, "alpha_loaded");
		bool alphaOk = lua_tointeger(L, -1) == 1;
		lua_pop(L, 1);
		c.add("lua_include", alphaOk && bomOk && missingFalse && ss->GetErrorCount() == 0, "alpha, bom run; Gone -> false; errors=%d", ss->GetErrorCount());
		int notFound = 0, goneLines = 0;
		for (size_t i = 0; i < ss->PrintedLines().size(); ++i)
		{
			if (ss->PrintedLines()[i].find("nope_nope") != std::string::npos) ++notFound;
			if (ss->PrintedLines()[i].find("Gone") != std::string::npos) ++goneLines;
		}
		c.add("lua_known_missing_silent", notFound == 1 && goneLines == 0, "unknown include logs %d line, known_missing %d", notFound, goneLines);
		lua_State* t = ss->CreateLuaThread("T");
		c.add("lua_loadfile", t && ss->LoadFile(t, "Scripts/Map/alpha.lua") && ss->GetErrorCount() == 0, "LoadFile by path through RtVfs");
		if (t) ss->FreeLuaThread(t);
	}
	delete ss;
}

bool ReadGameFile(const gc_host* host, const char* path, std::string& out)
{
	gc_buf buf = {0, 0, 0, 0};
	if (host->read_file(host->user, path, &buf) != GC_OK) return false;
	out.assign((const char*)buf.data, buf.len);
	gc_buf_release(&buf);
	return true;
}

void RunReal(Check& c, const gc_host* host)
{
	RtVfs vfs;
	std::string err;
	bool loaded = vfs.Load(host, err);
	c.add("real_load", loaded, "%s (%u files, %u roots, %u contexts)", err.c_str(), (unsigned)vfs.Files().size(),
	      (unsigned)vfs.IncludeRoots().size(), (unsigned)vfs.Contexts().size());
	if (!loaded) return;
	c.add("real_counts", vfs.Files().size() == 53 && vfs.IncludeRoots().size() == 9 && vfs.Contexts().size() == 37, "53 files, 9 roots, 37 contexts");
	c.add("real_context_entry", vfs.ContextEntry() == "assets/dlc/lekmod_v34.15/lua/ui/ingame.lua", "%s", vfs.ContextEntry().c_str());

	struct Case { const char* name; const char* expect; };
	const Case cases[] = {
		{"HBMapGenerator", "assets/maps/lekmap v5.2/hbmapgenerator.lua"},
		{"MapmakerUtilities", "assets/dlc/expansion2/gameplay/lua/mapmakerutilities.lua"},  // first match: expansion2 gameplay root
		{"HBMapmakerUtilities", "assets/maps/lekmap v5.2/hbmapmakerutilities.lua"},
		{"FLuaVector", "assets/gameplay/lua/fluavector.lua"},
		{"PlotIterators", "assets/dlc/lekmod_v34.15/lua/utilities/plotiterators.lua"},
		{"InstanceManager", "assets/ui/instancemanager.lua"},
		{"lekmod_utilities", "assets/dlc/lekmod_v34.15/lua/utilities/lekmod_utilities.lua"},
		{"LEKMOD_ROMANIA.lua", "assets/dlc/lekmod_v34.15/lua/civilizations/lekmod_romania.lua"},
		{"InGame", "assets/dlc/lekmod_v34.15/lua/ui/ingame.lua"},
	};
	bool all = true;
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
	{
		std::string got = Resolve(vfs, cases[i].name);
		if (got != cases[i].expect) all = false;
		printf("vfs:      include(%s) -> %s\n", cases[i].name, got.c_str());
	}
	c.add("real_includes", all, "%u includes resolved as the root order says", (unsigned)(sizeof(cases) / sizeof(cases[0])));
	c.add("real_known_missing", vfs.IsKnownMissing("PlotAreaSweepIterator") && Resolve(vfs, "PlotAreaSweepIterator") == "<none>", "PlotAreaSweepIterator.lua");

	std::string map = vfs.MapScript("lekmap_pangaea");
	std::string p;
	std::string text;
	bool mapOk = !map.empty() && vfs.ResolvePath("Assets\\Maps\\Lekmap v5.2\\LekMapPangaeaFractalv5.2.lua", p) && p == map && vfs.ReadSource(map, text) &&
	             text.find("GetMapInitData") != std::string::npos;
	mapOk = mapOk && vfs.ResolvePath("C:/Games/Civilization V/Assets/Maps/Lekmap v5.2/LekMapPangaeaFractalv5.2.lua", p) && p == map &&
	        vfs.ResolvePath("Maps/Lekmap v5.2/LekMapPangaeaFractalv5.2.lua", p) && p == map;  // absolute tail, relative to Assets
	c.add("real_map_script", mapOk, "%s (%u bytes)", map.c_str(), (unsigned)text.size());

	// every listed file reads and compiles under Lua 5.1; the folder of the map script is searched first after LoadFile
	RtScriptSystem* ss = new RtScriptSystem();
	bool inited = ss->Init(&vfs, NULL, NULL);
	int unreadable = 0, bad = 0;
	if (inited)
	{
		lua_State* L = ss->GetMasterState();
		for (size_t i = 0; i < vfs.Files().size(); ++i)
		{
			std::string src;
			if (!vfs.ReadSource(vfs.Files()[i], src)) { ++unreadable; continue; }
			if (luaL_loadbuffer(L, src.data(), src.size(), vfs.Files()[i].c_str()) != 0)
			{
				printf("vfs:      compile error %s\n", lua_tostring(L, -1));
				++bad;
			}
			lua_settop(L, 0);
		}
		ss->RunString(L, "r_known = include('PlotAreaSweepIterator')", "vfs_real");
		lua_getglobal(L, "r_known");
		bool silent = lua_isboolean(L, -1) && !lua_toboolean(L, -1) && ss->PrintedLines().empty() && ss->GetErrorCount() == 0;
		lua_pop(L, 1);
		c.add("real_include_known_missing", silent, "include('PlotAreaSweepIterator') -> false, no log line");
	}
	c.add("real_all_files_compile", inited && unreadable == 0 && bad == 0, "%u files: %d unreadable, %d with syntax errors", (unsigned)vfs.Files().size(), unreadable, bad);
	delete ss;
}

}  // namespace

extern "C" int rt_selftest_vfs(const gc_host* host, char* report, size_t report_len)
{
	Check c;
	RunFixture(c);
	std::string probe;
	if (host && host->read_file && ReadGameFile(host, "vfs-index.json", probe)) RunReal(c, host);
	else c.add("real_export", true, "skipped: no vfs-index.json from the host (pass --game)");

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
