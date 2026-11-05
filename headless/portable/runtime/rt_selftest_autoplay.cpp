// Scenario `autoplay` (task F8, milestone M4; run.sh --scenario autoplay --turns N --seeds S,.. --ai A,..; one process per seed x AI count):
// one game through the C ABI only, as a host would play it: gc_create, gc_new_game({"seed":S,"ai_count":A,"autoplay_human":1}), then
// gc_pump until the game turn is start + N. With autoplay_human the test bot (rt_autoplay) plays the human through gc_apply_command;
// research, policies and the other choices are the auto-choices (SIMP-002), AI diplomacy is auto-declined (SIMP-003).
// Checks: N turns done, no STUCK, no GAME_OVER, no gc_pump error, 0 Lua errors (LUA_ERROR events), every AI diplomacy message answered,
// the bot founded a city and gave commands. Diplomacy requests are counted (the matrix needs at least one over all runs: run.sh).
// "autoplay-turn|turn|ms|digest" lines (gc_state_digest(-1) after every turn change) let two targets / processes be compared.
#include "CvGameCoreDLLPCH.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <string>
#include <vector>

#include "../abi/gc_internal.h"
#include "rt_autoplay.h"
#include "rt_diplo.h"
#include "rt_selftest.h"
#include "rt_session.h"

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

std::string Format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string Format(const char* fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return buf;
}

struct Check
{
	std::string json;
	int failed;
	Check() : failed(0) {}

	void add(const char* name, bool ok, const char* fmt, ...) __attribute__((format(printf, 4, 5)))
	{
		char detail[1024];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(detail, sizeof(detail), fmt, ap);
		va_end(ap);
		json += std::string(json.empty() ? "" : ",") + "{\"check\":\"" + name + "\",\"ok\":" + (ok ? "true" : "false") + ",\"detail\":\"" + Escape(detail) + "\"}";
		printf("autoplay: %-4s %s: %s\n", ok ? "ok" : "FAIL", name, detail);
		if (!ok) ++failed;
	}
};

double NowMs()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

// What the host callbacks saw.
struct Seen
{
	const gc_host* outer;
	int logErrors, logWarnings;
	int events[16];
	int diploDeclined;   // DIPLO_AUTO_DECLINED
	int luaErrors;       // LUA_ERROR
	int autoChoices;     // AUTO_CHOICE
	int warOnHuman;      // WAR_STATE events with at_war = 1 that involve the human's team
	int humanTeam;
	std::vector<std::string> lines;  // the first diplomacy / Lua error / war texts
	Seen() : outer(NULL), logErrors(0), logWarnings(0), diploDeclined(0), luaErrors(0), autoChoices(0), warOnHuman(0), humanTeam(-1) { memset(events, 0, sizeof(events)); }
};

void OnLog(void* user, int32_t level, const char* msg)
{
	Seen* s = static_cast<Seen*>(user);
	if (level == GC_LOG_ERROR) ++s->logErrors;
	if (level == GC_LOG_WARN) ++s->logWarnings;
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
	if (ev->kind >= 0 && ev->kind < 16) ++s->events[ev->kind];
	if (ev->kind == GC_EVENT_DIPLO_AUTO_DECLINED)
	{
		++s->diploDeclined;
		if (s->lines.size() < 40) s->lines.push_back(Format("diplo AI %d state %d: %s", ev->argc > 0 ? ev->args[0] : -1, ev->argc > 1 ? ev->args[1] : -1, ev->text2 ? ev->text2 : ""));
	}
	if (ev->kind == GC_EVENT_LUA_ERROR)
	{
		++s->luaErrors;
		if (s->lines.size() < 40) s->lines.push_back(std::string("lua error: ") + (ev->text ? ev->text : ""));
	}
	if (ev->kind == GC_EVENT_AUTO_CHOICE) ++s->autoChoices;
	if (ev->kind == GC_EVENT_WAR_STATE && ev->argc == 3 && ev->args[2] && s->humanTeam >= 0 && (ev->args[0] == s->humanTeam || ev->args[1] == s->humanTeam))
	{
		++s->warOnHuman;
		if (s->lines.size() < 40) s->lines.push_back(Format("war: teams %d and %d", ev->args[0], ev->args[1]));
	}
}

std::string Num(long long v) { return Format("%lld", v); }

}  // namespace

