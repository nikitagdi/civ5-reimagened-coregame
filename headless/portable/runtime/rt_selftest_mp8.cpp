// Scenario `mp8` (S4, task N3.17; run.sh --scenario mp8 [--humans 8] [--ai 2] [--world-size WORLDSIZE_HUGE] --turns T; Linux only): the speed of a
// server with many humans. A network game (Pitboss, simultaneous turns, humans played by test bots) on Huge (default) with 8 humans + 2 AI, through the C ABI, T turns
// (the card: 30 for the start-up check, 150 for the measurements; the numbers are reported per turn window 1-30, 31-100, 101+).
// Measured (BUILD AC-N2.5, AC-N5.4; the numbers go to the orchestrator, the checks only fail on errors):
//  - the cost of one command: gc_apply_command (apply) and the step that follows it (RtSession::Pump with one slice = the core's Update of the queued command), per human;
//  - what a server step needs on top: the views of all human teams after a command (Q_PLOTS_DIRTY, Q_PLOTS of the dirty plots, Q_UNITS, Q_CITIES, Q_PLAYERS per team,
//    Q_TOP_BAR, Q_NOTIFICATIONS per human), summed = the AC-N2.5 step (apply + step + views of all humans); one command on its own and 8 commands with one shared step;
//  - Q_PLOTS_DIRTY after a whole turn (the set of the AI phase) and after a probe step;
//  - the AI turn (the gc_pump calls made while the human phase has not started, and the call that changes the turn: the AI civs move there) and the human phase (the bots rounds and steps) per turn, p50 / p95 / max per window.
// Probes run every 5th turn in the human phase. The commands are UNIT_SKIP of one unit per human (harmless: the unit does nothing this turn).
#include "CvGameCoreDLLPCH.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <string>
#include <vector>

#include "../abi/gc_internal.h"
#include "rt_active.h"
#include "rt_autoplay.h"
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

std::string Num(long long v) { return Format("%lld", v); }

struct Check
{
	std::string json;
	int failed;
	Check() : failed(0) {}
	void add(const char* name, bool ok, const char* fmt, ...) __attribute__((format(printf, 4, 5)))
	{
		char detail[2048];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(detail, sizeof(detail), fmt, ap);
		va_end(ap);
		json += std::string(json.empty() ? "" : ",") + "{\"check\":\"" + name + "\",\"ok\":" + (ok ? "true" : "false") + ",\"detail\":\"" + Escape(detail) + "\"}";
		printf("mp8: %-4s %s: %s\n", ok ? "ok" : "FAIL", name, detail);
		if (!ok) ++failed;
	}
};

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
	if (level == GC_LOG_ERROR) { ++s->logErrors; if (s->lines.size() < 20) s->lines.push_back(std::string("log error: ") + msg); }
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
		if (s->lines.size() < 20) s->lines.push_back(std::string("lua error: ") + (ev->text ? ev->text : ""));
	}
}

double NowMs()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

double Percentile(std::vector<double> v, double q)
{
	if (v.empty()) return 0;
	std::sort(v.begin(), v.end());
	size_t i = (size_t)(q * (double)(v.size() - 1) + 0.5);
	if (i >= v.size()) i = v.size() - 1;
	return v[i];
}

std::string Stat(const std::vector<double>& v)
{
	return Format("n %d p50 %.2f p95 %.2f max %.2f", (int)v.size(), Percentile(v, 0.5), Percentile(v, 0.95), Percentile(v, 1.0));
}

std::string StatJson(const std::vector<double>& v)
{
	return Format("{\"n\":%d,\"p50\":%.2f,\"p95\":%.2f,\"max\":%.2f}", (int)v.size(), Percentile(v, 0.5), Percentile(v, 0.95), Percentile(v, 1.0));
}

