// Scenario `spike-net` (spike S1, task N1.1; run.sh --scenario spike-net --humans N --ai A --mode simultaneous|hybrid
// --turns T): does the gamecore play a network game (GAME_NETWORK_MULTIPLAYER, Pitboss, the server seat as active player) without the exe's
// network layer? RtSession::Load + NewGame({"game_type":"network","humans":N,"ai_count":A,"turn_mode":M}), then RtSession::Pump with the exe
// turn model of rt_net.cpp; at every WAITING_HUMAN the test bot (rt_autoplay, one per human) plays each waiting human under ActiveAs. Hybrid:
// at game turn 5 human 0 declares war on human 1 (they are made to meet first, a scenario-only call), then the two must never be turn-active
// at the same time.
// Measured (the S1 report): turns done, STUCK and its reason, exe methods called (RtEngine counters + the "stub:" log lines), messages nobody
// delivers, AI diplomacy towards each human, whether the humans' automated workers work, the AI turn's wall time, the active-player rule.
#include "CvGameCoreDLLPCH.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "CvDiplomacyRequests.h"
#include "../abi/gc_command.h"
#include "rt_active.h"
#include "rt_autoplay.h"
#include "rt_diplo.h"
#include "rt_lua.h"
#include "rt_selftest.h"
#include "rt_session.h"
#include "lekmod_portable/host_log.h"

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
		printf("spike-net: %-4s %s: %s\n", ok ? "ok" : "FAIL", name, detail);
		if (!ok) ++failed;
	}
};

// What the log sink saw: errors, warnings, the "stub: X" lines (each stub logs once).
struct Seen
{
	const gc_host* host;
	int errors, warnings;
	std::vector<std::string> stubs, firstErrors;
	Seen() : host(NULL), errors(0), warnings(0) {}
};

void Sink(void* user, int level, const char* msg)
{
	Seen* s = static_cast<Seen*>(user);
	if (level == LOG_ERROR) { ++s->errors; if (s->firstErrors.size() < 10) s->firstErrors.push_back(msg); }
	if (level == LOG_WARN) ++s->warnings;
	if (strncmp(msg, "stub: ", 6) == 0) s->stubs.push_back(msg + 6);
	if (s->host && s->host->log) s->host->log(s->host->user, level, msg);
}

double NowMs()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

std::string Num(long long v) { return Format("%lld", v); }
std::string Fix(double v) { return Format("%.1f", v); }

double Percentile(std::vector<double> v, double q)
{
	if (v.empty()) return 0;
	std::sort(v.begin(), v.end());
	size_t i = (size_t)(q * (double)(v.size() - 1) + 0.5);
	if (i >= v.size()) i = v.size() - 1;
	return v[i];
}

std::string CountMap(const std::map<std::string, int>& m)
{
	std::string s;
	for (std::map<std::string, int>::const_iterator it = m.begin(); it != m.end(); ++it) s += Format("%s\"%s\":%d", s.empty() ? "" : ",", it->first.c_str(), it->second);
	return "{" + s + "}";
}

bool IsWorker(const CvUnit* u) { return u && u->getUnitInfo().GetDefaultUnitAIType() == UNITAI_WORKER; }

// Per human: the automated workers at the start of each human phase, to see whether they move or build between two phases.
struct WorkerWatch
{
	std::map<int, std::pair<int, int> > lastPos;  // unit id -> plot at the previous snapshot
	int automatedTurns;    // (automated worker, turn) pairs seen
	int workingTurns;      // of those: moved since the previous snapshot or building now
	int maxAutomated;
	WorkerWatch() : automatedTurns(0), workingTurns(0), maxAutomated(0) {}
	void Snapshot(PlayerTypes who)
	{
		std::map<int, std::pair<int, int> > now;
		int loop, automated = 0;
		for (CvUnit* u = GET_PLAYER(who).firstUnit(&loop); u; u = GET_PLAYER(who).nextUnit(&loop))
		{
			if (!IsWorker(u) || !u->IsAutomated()) continue;
			++automated;
			now[u->GetID()] = std::make_pair(u->getX(), u->getY());
			std::map<int, std::pair<int, int> >::const_iterator it = lastPos.find(u->GetID());
			if (it == lastPos.end()) continue;
			++automatedTurns;
			if (it->second != now[u->GetID()] || u->getBuildType() != NO_BUILD) ++workingTurns;
		}
		maxAutomated = std::max(maxAutomated, automated);
		lastPos.swap(now);
	}
};

