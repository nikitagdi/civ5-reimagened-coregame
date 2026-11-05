// Scenario `gameinfo` (task A5, Lua half of milestone M1; run.sh --scenario gameinfo): rt_session boot on the real game DB,
// rt_lua with GameInfo / DB.Query (rt_lua_gameinfo.cpp) over it, then lua_dir/gameinfo/gameinfo_probe.lua: 50 reference
// queries (real usages from the gameplay contexts and the Lekmap map script) plus semantics lines, compared line by line
// with lua_dir/gameinfo/civhost_reference.txt (what civhost printed for the same file; how it was made: header of that file).
#include "CvGameCoreDLLPCH.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "CvDllContext.h"
#include "rt_lua.h"
#include "rt_selftest.h"
#include "rt_session.h"
#include "lekmod_portable/host_log.h"

using namespace lekmod_portable;

namespace {

std::string Escape(const std::string& s)
{
	std::string out;
	for (size_t i = 0; i < s.size(); ++i)
	{
		const char ch = s[i];
		if (ch == '"' || ch == '\\') { out += '\\'; out += ch; }
		else if ((unsigned char)ch >= 0x20) out += ch;
	}
	return out;
}

struct Check
{
	std::string json;
	int failed;
	Check() : failed(0) {}

	void add(const char* name, bool ok, const char* fmt, ...) __attribute__((format(printf, 4, 5)))
	{
		char detail[512];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(detail, sizeof(detail), fmt, ap);
		va_end(ap);
		char line[800];
		snprintf(line, sizeof(line), "%s{\"check\":\"%s\",\"ok\":%s,\"detail\":\"%s\"}", json.empty() ? "" : ",", name, ok ? "true" : "false", Escape(detail).c_str());
		json += line;
		printf("gameinfo: %-4s %s: %s\n", ok ? "ok" : "FAIL", name, detail);
		if (!ok) ++failed;
	}
};

void ForwardLog(void* user, int level, const char* msg)
{
	const gc_host* host = static_cast<const gc_host*>(user);
	if (host && host->log) host->log(host->user, level, msg);
}

// "GI|Qnn|label|..." -> "Qnn"; "GI|S|name|value" -> "S|name"; "GI|done" -> "done".
std::string LineKey(const std::string& line)
{
	const std::string body = line.substr(3);
	const size_t a = body.find('|');
	if (a == std::string::npos) return body;
	if (body.compare(0, a, "S") != 0) return body.substr(0, a);
	const size_t b = body.find('|', a + 1);
	return b == std::string::npos ? body : body.substr(0, b);
}

bool ReadText(const std::string& path, std::string& text)
{
	FILE* f = fopen(path.c_str(), "rb");
	if (!f) return false;
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
	fclose(f);
	return true;
}

bool ReadLines(const std::string& path, std::vector<std::string>& out)
{
	std::string text;
	if (!ReadText(path, text)) return false;
	size_t pos = 0;
	while (pos < text.size())
	{
		size_t end = text.find('\n', pos);
		if (end == std::string::npos) end = text.size();
		std::string line = text.substr(pos, end - pos);
		if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
		if (line.compare(0, 3, "GI|") == 0) out.push_back(line);
		pos = end + 1;
	}
	return true;
}

}  // namespace