struct Reply
{
	gc_buf buf;
	gcq_header h;
	int32_t rc;
	Reply() : rc(-99) { memset(&buf, 0, sizeof(buf)); memset(&h, 0, sizeof(h)); }
	~Reply() { gc_buf_release(&buf); }
	bool Ask(gc_instance* inst, int kind, const int32_t* args, unsigned argc)
	{
		gc_buf_release(&buf);
		memset(&h, 0, sizeof(h));
		gc_error e;
		memset(&e, 0, sizeof(e));
		rc = gc_query(inst, kind, args, argc, &buf, &e);
		if (rc != GC_OK || buf.len < sizeof(h)) return false;
		memcpy(&h, buf.data, sizeof(h));
		return h.kind == kind;
	}
	const uint8_t* Rec(uint32_t i) const { return buf.data + sizeof(h) + (size_t)i * h.record_size; }
};

// A query timed into `ms`; false when it failed.
bool Timed(gc_instance* inst, Reply& r, int kind, const int32_t* args, unsigned argc, double& ms)
{
	const double t = NowMs();
	const bool ok = r.Ask(inst, kind, args, argc);
	ms += NowMs() - t;
	return ok;
}

struct Probe
{
	std::vector<double> apply, step, viewsOne, totalOne, dirty, applyAll, stepAll, viewsAll, totalAll;
	std::vector<double> viewKind[6];  // dirty plots, units, cities, players, top bar, notifications: per team / human
	int commands, refused, viewFail, probeTurns, humansWithData, dirtyPlotsSum;
	std::string firstFail;
	Probe() : commands(0), refused(0), viewFail(0), probeTurns(0), humansWithData(0), dirtyPlotsSum(0) {}
};

gc_command SkipCmd(int player, int unit, int32_t* args)
{
	args[0] = unit;
	gc_command c;
	memset(&c, 0, sizeof(c));
	c.kind = GC_CMD_UNIT_SKIP;
	c.player = (uint8_t)player;
	c.argc = 1;
	c.args = args;
	return c;
}

// The id of a unit of the player that can take UNIT_SKIP now, or -1.
int PickUnit(int player, int skip)
{
	CvPlayer& p = GET_PLAYER((PlayerTypes)player);
	int loop, seen = 0;
	for (CvUnit* u = p.firstUnit(&loop); u; u = p.nextUnit(&loop))
	{
		if (u->isDelayedDeath() || u->IsBusy() || !u->canMove()) continue;
		if (seen++ < skip) continue;
		return (int)u->GetID();
	}
	return -1;
}