int Improvements(PlayerTypes who)
{
	int n = 0;
	CvMap& map = GC.getMap();
	for (int i = 0; i < map.numPlots(); ++i)
	{
		const CvPlot* p = map.plotByIndex(i);
		if (p && p->getOwner() == who && p->getImprovementType() != NO_IMPROVEMENT) ++n;
	}
	return n;
}

}  // namespace

extern "C" int rt_selftest_spikenet(const gc_host* host, unsigned seed, unsigned turns, unsigned humans, unsigned ai, const char* mode, const char* world_size, char* report, size_t report_len)
{
	Seen seen;
	seen.host = host;
	SetLogSink(Sink, &seen);
	Check c;
	const std::string turnMode = mode && *mode ? mode : "simultaneous";
	const bool hybrid = turnMode == "hybrid";

	// Left alive on purpose, as in scenario `load`.
	RtSession* session = new RtSession();
	RtSession::LoadStatus ls;
	const bool loaded = session->Load(*host, ls);
	c.add("load", loaded, "boot reached step %s%s%s", RtSession::StepName(ls.step), ls.error.empty() ? "" : ": ", ls.error.c_str());

	// P2 "players" array: `humans` human slots (the first Rome, the others random), then `ai` random AI slots.
	std::string players;
	for (unsigned i = 0; i < humans; ++i)
		players += Format("%s{\"kind\":\"human\",\"civ\":\"%s\",\"nick\":\"bot%u\"}", i ? "," : "", i ? "random" : "CIVILIZATION_ROME", i);
	for (unsigned i = 0; i < ai; ++i) players += std::string(humans || i ? "," : "") + "{\"kind\":\"ai\",\"civ\":\"random\"}";
	const std::string size = world_size && *world_size ? Format(",\"world_size\":\"%s\"", world_size) : std::string();
	const std::string setup = Format("{\"seed\":%u,\"game_type\":\"network\",\"players\":[%s],\"turn_mode\":\"%s\"%s}", seed, players.c_str(), turnMode.c_str(), size.c_str());
	RtSession::NewGameStatus st;
	const bool started = loaded && session->NewGame(*host, setup, st);
	c.add("new_game", started, "%s reached step %s (Init2 %.0f ms)%s%s", setup.c_str(), RtSession::NewGameStepName(st.step), st.msInit2, st.error.empty() ? "" : ": ", st.error.c_str());

	int startTurn = -1, endTurn = -1, doneTurns = 0, stuck = 0, gameOver = 0, pumps = 0, waitingSteps = 0, rounds = 0;
	int activeRuleBroken = 0, bothWarringActive = 0, warringSteps = 0, warTurn = -1, luaErrors = 0;
	int humanCount = 0, seat = -1, notYourTurn = 0, notYourTurnOk = 0;
	std::string stopReason, warHow, turnsJson, setupFacts, notYourTurnFirst;
	std::vector<double> aiMs, turnMs;
	std::vector<RtAutoplay> bots(MAX_PLAYERS);
	std::vector<WorkerWatch> workers(MAX_PLAYERS);
	std::vector<int> humanList, improvementsAt10(MAX_PLAYERS, -1), improvementsEnd(MAX_PLAYERS, 0), waitedFor(MAX_PLAYERS, 0);
	std::map<int, std::vector<int> > activeHumansByTurn;  // turn -> the sets of turn-active humans at WAITING steps, in order (bit k = humanList[k])
	std::vector<int> bothTurns;                           // hybrid: turns after the war with both warring humans turn-active at once
	if (started)
	{
		RtScriptSystem* ss = session->Scripts();
		const int luaBefore = ss ? ss->GetErrorCount() : 0;
		CvGame& game = GC.getGame();
		seat = (int)ServerSeat();
		for (int i = 0; i < MAX_PLAYERS; ++i)
		{
			const CvPlayer& p = GET_PLAYER((PlayerTypes)i);
			if (p.isAlive() && p.isHuman() && !p.isObserver()) humanList.push_back(i);
		}
		humanCount = (int)humanList.size();
		setupFacts = Format("game type %d (network %d), pitboss %d, simultaneous %d, dynamic %d, timer %d, server seat %d (observer %d), active %d, humans %d",
			(int)CvPreGame::gameType(), CvPreGame::isNetworkMultiplayerGame() ? 1 : 0, CvPreGame::isPitBoss() ? 1 : 0, game.isOption(GAMEOPTION_SIMULTANEOUS_TURNS) ? 1 : 0,
			game.isOption(GAMEOPTION_DYNAMIC_TURNS) ? 1 : 0, game.isOption(GAMEOPTION_END_TURN_TIMER_ENABLED) ? 1 : 0, seat,
			seat >= 0 && CvPreGame::slotStatus((PlayerTypes)seat) == SS_OBSERVER ? 1 : 0, (int)CvPreGame::activePlayer(), humanCount);
		c.add("network_setup", game.isNetworkMultiPlayer() && CvPreGame::isPitBoss() && seat >= 0 && CvPreGame::activePlayer() == seat && humanCount == (int)humans &&
			game.isOption(GAMEOPTION_DYNAMIC_TURNS) == hybrid && game.isOption(GAMEOPTION_SIMULTANEOUS_TURNS) == !hybrid, "%s", setupFacts.c_str());

		startTurn = game.getGameTurn();
		int turn = startTurn;
		const int target = startTurn + (int)turns;
		bool phaseSeen = false;  // the human phase of `turn` has been reached
		bool inHumans = false;   // the last pump ended in the human phase
		double tTurn = NowMs(), tAi = tTurn;  // tAi: start of the first pump after the human phase (the AI phase starts there)
		const long long maxPumps = 200000LL * (turns + 1);
		while (turn < target)
		{
			if (++pumps > maxPumps) { stopReason = Format("turn %d: %lld pumps without reaching turn %d", turn, maxPumps, target); break; }
			const double tPump = NowMs();
			const RtSession::PumpResult r = session->Pump(64);
			if (CvPreGame::activePlayer() != seat) ++activeRuleBroken;
			if (r.status == RtSession::PUMP_STUCK) { ++stuck; stopReason = Format("STUCK at turn %d: %s", r.turn, r.reason.c_str()); break; }
			if (r.status == RtSession::PUMP_GAME_OVER) { ++gameOver; stopReason = Format("GAME_OVER at turn %d", r.turn); break; }
			if (inHumans && (r.status != RtSession::PUMP_WAITING_HUMAN || r.turn != turn)) { tAi = tPump; inHumans = false; }
			if (r.turn != turn)
			{
				const double now = NowMs();
				turnMs.push_back(now - tTurn);
				printf("spikenet-turn|%d|%.1f ms|digest %016llx\n", r.turn, now - tTurn, (unsigned long long)RtSession::StateDigest());
				turnsJson += std::string(turnsJson.empty() ? "" : ",") + Fix(now - tTurn);
				tTurn = now;
				doneTurns += r.turn - turn;
				turn = r.turn;
				phaseSeen = false;
				if (turn - startTurn == 10)
					for (size_t h = 0; h < humanList.size(); ++h) improvementsAt10[humanList[h]] = Improvements((PlayerTypes)humanList[h]);
				if (turn >= target) break;
			}
			if (r.status != RtSession::PUMP_WAITING_HUMAN) continue;
			++waitingSteps;
			inHumans = true;
			if (!phaseSeen)
			{
				phaseSeen = true;
				if (turn > startTurn) aiMs.push_back(NowMs() - tAi);  // from the end of the humans' phase to the next one: AI turn + doTurn
				for (size_t h = 0; h < humanList.size(); ++h) workers[humanList[h]].Snapshot((PlayerTypes)humanList[h]);
			}
			// The sequence of turn-active human sets of this turn, as a code per set (bit k = humanList[k]), recorded when it changes.
			int code = 0;
			for (size_t h = 0; h < humanList.size(); ++h)
				if (GET_PLAYER((PlayerTypes)humanList[h]).isTurnActive()) code |= 1 << h;
			std::vector<int>& seq = activeHumansByTurn[turn];
			if (seq.empty() || seq.back() != code) seq.push_back(code);
			// The turn mode changes from the turn after the declaration (CvGame::doTurn, setDynamicTurnsSimultMode).
			if (hybrid && warTurn >= 0 && turn > warTurn && humanList.size() >= 2)
			{
				++warringSteps;
				// N3.4: a command of a human whose turn is not active (the other warring human's turn) is NOT_YOUR_TURN.
				for (size_t h = 0; h < humanList.size(); ++h)
				{
					const PlayerTypes who = (PlayerTypes)humanList[h];
					if (GET_PLAYER(who).isTurnActive() || session->Engine().TurnCompleteSent(who)) continue;
					gc_command probe;
					memset(&probe, 0, sizeof(probe));
					probe.kind = GC_CMD_END_TURN;
					probe.player = (uint8_t)who;
					std::string pmsg;
					int32_t preason = -1;
					const int32_t prc = ApplyHumanCommand(*session, probe, pmsg, &preason);
					++notYourTurn;
					if (prc == GC_ERR_REJECTED && preason == GC_REASON_NOT_YOUR_TURN) ++notYourTurnOk;
					else if (notYourTurnFirst.empty()) notYourTurnFirst = Format("turn %d player %d: rc %d reason %d \"%s\"", turn, (int)who, prc, preason, pmsg.c_str());
				}
				if ((code & 3) == 3)
				{
					++bothWarringActive;
					if (bothTurns.size() < 20) bothTurns.push_back(turn);
				}
			}
			// Hybrid: human 0 declares war on human 1 at game turn 5, through the command path (DECLARE_WAR under ActiveAs).
			if (hybrid && warTurn < 0 && turn >= 5 && humanList.size() >= 2 && session->IsHumanWaiting((PlayerTypes)humanList[0]))
			{
				const TeamTypes t0 = GET_PLAYER((PlayerTypes)humanList[0]).getTeam(), t1 = GET_PLAYER((PlayerTypes)humanList[1]).getTeam();
				const bool met = GET_TEAM(t0).isHasMet(t1);
				if (!met) GET_TEAM(t0).meet(t1, true);  // scenario-only: on a Duel map they may not have met by turn 5
				ActiveAs as((PlayerTypes)humanList[0]);
				const int32_t args[1] = {(int32_t)t1};
				gc_command cmd;
				memset(&cmd, 0, sizeof(cmd));
				cmd.kind = GC_CMD_DECLARE_WAR;
				cmd.player = (uint8_t)humanList[0];
				cmd.argc = 1;
				cmd.args = args;
				std::string msg;
				const int32_t rc = ApplyHumanCommand(*session, cmd, msg);
				warHow = Format("turn %d: teams %d -> %d (met before: %d): DECLARE_WAR rc %d %s", turn, (int)t0, (int)t1, met ? 1 : 0, rc, msg.c_str());
				if (rc != GC_OK)
				{
					GET_TEAM(t0).declareWar(t1);
					warHow += "; fallback CvTeam::declareWar";
				}
				warTurn = turn;
				printf("spikenet-war|%s\n", warHow.c_str());
				continue;  // the command is delivered by the next pump
			}
			// Every waiting human gets a bot round, the first one in turn (rotating); a human is skipped when the rounds before it left queued
			// commands (its END_TURN would be refused until the next pump delivers them: B1's IsHumanWaiting rule).
			const std::vector<PlayerTypes> waiting = session->WaitingHumans();
			for (size_t j = 0; j < waiting.size(); ++j)
			{
				const PlayerTypes who = waiting[(j + (size_t)waitingSteps) % waiting.size()];
				if (!session->IsHumanWaiting(who)) continue;
				++waitedFor[(int)who];
				std::string why;
				++rounds;
				if (!bots[(int)who].ActAs(*session, (int)who, why)) { stopReason = Format("turn %d human %d: %s", turn, (int)who, why.c_str()); break; }
				if (CvPreGame::activePlayer() != seat) ++activeRuleBroken;
			}
			if (!stopReason.empty()) break;
		}
		endTurn = turn;
		luaErrors = ss ? ss->GetErrorCount() - luaBefore : 0;
		for (size_t h = 0; h < humanList.size(); ++h) improvementsEnd[humanList[h]] = Improvements((PlayerTypes)humanList[h]);
	}

	// ---- report -------------------------------------------------------------------------------------------------------------
	const RtEngine& e = session->Engine();
	RtDiplo& dp = session->Diplo();
	std::map<int, int> diploTo, requestsTo;
	const std::vector<RtDiploEvent>& evs = dp.Events();
	for (size_t i = 0; i < evs.size(); ++i)
	{
		++diploTo[evs[i].to];
		if (evs[i].request) ++requestsTo[evs[i].to];
		if (i < 20) printf("spikenet-diplo|%s\n", RtDiplo::Describe(evs[i]).c_str());
	}
	std::string humansJson;
	int automatedTurns = 0, workingTurns = 0, botTurns = 0, botCommands = 0, pendingRequests = 0;
	for (size_t h = 0; h < humanList.size(); ++h)
	{
		const int p = humanList[h];
		const CvPlayer& pl = GET_PLAYER((PlayerTypes)p);
		const RtAutoplayStats& b = bots[p].Stats();
		const WorkerWatch& w = workers[p];
		const bool pending = pl.GetDiplomacyRequests() && pl.GetDiplomacyRequests()->HasPendingRequests();
		pendingRequests += pending ? 1 : 0;
		automatedTurns += w.automatedTurns;
		workingTurns += w.workingTurns;
		botTurns += b.turns;
		botCommands += b.commands;
		humansJson += std::string(humansJson.empty() ? "" : ",") + "{\"player\":" + Num(p) + ",\"alive\":" + Num(pl.isAlive() ? 1 : 0) + ",\"cities\":" + Num(pl.getNumCities()) +
			",\"units\":" + Num(pl.getNumUnits()) + ",\"waited_for\":" + Num(waitedFor[p]) + ",\"bot\":{\"turns\":" + Num(b.turns) + ",\"forced\":" + Num(b.forced) + ",\"commands\":" + Num(b.commands) +
			",\"rejected\":" + Num(b.rejected) + ",\"founded\":" + Num(b.founded) + ",\"attacks\":" + Num(b.attacks) + ",\"retaken\":" + Num(b.retaken) + ",\"first_reject\":\"" + Escape(b.firstReject) + "\"}" +
			",\"workers\":{\"max_automated\":" + Num(w.maxAutomated) + ",\"automated_turns\":" + Num(w.automatedTurns) + ",\"working_turns\":" + Num(w.workingTurns) + ",\"improvements_turn10\":" + Num(improvementsAt10[p]) +
			",\"improvements_end\":" + Num(improvementsEnd[p]) + "}" + ",\"diplo_messages\":" + Num(diploTo[p]) + ",\"diplo_requests\":" + Num(requestsTo[p]) + ",\"requests_pending_end\":" + Num(pending ? 1 : 0) + "}";
		printf("spikenet-human|player %d|alive %d|cities %d|units %d|bot turns %d commands %d refused %d founded %d attacks %d|automated workers max %d, %d/%d worker-turns working|improvements t10 %d end %d|diplo %d (requests %d)\n",
			p, pl.isAlive() ? 1 : 0, pl.getNumCities(), pl.getNumUnits(), b.turns, b.commands, b.rejected, b.founded, b.attacks, w.maxAutomated, w.workingTurns, w.automatedTurns,
			improvementsAt10[p], improvementsEnd[p], diploTo[p], requestsTo[p]);
	}
	std::string hybridOrder;
	if (hybrid)
		for (std::map<int, std::vector<int> >::const_iterator it = activeHumansByTurn.begin(); it != activeHumansByTurn.end(); ++it)
		{
			std::string list;
			for (size_t k = 0; k < it->second.size(); ++k) list += Format("%s%d", list.empty() ? "" : ">", it->second[k]);
			hybridOrder += Format("%s\"%d\":\"%s\"", hybridOrder.empty() ? "" : ",", it->first, list.c_str());
		}
	std::string both;
	for (size_t i = 0; i < bothTurns.size(); ++i) both += Format("%s%d", both.empty() ? "" : " ", bothTurns[i]);
	std::map<std::string, int> stubs;
	for (size_t i = 0; i < seen.stubs.size(); ++i) ++stubs[seen.stubs[i]];
	// The portable build defines FINAL_RELEASE, which compiles CvAssert / FAssert out; a build without it crashes in the gamecore's debug-only code
	// already in the B1 smoke run (N1.1 report), so asserts are not counted here.
	const std::string asserts = ",\"asserts\":\"not counted: FINAL_RELEASE compiles CvAssert out\"";
	std::string ais;
	for (int i = 0; i < MAX_MAJOR_CIVS; ++i)
	{
		const CvPlayer& p = GET_PLAYER((PlayerTypes)i);
		if (!p.isEverAlive() || p.isHuman() || p.isMinorCiv() || p.isBarbarian()) continue;
		ais += Format("%s{\"player\":%d,\"alive\":%d,\"cities\":%d,\"units\":%d,\"techs\":%d}", ais.empty() ? "" : ",", i, p.isAlive() ? 1 : 0, p.getNumCities(), p.getNumUnits(),
			GET_TEAM(p.getTeam()).GetTeamTechs()->GetNumTechsKnown());
		printf("spikenet-ai|player %d|alive %d|cities %d|units %d\n", i, p.isAlive() ? 1 : 0, p.getNumCities(), p.getNumUnits());
	}
	int dropped = 0;
	for (std::map<std::string, int>::const_iterator it = e.Dropped().begin(); it != e.Dropped().end(); ++it) dropped += it->second;
	for (std::map<std::string, int>::const_iterator it = e.ExeCalls().begin(); it != e.ExeCalls().end(); ++it) printf("spikenet-exe|%s|%d\n", it->first.c_str(), it->second);
	for (std::map<std::string, int>::const_iterator it = e.Dropped().begin(); it != e.Dropped().end(); ++it) printf("spikenet-dropped|%s|%d\n", it->first.c_str(), it->second);
	for (std::map<std::string, int>::const_iterator it = stubs.begin(); it != stubs.end(); ++it) printf("spikenet-stub|%s\n", it->first.c_str());

	c.add("turns", started && doneTurns == (int)turns && stopReason.empty(), "%d/%u turn(s) (turn %d -> %d), %d pump(s), %d WAITING step(s), %d bot round(s)%s%s", doneTurns, turns, startTurn,
		endTurn, pumps, waitingSteps, rounds, stopReason.empty() ? "" : ": ", stopReason.c_str());
	c.add("no_stuck", stuck == 0 && gameOver == 0, "%d STUCK, %d GAME_OVER", stuck, gameOver);
	c.add("humans_played", humanCount == (int)humans && botTurns >= doneTurns * humanCount && botCommands > 0, "%d human(s), bots ended %d turn(s) in all, %d command(s)", humanCount, botTurns, botCommands);
	c.add("active_rule", activeRuleBroken == 0, "%d time(s) the active player was not the server seat %d after a pump or a bot round", activeRuleBroken, seat);
	c.add("lua_errors", luaErrors == 0, "%d Lua error(s) during the turns", luaErrors);
	c.add("log_errors", seen.errors == 0, "%d error(s), %d warning(s) logged%s%s", seen.errors, seen.warnings, seen.firstErrors.empty() ? "" : "; first: ", seen.firstErrors.empty() ? "" : seen.firstErrors[0].c_str());
	if (hybrid)
	{
		c.add("war_declared", warTurn >= 0 && humanList.size() >= 2 && GET_TEAM(GET_PLAYER((PlayerTypes)humanList[0]).getTeam()).isAtWar(GET_PLAYER((PlayerTypes)humanList[1]).getTeam()),
			"%s", warHow.empty() ? "no war" : warHow.c_str());
		c.add("not_your_turn", notYourTurn > 0 && notYourTurnOk == notYourTurn, "%d command(s) of a human waiting for its turn refused: %d with GC_REASON_NOT_YOUR_TURN%s%s", notYourTurn, notYourTurnOk,
			notYourTurnFirst.empty() ? "" : "; first other: ", notYourTurnFirst.c_str());
		c.add("hybrid_order", warTurn >= 0 && bothWarringActive == 0, "from the turn after the war the two humans were turn-active together in %d of %d WAITING step(s)%s%s",
			bothWarringActive, warringSteps, both.empty() ? "" : ", turns ", both.c_str());
	}

	const std::string out = "{\"failed\":" + Num(c.failed) + ",\"mode\":\"" + turnMode + "\",\"seed\":" + Num(seed) + ",\"humans\":" + Num(humans) + ",\"ai\":" + Num(ai) +
		",\"turns_requested\":" + Num(turns) + ",\"turns_done\":" + Num(doneTurns) + ",\"start_turn\":" + Num(startTurn) + ",\"end_turn\":" + Num(endTurn) + ",\"stuck\":" + Num(stuck) +
		",\"game_over\":" + Num(gameOver) + ",\"stop_reason\":\"" + Escape(stopReason) + "\",\"setup\":\"" + Escape(setupFacts) + "\",\"log_errors\":" + Num(seen.errors) +
		",\"log_warnings\":" + Num(seen.warnings) + ",\"lua_errors\":" + Num(luaErrors) + ",\"active_rule_broken\":" + Num(activeRuleBroken) +
		asserts +
		",\"exe_calls\":" + CountMap(e.ExeCalls()) + ",\"exe_stubs_logged\":" + CountMap(stubs) + ",\"dropped_messages\":" + CountMap(e.Dropped()) + ",\"dropped_total\":" + Num(dropped) +
		",\"net_queue_end\":" + Num(e.NetQueueSize()) + ",\"net_delivered\":" + Num(e.NetMessagesDelivered()) +
		",\"diplo\":{\"total\":" + Num(dp.Total()) + ",\"pending\":" + Num(dp.Pending()) + ",\"humans_with_queued_requests_end\":" + Num(pendingRequests) + "}" +
		",\"workers\":{\"automated_worker_turns\":" + Num(automatedTurns) + ",\"working\":" + Num(workingTurns) + "}" +
		",\"war\":{\"turn\":" + Num(warTurn) + ",\"how\":\"" + Escape(warHow) + "\",\"both_active_steps\":" + Num(bothWarringActive) + ",\"steps\":" + Num(warringSteps) + "}" +
		",\"active_humans_by_turn\":{" + hybridOrder + "}" +
		",\"ms\":{\"ai_turn_p50\":" + Fix(Percentile(aiMs, 0.5)) + ",\"ai_turn_p95\":" + Fix(Percentile(aiMs, 0.95)) + ",\"ai_turn_max\":" + Fix(Percentile(aiMs, 1.0)) +
		",\"turn_p50\":" + Fix(Percentile(turnMs, 0.5)) + ",\"turn_max\":" + Fix(Percentile(turnMs, 1.0)) + "},\"turn_ms\":[" + turnsJson + "]" +
		",\"human_list\":[" + humansJson + "],\"ai_list\":[" + ais + "],\"checks\":[" + c.json + "]}";
	if (report && report_len)
	{
		strncpy(report, out.c_str(), report_len - 1);
		report[report_len - 1] = '\0';
	}
	printf("spikenet-summary|mode %s|turns %d/%u|stuck %d|log errors %d|lua errors %d|dropped %d|diplo %d|workers %d/%d working|ai turn p50 %.1f ms max %.1f ms|%s\n", turnMode.c_str(), doneTurns,
		turns, stuck, seen.errors, luaErrors, dropped, dp.Total(), workingTurns, automatedTurns, Percentile(aiMs, 0.5), Percentile(aiMs, 1.0), stopReason.c_str());
	SetLogSink(NULL, NULL);
	return c.failed == 0 ? 0 : 1;
}