extern "C" int rt_selftest_gameinfo(const gc_host* host, const char* lua_dir, char* report, size_t report_len)
{
	SetLogSink(ForwardLog, const_cast<gc_host*>(host));
	Check c;
	std::string mismatches;
	int qTotal = 0, qMatch = 0, sTotal = 0, sMatch = 0;

	// Left alive on purpose, as in scenario `load` (GC.init's objects keep statements on the DB connection).
	RtSession* session = new RtSession();
	RtSession::LoadStatus st;
	const bool loaded = session->Load(*host, st);
	c.add("load", loaded, "boot D1-D2 B1-B4 I1-I8 reached step %s%s%s", RtSession::StepName(st.step), st.error.empty() ? "" : ": ", st.error.c_str());

	const std::string dir = std::string(lua_dir ? lua_dir : ".") + "/gameinfo";
	std::vector<std::string> reference;
	const bool haveRef = ReadLines(dir + "/civhost_reference.txt", reference);
	c.add("reference", haveRef && !reference.empty(), "%s/civhost_reference.txt: %d GI| lines", dir.c_str(), (int)reference.size());

	RtScriptSystem* ss = NULL;
	if (loaded)
	{
		ss = new RtScriptSystem();
		ss->SetCapturePrints(true);
		const bool inited = ss->Init(NULL, session->Db().Connection(), NULL);
		c.add("init", inited, "rt_lua over the game DB (GameInfo, DB.Query)");
		lua_State* L = inited ? ss->GetMasterState() : NULL;
		if (L)
		{
			// M1, Lua half: GameInfo.Units["UNIT_WARRIOR"] against the gamecore's cached CvUnitEntry
			const int warrior = GC.getInfoTypeForString("UNIT_WARRIOR", true);
			CvUnitEntry* pkWarrior = warrior >= 0 ? GC.getUnitInfo((UnitTypes)warrior) : NULL;
			int luaId = -1, luaCombat = -1;
			if (luaL_loadstring(L, "local r = GameInfo.Units[\"UNIT_WARRIOR\"] return r.ID, r.Combat, r.Type") == 0 && lua_pcall(L, 0, 3, 0) == 0)
			{
				luaId = (int)lua_tonumber(L, -3);
				luaCombat = (int)lua_tonumber(L, -2);
				lua_pop(L, 3);
			}
			else
				lua_pop(L, 1);
			c.add("m1_unit_warrior", pkWarrior && luaId == warrior && luaCombat == pkWarrior->GetCombat(),
				"GameInfo.Units[\"UNIT_WARRIOR\"]: ID=%d Combat=%d; gamecore id=%d combat=%d", luaId, luaCombat, warrior, pkWarrior ? pkWarrior->GetCombat() : -1);

			// GI_PROBE_VERBOSE=1 in the environment (linux): row dumps (GI# lines) as civhost prints them, for diffing a mismatch.
			lua_pushboolean(L, getenv("GI_PROBE_VERBOSE") == NULL);
			lua_setglobal(L, "GI_PROBE_QUIET");
			// Read directly (no directory listing: node's WASI on Windows does not list a preopen the way RtDirSource expects).
			const std::string path = dir + "/gameinfo_probe.lua";
			std::string probe;
			const bool found = ReadText(path, probe);
			const bool ran = found && ss->RunString(L, probe.c_str(), "@gameinfo_probe.lua");
			c.add("run_probe", ran, "%s%s", path.c_str(), found ? "" : " not readable");

			std::map<std::string, std::string> ours;
			for (size_t i = 0; i < ss->PrintedLines().size(); ++i)
			{
				const std::string& line = ss->PrintedLines()[i];
				if (line.compare(0, 3, "GI|") == 0) ours[LineKey(line)] = line;
				else if (line.compare(0, 3, "GI#") == 0) printf("gameinfo: %s\n", line.c_str());  // error details of the probe
			}
			std::map<std::string, bool> seen;
			for (size_t i = 0; i < reference.size(); ++i)
			{
				const std::string key = LineKey(reference[i]);
				seen[key] = true;
				std::map<std::string, std::string>::const_iterator it = ours.find(key);
				const bool ok = it != ours.end() && it->second == reference[i];
				const bool isQuery = key.size() == 3 && key[0] == 'Q';
				if (isQuery) { ++qTotal; qMatch += ok ? 1 : 0; }
				else if (key != "done") { ++sTotal; sMatch += ok ? 1 : 0; }
				printf("gameinfo: %-4s %s\n", ok ? "ok" : "DIFF", reference[i].c_str());
				if (!ok)
				{
					const std::string got = it == ours.end() ? std::string("(missing)") : it->second;
					printf("gameinfo:      rt_lua: %s\n", got.c_str());
					mismatches += std::string(mismatches.empty() ? "" : ",") + "{\"civhost\":\"" + Escape(reference[i]) + "\",\"rt_lua\":\"" + Escape(got) + "\"}";
				}
			}
			int extra = 0;
			for (std::map<std::string, std::string>::const_iterator it = ours.begin(); it != ours.end(); ++it)
				if (!seen.count(it->first)) ++extra;
			c.add("queries", qTotal == 50 && qMatch == qTotal, "%d/%d reference queries match civhost (rows and fields)", qMatch, qTotal);
			c.add("semantics", sTotal > 0 && sMatch == sTotal, "%d/%d semantics lines match civhost", sMatch, sTotal);
			c.add("no_extra_lines", extra == 0, "%d probe lines without a reference line", extra);
			c.add("no_lua_errors", ss->GetErrorCount() == 0, "%d Lua errors reported by the host", ss->GetErrorCount());
		}
	}

	char head[256];
	snprintf(head, sizeof(head), "{\"step\":\"%s\",\"queries_total\":%d,\"queries_match\":%d,\"semantics_total\":%d,\"semantics_match\":%d,\"failed\":%d,\"mismatches\":[",
		RtSession::StepName(st.step), qTotal, qMatch, sTotal, sMatch, c.failed);
	std::string out = std::string(head) + mismatches + "],\"checks\":[" + c.json + "]}";
	if (report && report_len)
	{
		strncpy(report, out.c_str(), report_len - 1);
		report[report_len - 1] = '\0';
	}
	delete ss;
	SetLogSink(NULL, NULL);
	return c.failed == 0 ? 0 : 1;
}