// The views a server needs for the humans after a step: the dirty plots (Q_PLOTS_DIRTY, then Q_PLOTS of those plots per team), Q_UNITS, Q_CITIES, Q_PLAYERS per team,
// Q_TOP_BAR and Q_NOTIFICATIONS per human. kindMs[0..5] gets the time of each part, summed over the humans; the dirty query itself is returned in dirtyMs.
void Views(gc_instance* inst, const std::vector<int>& humans, const std::vector<int>& teams, double* kindMs, double& dirtyMs, int& dirtyPlots, Probe& pr)
{
	Reply d;
	dirtyMs = 0;
	std::vector<int32_t> idx;
	if (Timed(inst, d, GC_Q_PLOTS_DIRTY, NULL, 0, dirtyMs) && d.h.record_size >= sizeof(gcq_plot_dirty))
	{
		dirtyPlots = (int)d.h.count;
		for (uint32_t i = 0; i < d.h.count && idx.size() < 400; ++i)
		{
			gcq_plot_dirty rec;
			memcpy(&rec, d.Rec(i), sizeof(rec));
			idx.push_back((int32_t)rec.index);
		}
	}
	else { ++pr.viewFail; if (pr.firstFail.empty()) pr.firstFail = Format("Q_PLOTS_DIRTY rc %d", d.rc); }
	for (size_t k = 0; k < humans.size(); ++k)
	{
		Reply r;
		const int32_t team = teams[k], player = humans[k];
		std::vector<int32_t> a(1, team);
		a.insert(a.end(), idx.begin(), idx.end());
		if (!idx.empty() && !Timed(inst, r, GC_Q_PLOTS, &a[0], (unsigned)a.size(), kindMs[0])) { ++pr.viewFail; if (pr.firstFail.empty()) pr.firstFail = Format("Q_PLOTS(team %d, %d idx) rc %d", team, (int)idx.size(), r.rc); }
		if (!Timed(inst, r, GC_Q_UNITS, &team, 1, kindMs[1])) { ++pr.viewFail; if (pr.firstFail.empty()) pr.firstFail = Format("Q_UNITS(%d) rc %d", team, r.rc); }
		else if (r.h.count > 0) ++pr.humansWithData;
		if (!Timed(inst, r, GC_Q_CITIES, &team, 1, kindMs[2])) { ++pr.viewFail; if (pr.firstFail.empty()) pr.firstFail = Format("Q_CITIES(%d) rc %d", team, r.rc); }
		if (!Timed(inst, r, GC_Q_PLAYERS, &team, 1, kindMs[3])) { ++pr.viewFail; if (pr.firstFail.empty()) pr.firstFail = Format("Q_PLAYERS(%d) rc %d", team, r.rc); }
		if (!Timed(inst, r, GC_Q_TOP_BAR, &player, 1, kindMs[4])) { ++pr.viewFail; if (pr.firstFail.empty()) pr.firstFail = Format("Q_TOP_BAR(%d) rc %d", player, r.rc); }
		if (!Timed(inst, r, GC_Q_NOTIFICATIONS, &player, 1, kindMs[5])) { ++pr.viewFail; if (pr.firstFail.empty()) pr.firstFail = Format("Q_NOTIFICATIONS(%d) rc %d", player, r.rc); }
	}
}

// One probe: every human gets one command with its own step and views (the AC-N2.5 step), then all humans one command each with one shared step.
void RunProbe(gc_instance* inst, RtSession* session, const std::vector<int>& humans, const std::vector<int>& teams, Probe& pr)
{
	++pr.probeTurns;
	gc_error err;
	double allApply = 0, allStep = 0;
	int issued = 0;
	for (int pass = 0; pass < 2; ++pass)
	{
		for (size_t k = 0; k < humans.size(); ++k)
		{
			const int who = humans[k];
			if (!session->IsHumanWaiting((PlayerTypes)who)) continue;
			const int unit = PickUnit(who, pass);
			if (unit < 0) continue;
			int32_t a[1];
			gc_command cmd = SkipCmd(who, unit, a);
			memset(&err, 0, sizeof(err));
			double t = NowMs();
			const int32_t rc = gc_apply_command(inst, &cmd, &err);
			const double applyMs = NowMs() - t;
			if (rc != GC_OK) { ++pr.refused; continue; }
			++pr.commands;
			if (pass == 0)
			{
				// One command with its own step and views.
				t = NowMs();
				session->Pump(1, false);
				const double stepMs = NowMs() - t;
				double kindMs[6] = {0, 0, 0, 0, 0, 0}, dirtyMs = 0;
				int dirtyPlots = 0;
				Views(inst, humans, teams, kindMs, dirtyMs, dirtyPlots, pr);
				double views = dirtyMs;
				for (int i = 0; i < 6; ++i) { views += kindMs[i]; pr.viewKind[i].push_back(kindMs[i]); }
				pr.apply.push_back(applyMs);
				pr.step.push_back(stepMs);
				pr.dirty.push_back(dirtyMs);
				pr.viewsOne.push_back(views);
				pr.totalOne.push_back(applyMs + stepMs + views);
				pr.dirtyPlotsSum += dirtyPlots;
			}
			else
			{
				// Several commands, one shared step (the batch a server could make with gc_deliver: apply all, one Update).
				allApply += applyMs;
				++issued;
			}
		}
		if (pass == 1 && issued > 0)
		{
			double t = NowMs();
			session->Pump(1, false);
			allStep = NowMs() - t;
			double kindMs[6] = {0, 0, 0, 0, 0, 0}, dirtyMs = 0;
			int dirtyPlots = 0;
			Views(inst, humans, teams, kindMs, dirtyMs, dirtyPlots, pr);
			double views = dirtyMs;
			for (int i = 0; i < 6; ++i) views += kindMs[i];
			pr.applyAll.push_back(allApply);
			pr.stepAll.push_back(allStep);
			pr.viewsAll.push_back(views);
			pr.totalAll.push_back(allApply + allStep + views);
		}
	}
}

}  // namespace

