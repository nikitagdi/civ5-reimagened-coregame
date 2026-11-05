// Scenario `newgame` (task A8, milestone M2; run.sh --scenario newgame --seed N): RtSession::Load, then RtSession::NewGame with the
// setup JSON {"seed":N} (ABI 4.2 defaults: Duel, Lekmap Pangaea, Rome + 1 AI, 4 city-states): G1-G9, L1-L6 (Init2 runs the map
// script through rt_lua + rt_vfs), the 37 gameplay contexts, DoGameStarted, Events.SequenceGameInitComplete. Checks: 0 Lua errors,
// every context loaded, players alive, and the map digest recomputed from lua_dir/newgame/map_probe.lua (the probe also runs in
// civhost, so its map can be digested the same way). The digest itself is compared across processes by the caller.
#include "CvGameCoreDLLPCH.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <memory>
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
		char detail[768];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(detail, sizeof(detail), fmt, ap);
		va_end(ap);
		json += std::string(json.empty() ? "" : ",") + "{\"check\":\"" + name + "\",\"ok\":" + (ok ? "true" : "false") + ",\"detail\":\"" + Escape(detail) + "\"}";
		printf("newgame: %-4s %s: %s\n", ok ? "ok" : "FAIL", name, detail);
		if (!ok) ++failed;
	}
};

void ForwardLog(void* user, int level, const char* msg)
{
	const gc_host* host = static_cast<const gc_host*>(user);
	if (host && host->log) host->log(host->user, level, msg);
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

std::string Num(int v)
{
	char buf[16];
	snprintf(buf, sizeof(buf), "%d", v);
	return buf;
}

std::string Hex(uint64_t v)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)v);
	return buf;
}

// "MP|H|w h", "MP|R|y|x0|cells", "MP|done" -> RtSession::MapText() layout. Rows and chunks arrive in order.
bool ProbeText(const std::vector<std::string>& lines, std::string& text, int& rows)
{
	text.clear();
	rows = 0;
	int curRow = -1;
	bool done = false, head = false;
	for (size_t i = 0; i < lines.size(); ++i)
	{
		const std::string& l = lines[i];
		if (l.compare(0, 5, "MP|H|") == 0) { text += l.substr(5) + "\n"; head = true; }
		else if (l.compare(0, 5, "MP|R|") == 0)
		{
			const size_t a = l.find('|', 5), b = a == std::string::npos ? a : l.find('|', a + 1);
			if (b == std::string::npos) return false;
			const int y = atoi(l.substr(5, a - 5).c_str());
			if (y != curRow)
			{
				if (curRow >= 0) text += "\n";
				text += l.substr(5, a - 5) + ":";
				curRow = y;
				++rows;
			}
			text += l.substr(b + 1);
		}
		else if (l == "MP|done") done = true;
	}
	if (curRow >= 0) text += "\n";
	return head && done;
}

}  // namespace