extern "C" int rt_selftest_autoplay(const gc_host* host, unsigned seed, unsigned turns, unsigned ai, char* report, size_t report_len)
{
	Check c;
	Seen seen;
	seen.outer = host;
	gc_host mine = {&seen, OnLog, OnReadFile, OnEvent};
	gc_error err;
	memset(&err, 0, sizeof(err));
	const double t0 = NowMs();

	gc_instance* inst = gc_create(&mine, &err);
	const double msCreate = NowMs() - t0;
	c.add("create", inst != NULL, "gc_create: %d %s (%.0f ms)", err.code, err.message, msCreate);

	int startTurn = -1, endTurn = -1, doneTurns = 0, stuck = 0, gameOver = 0, pumpErrors = 0, pumpCalls = 0, diploTotal = 0, diploRequests = 0, diploPending = -1;
	int humanCities = 0, humanUnits = 0, humanAlive = 0;
	long long slices = 0;
	double msNewGame = 0, msTurns = 0, msTurnMax = 0;
	std::string stopReason, setup, turnsJson, digestsJson;
	RtAutoplayStats bot;
	if (inst)
	{
		setup = Format("{\"game_type\":\"single\",\"seed\":%u,\"ai_count\":%u,\"autoplay_human\":1}", seed, ai);
		const double t1 = NowMs();
		const int32_t rc = gc_new_game(inst, setup.c_str(), &err);
		msNewGame = NowMs() - t1;
		c.add("new_game", rc == GC_OK, "%s: %d %s (%.0f ms)", setup.c_str(), rc, err.message, msNewGame);
		RtSession* session = InstanceSession(inst);
		if (rc == GC_OK && session)
		{
			const PlayerTypes me = session->Engine().GetActivePlayer();
			seen.humanTeam = me != NO_PLAYER ? (int)GET_PLAYER(me).getTeam() : -1;
			gc_pump_result pr;
			memset(&pr, 0, sizeof(pr));
			startTurn = GC.getGame().getGameTurn();
			int turn = startTurn;
			const int target = startTurn + (int)turns;
			const long long maxCalls = 20000LL * (turns + 1);
			double tTurn = NowMs();
			const double tStart = tTurn;
			while (turn < target)
			{
				if (++pumpCalls > maxCalls)
				{
					stopReason = Format("turn %d: %lld gc_pump calls without reaching turn %d", turn, maxCalls, target);
					break;
				}
				const int32_t prc = gc_pump(inst, 64, &pr, &err);
				slices += pr.slices_used;
				if (pr.status == GC_PUMP_STUCK) { ++stuck; stopReason = Format("STUCK at turn %d: %s", pr.turn, err.message); break; }
				if (pr.status == GC_PUMP_GAME_OVER) { ++gameOver; stopReason = Format("GAME_OVER at turn %d", pr.turn); break; }
				if (prc != GC_OK) { ++pumpErrors; stopReason = Format("gc_pump at turn %d: %d %s", pr.turn, prc, err.message); break; }
				if (pr.status == GC_PUMP_WAITING_HUMAN) { stopReason = Format("WAITING_HUMAN at turn %d with autoplay_human (the bot did not take the turn)", pr.turn); break; }
				if (pr.turn != turn)
				{
					const double now = NowMs();
					const double ms = now - tTurn;
					tTurn = now;
					if (ms > msTurnMax) msTurnMax = ms;
					uint64_t digest = 0;
					gc_state_digest(inst, -1, &digest, &err);
					const std::string hex = Format("%016llx", (unsigned long long)digest);
					printf("autoplay-turn|%d|%.1f|%s\n", pr.turn, ms, hex.c_str());
					turnsJson += std::string(turnsJson.empty() ? "" : ",") + Format("%.1f", ms);
					digestsJson += std::string(digestsJson.empty() ? "" : ",") + "\"" + hex + "\"";
					doneTurns += pr.turn - turn;
					turn = pr.turn;
				}
			}
			msTurns = NowMs() - tStart;
			endTurn = turn;
			RtDiplo& dp = session->Diplo();
			diploTotal = dp.Total();
			diploPending = dp.Pending();
			const std::vector<RtDiploEvent>& evs = dp.Events();
			for (size_t i = 0; i < evs.size(); ++i)
			{
				if (evs[i].request) ++diploRequests;
				if (i < 30) printf("autoplay-diplo|%s\n", RtDiplo::Describe(evs[i]).c_str());
			}
			if (me != NO_PLAYER)
			{
				CvPlayer& p = GET_PLAYER(me);
				humanAlive = p.isAlive() ? 1 : 0;
				humanCities = p.getNumCities();
				humanUnits = p.getNumUnits();
			}
		}
		const RtAutoplay* ap = InstanceAutoplay(inst);
		if (ap) bot = ap->Stats();
	}
	for (size_t i = 0; i < seen.lines.size(); ++i) printf("autoplay-event|%s\n", seen.lines[i].c_str());

	c.add("turns", doneTurns == (int)turns && stopReason.empty(), "%d/%u turn(s) (turn %d -> %d), %d gc_pump call(s), %lld slice(s)%s%s", doneTurns, turns, startTurn, endTurn, pumpCalls, slices,
		stopReason.empty() ? "" : ": ", stopReason.c_str());
	c.add("no_stuck", stuck == 0 && gameOver == 0 && pumpErrors == 0, "%d STUCK, %d GAME_OVER, %d gc_pump error(s)", stuck, gameOver, pumpErrors);
	c.add("lua_errors", seen.luaErrors == 0 && seen.events[GC_EVENT_LUA_ERROR] == 0, "%d LUA_ERROR event(s)", seen.luaErrors);
	c.add("log_errors", seen.logErrors == 0, "%d error(s), %d warning(s) logged", seen.logErrors, seen.logWarnings);
	c.add("diplo_answered", diploPending == 0, "%d AI diplomacy message(s) to the human (%d requests), %d declined (DIPLO_AUTO_DECLINED), %d unanswered at the end", diploTotal, diploRequests,
		seen.diploDeclined, diploPending);
	// bot.turns counts accepted END_TURN / FORCE_END_TURN; it can exceed the turns done when the gamecore takes an ended turn back (CvGame::UpdateMoves
	// sends sendTurnUnready when a unit of the human is ready again, e.g. an explorer whose automation ran out), and the bot ends it once more.
	c.add("bot_played", bot.founded > 0 && bot.commands > 0 && bot.turns >= doneTurns, "the bot ended %d turn(s) (%d forced, %d taken back by the gamecore%s%s), %d command(s) accepted, %d refused, %d city(ies) founded, %d attack(s), %d production choice(s), %d round(s) (at most %d in one turn)%s%s",
		bot.turns, bot.forced, bot.retaken, bot.firstRetaken.empty() ? "" : ", first: ", bot.firstRetaken.c_str(), bot.commands, bot.rejected, bot.founded, bot.attacks, bot.productions, bot.rounds,
		bot.maxRoundsPerTurn, bot.firstReject.empty() ? "" : "; first refusal: ", bot.firstReject.c_str());
	c.add("human_alive", humanAlive == 1 && humanCities > 0, "the human is alive=%d with %d city(ies) and %d unit(s); %d war declaration(s) involving the human", humanAlive, humanCities, humanUnits, seen.warOnHuman);

	std::string kinds;
	for (int k = 0; k < 48; ++k)
		if (bot.byKind[k]) kinds += Format("%s\"%d\":%d", kinds.empty() ? "" : ",", k, bot.byKind[k]);
	const double msTotal = NowMs() - t0;
	const std::string out = "{\"failed\":" + Num(c.failed) + ",\"seed\":" + Num(seed) + ",\"ai_count\":" + Num(ai) + ",\"turns_requested\":" + Num(turns) +
		",\"turns_done\":" + Num(doneTurns) + ",\"start_turn\":" + Num(startTurn) + ",\"end_turn\":" + Num(endTurn) + ",\"stuck\":" + Num(stuck) + ",\"game_over\":" + Num(gameOver) +
		",\"log_errors\":" + Num(seen.logErrors) + ",\"lua_errors\":" + Num(seen.luaErrors) + ",\"diplo_total\":" + Num(diploTotal) + ",\"diplo_requests\":" + Num(diploRequests) +
		",\"diplo_declined\":" + Num(seen.diploDeclined) + ",\"diplo_pending\":" + Num(diploPending) + ",\"auto_choices\":" + Num(seen.autoChoices) + ",\"war_on_human\":" + Num(seen.warOnHuman) +
		",\"human\":{\"alive\":" + Num(humanAlive) + ",\"cities\":" + Num(humanCities) + ",\"units\":" + Num(humanUnits) + "}" +
		",\"bot\":{\"turns\":" + Num(bot.turns) + ",\"forced\":" + Num(bot.forced) + ",\"commands\":" + Num(bot.commands) + ",\"rejected\":" + Num(bot.rejected) + ",\"founded\":" + Num(bot.founded) +
		",\"attacks\":" + Num(bot.attacks) + ",\"productions\":" + Num(bot.productions) + ",\"rounds\":" + Num(bot.rounds) + ",\"max_rounds_per_turn\":" + Num(bot.maxRoundsPerTurn) + ",\"retaken\":" + Num(bot.retaken) +
		",\"first_retaken\":\"" + Escape(bot.firstRetaken) + "\"" +
		",\"by_kind\":{" + kinds + "},\"first_reject\":\"" + Escape(bot.firstReject) + "\"}" +
		",\"ms\":{\"create\":" + Format("%.1f", msCreate) + ",\"new_game\":" + Format("%.1f", msNewGame) + ",\"turns_total\":" + Format("%.1f", msTurns) + ",\"turn_max\":" + Format("%.1f", msTurnMax) +
		",\"total\":" + Format("%.1f", msTotal) + "},\"stop_reason\":\"" + Escape(stopReason) + "\",\"turn_ms\":[" + turnsJson + "],\"digests\":[" + digestsJson + "],\"checks\":[" + c.json + "]}";
	if (report && report_len)
	{
		strncpy(report, out.c_str(), report_len - 1);
		report[report_len - 1] = '\0';
	}
	printf("autoplay-summary|seed %u|ai %u|turns %d|stuck %d|log errors %d|lua errors %d|diplo %d (requests %d, declined %d)|bot commands %d refused %d founded %d attacks %d forced %d|cities %d|ms %.0f\n",
		seed, ai, doneTurns, stuck, seen.logErrors, seen.luaErrors, diploTotal, diploRequests, seen.diploDeclined, bot.commands, bot.rejected, bot.founded, bot.attacks, bot.forced, humanCities, msTotal);
	gc_destroy(inst);
	return c.failed == 0 ? 0 : 1;
}