extern "C" int rt_selftest_mp8(const gc_host* host, unsigned seed, unsigned turns, unsigned humans, unsigned ai, const char* world_size, char* report, size_t report_len)
{
	Check c;
	Seen seen;
	seen.outer = host;
	gc_host mine = {&seen, OnLog, OnReadFile, OnEvent};
	gc_error err;
	memset(&err, 0, sizeof(err));
	const double t0 = NowMs();
	const std::string size = world_size && *world_size ? world_size : "WORLDSIZE_HUGE";

	gc_instance* inst = gc_create(&mine, &err);
	c.add("create", inst != NULL, "gc_create: %d %s", err.code, err.message);

	int startTurn = -1, endTurn = -1, doneTurns = 0, stuck = 0, pumpErrors = 0, calls = 0, gameOver = 0, activeBroken = 0, seat = -1;
	std::string stopReason, setup;
	std::vector<double> aiMs, humanMs, totalMs, turnDirtyMs, turnDirtyCount;  // per turn
	std::vector<int> turnNo;
	std::vector<int> humanList, teams;
	Probe probe;
	size_t plotCount = 0;
	std::string bots;
	int botMinTurns = 1 << 30;
	if (inst)
	{
		std::string players;
		for (unsigned i = 0; i < humans; ++i)
			players += Format("%s{\"kind\":\"human\",\"civ\":\"%s\",\"nick\":\"bot%u\"}", i ? "," : "", i ? "random" : "CIVILIZATION_ROME", i);
		for (unsigned i = 0; i < ai; ++i) players += std::string(humans || i ? "," : "") + "{\"kind\":\"ai\",\"civ\":\"random\"}";
		std::string slots;
		for (unsigned i = 0; i < humans; ++i) slots += Format("%s%u", i ? "," : "", i);
		setup = Format("{\"seed\":%u,\"game_type\":\"network\",\"players\":[%s],\"turn_mode\":\"simultaneous\",\"autoplay_humans\":[%s],\"handicap\":\"HANDICAP_CHIEFTAIN\",\"world_size\":\"%s\"}", seed,
			players.c_str(), slots.c_str(), size.c_str());
		const double t1 = NowMs();
		const int32_t rc = gc_new_game(inst, setup.c_str(), &err);
		c.add("new_game", rc == GC_OK, "%s: %d %s (%.0f ms)", setup.c_str(), rc, err.message, NowMs() - t1);
		RtSession* session = InstanceSession(inst);
		if (rc == GC_OK && session)
		{
			CvGame& game = GC.getGame();
			plotCount = (size_t)GC.getMap().numPlots();
			seat = (int)ServerSeat();
			for (int i = 0; i < MAX_PLAYERS; ++i)
				if (IsSeatedHuman(i))
				{
					humanList.push_back(i);
					teams.push_back((int)GET_PLAYER((PlayerTypes)i).getTeam());
				}
			c.add("setup", game.isNetworkMultiPlayer() && humanList.size() == humans, "%dx%d = %u plots (%s), %d human(s) in %d team(s), %u AI, server seat %d", (int)GC.getMap().getGridWidth(),
				(int)GC.getMap().getGridHeight(), (unsigned)plotCount, size.c_str(), (int)humanList.size(), (int)teams.size(), ai, seat);

			gc_pump_result pr;
			memset(&pr, 0, sizeof(pr));
			startTurn = game.getGameTurn();
			int turn = startTurn;
			const int target = startTurn + (int)turns;
			const long long maxCalls = 20000LL * (turns + 1);
			double aiThisTurn = 0, humanThisTurn = 0;
			bool probedThisTurn = false;
			while (turn < target)
			{
				if (++calls > maxCalls) { stopReason = Format("turn %d: %lld gc_pump calls without reaching turn %d", turn, maxCalls, target); break; }
				const bool humanPhaseBefore = session->HumanPhase();
				const double tc = NowMs();
				const int32_t prc = gc_pump(inst, 64, &pr, &err);
				const double dt = NowMs() - tc;
				if (CvPreGame::activePlayer() != seat) ++activeBroken;
				if (pr.status == GC_PUMP_STUCK) { ++stuck; stopReason = Format("STUCK at turn %d: %s", pr.turn, err.message); break; }
				if (pr.status == GC_PUMP_GAME_OVER) { ++gameOver; stopReason = Format("GAME_OVER at turn %d", pr.turn); break; }
				if (prc != GC_OK) { ++pumpErrors; stopReason = Format("gc_pump at turn %d: %d %s", pr.turn, prc, err.message); break; }

				if (pr.turn == turn) { if (humanPhaseBefore) humanThisTurn += dt; else aiThisTurn += dt; }
				if (pr.turn != turn)
				{
					aiMs.push_back(aiThisTurn);
					humanMs.push_back(humanThisTurn);
					totalMs.push_back(aiThisTurn + humanThisTurn);
					turnNo.push_back(turn - startTurn + 1);
					// Q_PLOTS_DIRTY after a whole turn (the set of the AI phase), outside the turn timing.
					{
						Reply d;
						double ms = 0;
						if (Timed(inst, d, GC_Q_PLOTS_DIRTY, NULL, 0, ms)) { turnDirtyMs.push_back(ms); turnDirtyCount.push_back((double)d.h.count); }
					}
					// The call that changed the turn is the processing between the turns (the humans ended, the AI civs moved, the new turn began): it opens the AI time of the next turn.
					aiThisTurn = dt;
					humanThisTurn = 0;
					printf("mp8-turn|%d|ai %.0f ms|humans %.0f ms|total %.0f ms\n", turn, aiMs.back(), humanMs.back(), totalMs.back());
					doneTurns += pr.turn - turn;
					turn = pr.turn;
					probedThisTurn = false;
					if (turn >= target) break;
				}
				// A probe in the human phase, every 5th turn, once the bots have a round pending (the humans are waiting).
				if (!probedThisTurn && session->HumanPhase() && ((turn - startTurn) % 5 == 0) && !session->WaitingHumans().empty())
				{
					probedThisTurn = true;
					RunProbe(inst, session, humanList, teams, probe);  // not part of the turn time (it is not made through gc_pump)
				}
			}
			endTurn = turn;
			for (size_t h = 0; h < humanList.size(); ++h)
			{
				const RtAutoplay* b = InstanceBot(inst, humanList[h]);
				if (!b) continue;
				botMinTurns = std::min(botMinTurns, b->Stats().turns);
				bots += std::string(bots.empty() ? "" : ",") + "{\"player\":" + Num(humanList[h]) + ",\"turns\":" + Num(b->Stats().turns) + ",\"commands\":" + Num(b->Stats().commands) + ",\"rejected\":" + Num(b->Stats().rejected) + "}";
			}
		}
	}
	for (size_t i = 0; i < seen.lines.size(); ++i) printf("mp8-event|%s\n", seen.lines[i].c_str());
	if (botMinTurns == (1 << 30)) botMinTurns = 0;

	c.add("turns", doneTurns == (int)turns && stopReason.empty(), "%d/%u turn(s) (turn %d -> %d), %d gc_pump call(s)%s%s", doneTurns, turns, startTurn, endTurn, calls, stopReason.empty() ? "" : ": ", stopReason.c_str());
	c.add("no_stuck", stuck == 0 && gameOver == 0 && pumpErrors == 0, "%d STUCK, %d GAME_OVER, %d gc_pump error(s)", stuck, gameOver, pumpErrors);
	c.add("active_rule", activeBroken == 0, "%d time(s) the active player was not the server seat", activeBroken);
	c.add("lua_errors", seen.luaErrors == 0, "%d LUA_ERROR event(s)", seen.luaErrors);
	c.add("log_errors", seen.logErrors == 0, "%d error(s) logged", seen.logErrors);
	c.add("humans_played", !humanList.empty() && botMinTurns >= doneTurns, "the least active of %d human(s) ended %d of %d turn(s)", (int)humanList.size(), botMinTurns, doneTurns);
	c.add("views_per_human", turns < 5 || (probe.probeTurns > 0 && probe.viewFail == 0 && probe.humansWithData > 0),
		"%d probe turn(s), %d command(s) accepted, %d refused; every human got his Q_PLOTS / Q_UNITS / Q_CITIES / Q_PLAYERS / Q_TOP_BAR / Q_NOTIFICATIONS: %d failure(s)%s%s", probe.probeTurns, probe.commands,
		probe.refused, probe.viewFail, probe.firstFail.empty() ? "" : "; first: ", probe.firstFail.c_str());

	// The numbers. Windows by the turn since the start: 1-30, 31-100, 101 and later.
	const int bounds[4] = {0, 30, 100, 1 << 30};
	std::string windowsJson;
	for (int w = 0; w < 3; ++w)
	{
		std::vector<double> a, h, t;
		for (size_t i = 0; i < turnNo.size(); ++i)
			if (turnNo[i] > bounds[w] && turnNo[i] <= bounds[w + 1]) { a.push_back(aiMs[i]); h.push_back(humanMs[i]); t.push_back(totalMs[i]); }
		if (a.empty()) continue;
		const std::string label = w == 0 ? "turns 1-30" : (w == 1 ? "turns 31-100" : "turns 101+");
		printf("mp8-ai-turn|%s|%s|%s (ms; the AI phase = gc_pump calls before the human phase)\n", label.c_str(), size.c_str(), Stat(a).c_str());
		printf("mp8-human-phase|%s|%s|%s (ms; bots' rounds + steps, all humans)\n", label.c_str(), size.c_str(), Stat(h).c_str());
		printf("mp8-turn-total|%s|%s|%s (ms)\n", label.c_str(), size.c_str(), Stat(t).c_str());
		windowsJson += std::string(windowsJson.empty() ? "" : ",") + "{\"window\":\"" + label + "\",\"ai_ms\":" + StatJson(a) + ",\"human_phase_ms\":" + StatJson(h) + ",\"turn_ms\":" + StatJson(t) + "}";
	}
	printf("mp8-cmd|apply|%s ms (gc_apply_command of UNIT_SKIP)\n", Stat(probe.apply).c_str());
	printf("mp8-cmd|step|%s ms (one Pump slice that carries the queued command = the core's Update)\n", Stat(probe.step).c_str());
	const char* kindName[6] = {"Q_PLOTS(dirty plots)", "Q_UNITS", "Q_CITIES", "Q_PLAYERS", "Q_TOP_BAR", "Q_NOTIFICATIONS"};
	for (int i = 0; i < 6; ++i) printf("mp8-views|%s x %d human(s)|%s ms\n", kindName[i], (int)humanList.size(), Stat(probe.viewKind[i]).c_str());
	printf("mp8-views|Q_PLOTS_DIRTY after a step|%s ms (avg %.1f plots)\n", Stat(probe.dirty).c_str(), probe.dirty.empty() ? 0.0 : (double)probe.dirtyPlotsSum / (double)probe.dirty.size());
	printf("mp8-views|all views of %d human(s) (dirty + the six above)|%s ms\n", (int)humanList.size(), Stat(probe.viewsOne).c_str());
	printf("mp8-step|AC-N2.5: apply + step + views of all humans, one command|%s ms\n", Stat(probe.totalOne).c_str());
	printf("mp8-step|%d commands (one each) + one shared step + views: apply %s; step %s; views %s; total %s ms\n", (int)humanList.size(), Stat(probe.applyAll).c_str(), Stat(probe.stepAll).c_str(),
		Stat(probe.viewsAll).c_str(), Stat(probe.totalAll).c_str());
	printf("mp8-dirty-turn|Q_PLOTS_DIRTY after a whole turn (the set of the AI phase)|%s ms; plots %s of %u\n", Stat(turnDirtyMs).c_str(), Stat(turnDirtyCount).c_str(), (unsigned)plotCount);
	{
		// The AC-N2.5 limit (preliminary): p95 <= 60 ms on Huge with 8 humans, <= 20 ms on Duel with 4 humans. Anything else is judged against the 60 ms of the bigger case.
		const double limit = (size == "WORLDSIZE_DUEL" && humans <= 4) ? 20.0 : 60.0;
		const double p95 = Percentile(probe.totalOne, 0.95), stepP95 = Percentile(probe.step, 0.95), viewsP95 = Percentile(probe.viewsOne, 0.95), applyP95 = Percentile(probe.apply, 0.95);
		const bool over = p95 > limit;
		const bool stepDominates = stepP95 > viewsP95 && stepP95 > applyP95;
		printf("mp8-decision|AC-N2.5 p95 %.2f ms against the limit %.0f ms: %s; parts at p95: apply %.2f, step %.2f, views %.2f => %s\n", p95, limit, over ? "OVER" : "within",
			applyP95, stepP95, viewsP95,
			over ? (stepDominates ? "propose gc_deliver (a separate task): the Update per command is the dearest part" : "the views are the dearest part: dirty-based views (Q_PLOTS of dirty plots), not gc_deliver")
				 : (stepDominates ? "no change needed; the step is the biggest part but the budget holds (gc_deliver not needed)" : "no change needed"));
	}

	const std::string out = "{\"failed\":" + Num(c.failed) + ",\"seed\":" + Num(seed) + ",\"humans\":" + Num(humans) + ",\"ai\":" + Num(ai) + ",\"world_size\":\"" + size + "\",\"plots\":" + Num((long long)plotCount) +
		",\"turns_requested\":" + Num(turns) + ",\"turns_done\":" + Num(doneTurns) + ",\"start_turn\":" + Num(startTurn) + ",\"end_turn\":" + Num(endTurn) + ",\"stuck\":" + Num(stuck) +
		",\"log_errors\":" + Num(seen.logErrors) + ",\"lua_errors\":" + Num(seen.luaErrors) + ",\"windows\":[" + windowsJson + "],\"command\":{\"apply\":" + StatJson(probe.apply) + ",\"step\":" + StatJson(probe.step) +
		",\"views\":" + StatJson(probe.viewsOne) + ",\"total\":" + StatJson(probe.totalOne) + ",\"batch_total\":" + StatJson(probe.totalAll) + ",\"dirty_query\":" + StatJson(probe.dirty) +
		",\"dirty_whole_turn\":" + StatJson(turnDirtyMs) + ",\"commands\":" + Num(probe.commands) + ",\"refused\":" + Num(probe.refused) + "},\"total_ms\":" + Format("%.1f", NowMs() - t0) +
		",\"bots\":[" + bots + "],\"checks\":[" + c.json + "]}";
	if (report && report_len)
	{
		strncpy(report, out.c_str(), report_len - 1);
		report[report_len - 1] = '\0';
	}
	printf("mp8-summary|seed %u|humans %u|ai %u|%s|turns %d/%u|stuck %d|log errors %d|lua errors %d|%s\n", seed, humans, ai, size.c_str(), doneTurns, turns, stuck, seen.logErrors, seen.luaErrors, stopReason.c_str());
	if (inst) gc_destroy(inst);
	return c.failed == 0 ? 0 : 1;
}