extern "C" int rt_selftest_newgame(const gc_host* host, const char* lua_dir, unsigned seed, char* report, size_t report_len)
{
	SetLogSink(ForwardLog, const_cast<gc_host*>(host));
	Check c;

	// Left alive on purpose, as in scenario `load` (GC.init's objects keep statements on the DB connection).
	RtSession* session = new RtSession();
	RtSession::LoadStatus ls;
	const bool loaded = session->Load(*host, ls);
	c.add("load", loaded, "boot D1-D2 B1-B4 I1-I8 reached step %s%s%s", RtSession::StepName(ls.step), ls.error.empty() ? "" : ": ", ls.error.c_str());

	RtSession::NewGameStatus st;
	char setup[64];
	snprintf(setup, sizeof(setup), "{\"game_type\":\"single\",\"seed\":%u}", seed);
	std::string probeDigest = "", mapText;
	int stubs = 0;
	if (loaded)
	{
		const bool started = session->NewGame(*host, setup, st);
		c.add("new_game", started, "%s reached step %s%s%s", setup, RtSession::NewGameStepName(st.step), st.error.empty() ? "" : ": ", st.error.c_str());
		c.add("init2", st.init2, "game->Init2() returned %d (L1-L6 %.0f ms)", (int)st.init2, st.msLaunch);
		c.add("map", st.width > 0 && st.height > 0 && st.plots == st.width * st.height && st.startPlots >= 2,
			"%d x %d = %d plots, wrapX=%d wrapY=%d, %d start plots", st.width, st.height, st.plots, (int)st.wrapX, (int)st.wrapY, st.startPlots);
		c.add("lua_errors_map", st.luaErrorsMap == 0, "%d Lua error(s) during boot + map generation", st.luaErrorsMap);
		c.add("contexts", started && st.contextsTotal == 37 && st.contextsLoaded == st.contextsTotal, "%d/%d gameplay contexts loaded without errors (%.0f ms)",
			st.contextsLoaded, st.contextsTotal, st.msContexts);
		c.add("lua_errors_contexts", started && st.luaErrorsContexts == 0, "%d Lua error(s) while loading the contexts", st.luaErrorsContexts);
		c.add("lua_errors_start", started && st.luaErrorsStart == 0, "%d Lua error(s) in DoGameStarted + SequenceGameInitComplete (%d listener(s))", st.luaErrorsStart,
			st.initListeners);
		const int expectMajors = (int)session->Setup().civs.size(), expectMinors = (int)session->Setup().minors.size();
		// AssignStartingPlots may discard a city-state on a crowded Duel map ("discarded due to overcrowding", e.g. seed 7): original
		// behaviour, so fewer city-states than set up is accepted; every major civilization must have started.
		c.add("players", started && st.majorsAlive == expectMajors && st.minorsAlive <= expectMinors && (expectMinors == 0 || st.minorsAlive > 0) && st.units > 0,
			"turn %d: %d/%d major and %d/%d minor civ(s) alive%s, %d unit(s)", st.turn, st.majorsAlive, expectMajors, st.minorsAlive, expectMinors,
			st.minorsAlive < expectMinors ? " (the map script discarded the rest)" : "", st.units);
		c.add("map_digest", st.mapDigest != 0, "%s after map generation, %s after the game start%s; CvMap::Write digest %s", Hex(st.mapDigest).c_str(),
			Hex(st.mapDigestStarted).c_str(), st.mapDigest == st.mapDigestStarted ? " (unchanged)" : " (CHANGED by the start)", Hex(st.mapWriteDigest).c_str());

		// Regression guard: seed 1 is bit-identical to civhost (map digest of RtSession::MapText).
		if (seed == 1)
			c.add("map_digest_pinned", st.mapDigestStarted == 0xa4f58ec20dd082e0ull, "seed 1 map digest %s, expected a4f58ec20dd082e0 (civhost)", Hex(st.mapDigestStarted).c_str());

		// The probe in a thread of its own, as civhost runs it (a gameplay context).
		RtScriptSystem* ss = session->Scripts();
		const std::string path = std::string(lua_dir ? lua_dir : ".") + "/newgame/map_probe.lua";
		std::string src;
		if (ss && started && ReadText(path, src))
		{
			const int errors = ss->GetErrorCount();
			ss->ClearPrintedLines();
			ss->SetCapturePrints(true);
			lua_State* T = ss->CreateLuaThread("map_probe");
			const bool ran = ss->RunString(T, src.c_str(), "@map_probe.lua");
			ss->FreeLuaThread(T);
			ss->SetCapturePrints(false);
			std::vector<std::string> lines;
			for (size_t i = 0; i < ss->PrintedLines().size(); ++i)
				if (ss->PrintedLines()[i].compare(0, 3, "MP|") == 0) lines.push_back(ss->PrintedLines()[i]);
			ss->ClearPrintedLines();
			std::string text;
			int rows = 0;
			const bool parsed = ProbeText(lines, text, rows);
			mapText = RtSession::MapText();
			const uint64_t pd = RtSession::Fnv1a64(text.data(), text.size());
			probeDigest = Hex(pd);
			c.add("probe_digest", ran && parsed && ss->GetErrorCount() == errors && pd == st.mapDigestStarted && text == mapText,
				"map_probe.lua: %d row(s), %d line(s), digest %s (C++ %s)", rows, (int)lines.size(), probeDigest.c_str(), Hex(st.mapDigestStarted).c_str());
			stubs = ss->GetStubCount();
		}
		else
			c.add("probe_digest", false, "%s not readable or the game did not start", path.c_str());

		// RT_MAP_WRITE_DUMP=<file> (linux): the CvMap::Write bytes taken after L6, to diff with civhost's map-write.bin.
		const char* dump = getenv("RT_MAP_WRITE_DUMP");
		if (dump && *dump && !st.mapWrite.empty())
		{
			FILE* f = fopen(dump, "wb");
			if (f)
			{
				fwrite(&st.mapWrite[0], 1, st.mapWrite.size(), f);
				fclose(f);
			}
		}

		RtSession::NewGameStatus again;
		c.add("second_new_game_refused", !session->NewGame(*host, setup, again) && !again.error.empty(), "%s", again.error.c_str());
	}

	std::string out = "{\"failed\":" + Num(c.failed) + ",\"seed\":" + Num(seed) + ",\"setup\":\"" + Escape(setup) + "\",\"step\":\"" +
		RtSession::NewGameStepName(st.step) + "\",\"width\":" + Num(st.width) + ",\"height\":" + Num(st.height) +
		",\"plots\":" + Num(st.plots) + ",\"map_digest\":\"" + Hex(st.mapDigest) + "\",\"map_digest_started\":\"" + Hex(st.mapDigestStarted) +
		"\",\"probe_digest\":\"" + probeDigest + "\",\"map_write_digest\":\"" + Hex(st.mapWriteDigest) + "\",\"lua_errors\":{\"map\":" +
		Num(st.luaErrorsMap) + ",\"contexts\":" + Num(st.luaErrorsContexts) + ",\"start\":" + Num(st.luaErrorsStart) +
		"},\"lua_auto_stubs\":" + Num(stubs) + ",\"contexts_loaded\":" + Num(st.contextsLoaded) + ",\"turn\":" + Num(st.turn) +
		",\"ms\":{\"launch\":" + Num((int)st.msLaunch) + ",\"contexts\":" + Num((int)st.msContexts) + ",\"start\":" +
		Num((int)st.msStart) + "},\"setup_resolved\":\"" + Escape(st.setup) + "\",\"checks\":[" + c.json + "]}";
	if (report && report_len)
	{
		strncpy(report, out.c_str(), report_len - 1);
		report[report_len - 1] = '\0';
	}
	// The canonical map text for offline comparison (civhost): printed once, prefixed.
	if (!mapText.empty())
	{
		size_t pos = 0;
		while (pos < mapText.size())
		{
			size_t end = mapText.find('\n', pos);
			if (end == std::string::npos) end = mapText.size();
			printf("newgame-map|%s\n", mapText.substr(pos, end - pos).c_str());
			pos = end + 1;
		}
	}
	SetLogSink(NULL, NULL);
	return c.failed == 0 ? 0 : 1;
}
