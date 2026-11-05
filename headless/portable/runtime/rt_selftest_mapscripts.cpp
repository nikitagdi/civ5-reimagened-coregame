// Scenario `mapscripts` (task N3.18; run.sh --scenario mapscripts [--map-scripts NAME,..] [--sizes WORLDSIZE_DUEL,WORLDSIZE_HUGE] --game GAMEDATA): one game on one
// map script of the game data (vfs-index.json "map_scripts") and one world size: a network game with a test-bot human and AI through the C ABI (gc_create, gc_new_game with
// "map_script", gc_pump), `turns` (default 2) turns. A script passes with: the game starts (map generated, 0 Lua errors), the map has plots and every civ has a start, the turns are
// played without STUCK, GAME_OVER, gc_pump errors, Lua errors or logged errors. One process per game (the gamecore is a singleton); run.sh loops over scripts and sizes and writes
// map-scripts.json next to vfs-index.json from the "mapscripts-summary|" lines. The script argument is the path of the index ("assets/maps/continents.lua").
#include "CvGameCoreDLLPCH.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <string>
#include <vector>

#include "../abi/gc_internal.h"
#include "rt_active.h"
#include "rt_selftest.h"
#include "rt_session.h"

using namespace lekmod_portable;

namespace {

std::string Format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string Format(const char* fmt, ...)
{
	char buf[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return buf;
}

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

struct Seen
{
	const gc_host* outer;
	int logErrors, luaErrors;
	std::vector<std::string> lines;
	Seen() : outer(NULL), logErrors(0), luaErrors(0) {}
};

void OnLog(void* user, int32_t level, const char* msg)
{
	Seen* s = static_cast<Seen*>(user);
	if (level == GC_LOG_ERROR) { ++s->logErrors; if (s->lines.size() < 10) s->lines.push_back(std::string("log error: ") + msg); }
	if (s->outer && s->outer->log) s->outer->log(s->outer->user, level, msg);
}

int32_t OnReadFile(void* user, const char* path, gc_buf* out)
{
	Seen* s = static_cast<Seen*>(user);
	return s->outer->read_file(s->outer->user, path, out);
}

void OnEvent(void* user, const gc_event* ev)
{
	Seen* s = static_cast<Seen*>(user);
	if (ev->kind == GC_EVENT_LUA_ERROR)
	{
		++s->luaErrors;
		if (s->lines.size() < 10) s->lines.push_back(std::string("lua error: ") + (ev->text ? ev->text : ""));
	}
}

double NowMs()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

}  // namespace

extern "C" int rt_selftest_mapscripts(const gc_host* host, unsigned seed, unsigned turns, unsigned ai, const char* map_script, const char* world_size, char* report, size_t report_len)
{
	Seen seen;
	seen.outer = host;
	gc_host mine = {&seen, OnLog, OnReadFile, OnEvent};
	gc_error err;
	memset(&err, 0, sizeof(err));
	const double t0 = NowMs();
	const std::string script = map_script ? map_script : "", size = world_size && *world_size ? world_size : "WORLDSIZE_DUEL";
	std::string reason;
	int doneTurns = 0, width = 0, height = 0, startedCivs = 0, wantCivs = 0;
	double newGameMs = 0;
	bool started = false;

	gc_instance* inst = gc_create(&mine, &err);
	if (!inst) reason = Format("gc_create: %d %s", err.code, err.message);
	else
	{
		std::string players = "{\"kind\":\"human\",\"civ\":\"CIVILIZATION_ROME\",\"nick\":\"bot0\"}";
		for (unsigned i = 0; i < ai; ++i) players += ",{\"kind\":\"ai\",\"civ\":\"random\"}";
		const std::string setup = Format("{\"seed\":%u,\"game_type\":\"network\",\"players\":[%s],\"turn_mode\":\"simultaneous\",\"autoplay_humans\":[0],\"handicap\":\"HANDICAP_CHIEFTAIN\",\"world_size\":\"%s\",\"map_script\":\"%s\"}",
			seed, players.c_str(), size.c_str(), Escape(script).c_str());
		const double t1 = NowMs();
		const int32_t rc = gc_new_game(inst, setup.c_str(), &err);
		newGameMs = NowMs() - t1;
		RtSession* session = InstanceSession(inst);
		if (rc != GC_OK || !session) reason = Format("gc_new_game: %d %s", rc, err.message);
		else
		{
			started = true;
			CvGame& game = GC.getGame();
			width = GC.getMap().getGridWidth();
			height = GC.getMap().getGridHeight();
			wantCivs = (int)ai + 1;
			for (int i = 0; i < MAX_MAJOR_CIVS; ++i)
				if (GET_PLAYER((PlayerTypes)i).isAlive() && GET_PLAYER((PlayerTypes)i).getNumUnits() > 0) ++startedCivs;
			if (width <= 0 || height <= 0) reason = "the map has no plots";
			else if (startedCivs < wantCivs) reason = Format("only %d of %d civs started with units", startedCivs, wantCivs);
			gc_pump_result pr;
			memset(&pr, 0, sizeof(pr));
			const int start = game.getGameTurn(), target = start + (int)turns;
			int turn = start;
			const long long maxCalls = 20000LL * (turns + 1);
			for (long long calls = 0; reason.empty() && turn < target; ++calls)
			{
				if (calls > maxCalls) { reason = Format("turn %d: %lld gc_pump calls without reaching turn %d", turn, maxCalls, target); break; }
				const int32_t prc = gc_pump(inst, 64, &pr, &err);
				if (pr.status == GC_PUMP_STUCK) { reason = Format("STUCK at turn %d: %s", pr.turn, err.message); break; }
				if (pr.status == GC_PUMP_GAME_OVER) { reason = Format("GAME_OVER at turn %d", pr.turn); break; }
				if (prc != GC_OK) { reason = Format("gc_pump at turn %d: %d %s", pr.turn, prc, err.message); break; }
				doneTurns += pr.turn - turn;
				turn = pr.turn;
			}
		}
	}
	if (reason.empty() && seen.luaErrors > 0) reason = Format("%d Lua error(s)%s%s", seen.luaErrors, seen.lines.empty() ? "" : ": ", seen.lines.empty() ? "" : seen.lines[0].c_str());
	if (reason.empty() && seen.logErrors > 0) reason = Format("%d logged error(s)%s%s", seen.logErrors, seen.lines.empty() ? "" : ": ", seen.lines.empty() ? "" : seen.lines[0].c_str());
	const bool ok = reason.empty() && doneTurns == (int)turns;
	if (!ok && reason.empty()) reason = Format("%d of %u turns", doneTurns, turns);
	for (size_t i = 0; i < seen.lines.size(); ++i) printf("mapscripts-event|%s\n", seen.lines[i].c_str());

	const std::string out = Format("{\"failed\":%d,\"script\":\"%s\",\"world_size\":\"%s\",\"seed\":%u,\"ai\":%u,\"started\":%s,\"width\":%d,\"height\":%d,\"civs_started\":%d,\"turns_requested\":%u,\"turns_done\":%d,"
		"\"lua_errors\":%d,\"log_errors\":%d,\"new_game_ms\":%.0f,\"total_ms\":%.0f,\"reason\":\"%s\"}", ok ? 0 : 1, Escape(script).c_str(), size.c_str(), seed, ai, started ? "true" : "false", width, height, startedCivs,
		turns, doneTurns, seen.luaErrors, seen.logErrors, newGameMs, NowMs() - t0, Escape(reason).c_str());
	if (report && report_len)
	{
		strncpy(report, out.c_str(), report_len - 1);
		report[report_len - 1] = '\0';
	}
	printf("mapscripts-summary|%s|%s|%s|map %dx%d|civs %d/%d|turns %d/%u|new game %.0f ms|%s\n", ok ? "PASS" : "FAIL", script.c_str(), size.c_str(), width, height, startedCivs, wantCivs, doneTurns, turns, newGameMs,
		reason.c_str());
	if (inst) gc_destroy(inst);
	return ok ? 0 : 1;
}
