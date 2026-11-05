// Scenario `peace` (task N3.10; run.sh --scenario peace --humans N --ai A [--mode simultaneous|hybrid] [--turns T]): war and white peace between
// two humans through the C ABI only, with the humans played by this scenario (no test bots; END_TURN_TIMEOUT ends their turns): a network game, human 0 and human 1.
//  - Q_DIPLO of human 0: the record of human 1 (met, human, may declare war, may not make peace), of the AI (not human), bad arguments refused;
//  - MAKE_PEACE at peace: REJECTED / RULE; the queries and the refusals leave the simulation digest alone (AC-N1.4);
//  - DECLARE_WAR of human 0 on the team of human 1: delivered by the next gc_pump, one HUMAN_WAR_DECLARED event and a WAR_STATE event; Q_DIPLO of both sides then shows the war,
//    may make peace, may not declare war;
//  - refusals of MAKE_PEACE: no argument (INVALID_ARG), an AI as the player (NOT_HUMAN), an AI team or the own team as the target (BAD_TARGET);
//  - a lock of the war (the coop-war lock, set by this scenario): Q_DIPLO shows war_locked_turns and no CAN_MAKE_PEACE, MAKE_PEACE is REJECTED / RULE and the war goes on; the lock
//    runs down with the turns (CvTeam::doTurn), and the turn after it is gone MAKE_PEACE of human 1 is accepted: no war, WAR_STATE with at_war 0, Q_DIPLO shows peace, may not
//    declare war (the treaty's forced peace), a second MAKE_PEACE and a DECLARE_WAR are REJECTED / RULE;
//  - the treaty ends after its game length (PEACE_TREATY_LENGTH turns): CAN_DECLARE_WAR comes back.
// In --mode hybrid the same holds; the two humans at war then move one after the other, the scenario ends whoever's turn is active.
#include "CvGameCoreDLLPCH.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
		printf("peace: %-4s %s: %s\n", ok ? "ok" : "FAIL", name, detail);
		if (!ok) ++failed;
	}
};

struct Seen
{
	const gc_host* outer;
	int logErrors, logWarnings, luaErrors;
	int events[32];
	int warStateLast[3];     // event 10: team_a, team_b, at_war of the last one
	int warStateEvents;
	std::vector<std::string> lines;
	Seen() : outer(NULL), logErrors(0), logWarnings(0), luaErrors(0), warStateEvents(0)
	{
		memset(events, 0, sizeof(events));
		warStateLast[0] = warStateLast[1] = warStateLast[2] = -1;
	}
};

void OnLog(void* user, int32_t level, const char* msg)
{
	Seen* s = static_cast<Seen*>(user);
	if (level == GC_LOG_ERROR) { ++s->logErrors; if (s->lines.size() < 20) s->lines.push_back(std::string("log error: ") + msg); }
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
	if (ev->kind >= 0 && ev->kind < 32) ++s->events[ev->kind];
	if (ev->kind == GC_EVENT_WAR_STATE && ev->argc >= 3)
	{
		++s->warStateEvents;
		for (int i = 0; i < 3; ++i) s->warStateLast[i] = ev->args[i];
	}
	if (ev->kind == GC_EVENT_LUA_ERROR)
	{
		++s->luaErrors;
		if (s->lines.size() < 20) s->lines.push_back(std::string("lua error: ") + (ev->text ? ev->text : ""));
	}
}

gc_command Cmd(int kind, int player, const int32_t* args = NULL, unsigned argc = 0)
{
	gc_command c;
	memset(&c, 0, sizeof(c));
	c.kind = (uint16_t)kind;
	c.player = (uint8_t)player;
	c.argc = argc;
	c.args = args;
	return c;
}

// The Q_DIPLO record of `other` seen by `player`; false when there is none. *rc gets the query's return code.
bool GetDiplo(gc_instance* inst, int player, int other, gcq_diplo* out, int32_t* rc = NULL)
{
	gc_buf b = {0, 0, 0, 0};
	gc_error err;
	memset(&err, 0, sizeof(err));
	const int32_t arg[1] = {(int32_t)player};
	const int32_t r = gc_query(inst, GC_Q_DIPLO, arg, 1, &b, &err);
	if (rc) *rc = r;
	bool found = false;
	if (r == GC_OK && b.len >= sizeof(gcq_header))
	{
		gcq_header h;
		memcpy(&h, b.data, sizeof(h));
		for (uint32_t k = 0; k < h.count && h.record_size >= sizeof(gcq_diplo); ++k)
		{
			gcq_diplo d;
			memcpy(&d, b.data + sizeof(h) + (size_t)k * h.record_size, sizeof(d));
			if (d.other == other) { *out = d; found = true; }
		}
	}
	gc_buf_release(&b);
	return found;
}

int DiploCount(gc_instance* inst, int player)
{
	gc_buf b = {0, 0, 0, 0};
	gc_error err;
	memset(&err, 0, sizeof(err));
	const int32_t arg[1] = {(int32_t)player};
	int n = -1;
	if (gc_query(inst, GC_Q_DIPLO, arg, 1, &b, &err) == GC_OK && b.len >= sizeof(gcq_header))
	{
		gcq_header h;
		memcpy(&h, b.data, sizeof(h));
		n = (int)h.count;
	}
	gc_buf_release(&b);
	return n;
}

// gc_pump until the humans are up (WAITING_HUMAN); false when it did not come to that.
bool PumpToHumans(gc_instance* inst, gc_pump_result* pr, std::string* why)
{
	gc_error err;
	memset(&err, 0, sizeof(err));
	for (int calls = 0; calls < 20000; ++calls)
	{
		const int32_t rc = gc_pump(inst, 64, pr, &err);
		if (rc != GC_OK) { *why = Format("gc_pump: %d %s", rc, err.message); return false; }
		if (pr->status == GC_PUMP_WAITING_HUMAN) return true;
		if (pr->status == GC_PUMP_GAME_OVER || pr->status == GC_PUMP_STUCK) { *why = Format("gc_pump status %d at turn %d", pr->status, pr->turn); return false; }
	}
	*why = "no WAITING_HUMAN after 20000 gc_pump calls";
	return false;
}

// Ends the turn of every human whose turn is active (END_TURN_TIMEOUT, no blocker checks) and pumps until the game turn changes; then on to the next human phase.
bool EndTurnAndAdvance(gc_instance* inst, RtSession* session, const std::vector<int>& humans, gc_pump_result* pr, std::string* why)
{
	gc_error err;
	memset(&err, 0, sizeof(err));
	const int turn0 = GC.getGame().getGameTurn();
	for (int calls = 0; calls < 20000; ++calls)
	{
		if (session->HumanPhase())
			for (size_t h = 0; h < humans.size(); ++h)
			{
				const PlayerTypes who = (PlayerTypes)humans[h];
				if (IsSeatedHuman(humans[h]) && GET_PLAYER(who).isTurnActive() && !session->Engine().TurnCompleteSent(who))
				{
					gc_command c = Cmd(GC_CMD_END_TURN_TIMEOUT, humans[h]);
					gc_apply_command(inst, &c, &err);
				}
			}
		const int32_t rc = gc_pump(inst, 64, pr, &err);
		if (rc != GC_OK) { *why = Format("gc_pump: %d %s", rc, err.message); return false; }
		if (pr->status == GC_PUMP_GAME_OVER || pr->status == GC_PUMP_STUCK) { *why = Format("gc_pump status %d at turn %d", pr->status, pr->turn); return false; }
		if (pr->turn != turn0) return PumpToHumans(inst, pr, why);
	}
	*why = Format("turn %d did not end", turn0);
	return false;
}

// The simulation's digests (RtSession::StateDigest and the whole-truth gc_state_digest): equal before and after something that must not change the game.
std::string Digests(gc_instance* inst)
{
	gc_error err;
	uint64_t d = 0;
	if (gc_state_digest(inst, -1, &d, &err) != GC_OK) d = 0;
	return Format("%016llx/%016llx", (unsigned long long)RtSession::StateDigest(), (unsigned long long)d);
}

std::string Num(long long v) { return Format("%lld", v); }

}  // namespace

extern "C" int rt_selftest_peace(const gc_host* host, unsigned seed, unsigned turns, unsigned humans, unsigned ai, const char* mode, const char* world_size, char* report, size_t report_len)
{
	Check c;
	Seen seen;
	seen.outer = host;
	gc_host mine = {&seen, OnLog, OnReadFile, OnEvent};
	gc_error err;
	memset(&err, 0, sizeof(err));
	if (humans < 2) humans = 2;
	if (humans > 4) humans = 4;
	if (ai < 1) ai = 1;
	const std::string turnMode = mode && *mode ? mode : "simultaneous";
	const bool hybrid = turnMode == "hybrid";

	gc_instance* inst = gc_create(&mine, &err);
	c.add("create", inst != NULL, "gc_create: %d %s", err.code, err.message);

	std::string stopReason;
	int startTurn = -1, endTurn = -1, warTurn = -1, peaceTurn = -1, treatyEndTurn = -1;
	std::vector<int> humanList;
	if (inst)
	{
		std::string players;
		for (unsigned i = 0; i < humans; ++i)
			players += Format("%s{\"kind\":\"human\",\"civ\":\"%s\",\"nick\":\"human%u\"}", i ? "," : "", i ? "random" : "CIVILIZATION_ROME", i);
		for (unsigned i = 0; i < ai; ++i) players += ",{\"kind\":\"ai\",\"civ\":\"random\"}";
		const std::string setup = Format("{\"seed\":%u,\"game_type\":\"network\",\"players\":[%s],\"turn_mode\":\"%s\",\"handicap\":\"HANDICAP_CHIEFTAIN\"%s%s%s}", seed, players.c_str(), turnMode.c_str(),
			world_size && *world_size ? ",\"world_size\":\"" : "", world_size && *world_size ? world_size : "", world_size && *world_size ? "\"" : "");
		const int32_t rc = gc_new_game(inst, setup.c_str(), &err);
		c.add("new_game", rc == GC_OK, "%s: %d %s", setup.c_str(), rc, err.message);
		RtSession* session = InstanceSession(inst);
		if (rc == GC_OK && session)
		{
			for (int i = 0; i < MAX_PLAYERS; ++i)
				if (IsSeatedHuman(i)) humanList.push_back(i);
			int aiPlayer = -1;
			for (int i = 0; i < MAX_MAJOR_CIVS; ++i)
				if (GET_PLAYER((PlayerTypes)i).isAlive() && !IsSeatedHuman(i) && (int)ServerSeat() != i) { aiPlayer = i; break; }
			const int A = humanList.size() >= 1 ? humanList[0] : -1;
			const int B = humanList.size() >= 2 ? humanList[1] : -1;
			gc_pump_result pr;
			memset(&pr, 0, sizeof(pr));
			startTurn = GC.getGame().getGameTurn();
			if (A < 0 || B < 0 || aiPlayer < 0 || !PumpToHumans(inst, &pr, &stopReason))
			{
				if (stopReason.empty()) stopReason = "the game has no two humans and an AI";
			}
			else
			{
				const TeamTypes teamA = GET_PLAYER((PlayerTypes)A).getTeam(), teamB = GET_PLAYER((PlayerTypes)B).getTeam(), teamAI = GET_PLAYER((PlayerTypes)aiPlayer).getTeam();
				// On a small map they may not have met yet: scenario-only, the commands under test need a meeting.
				if (!GET_TEAM(teamA).isHasMet(teamB)) GET_TEAM(teamA).meet(teamB, true);
				if (!GET_TEAM(teamA).isHasMet(teamAI)) GET_TEAM(teamA).meet(teamAI, true);

				// ---- Q_DIPLO at peace ----
				gcq_diplo d;
				int32_t qrc = 0;
				const bool gotB = GetDiplo(inst, A, B, &d, &qrc);
				c.add("diplo_peace", gotB && (d.flags & GC_DIPLO_F_MET) && (d.flags & GC_DIPLO_F_HUMAN) && !(d.flags & GC_DIPLO_F_AT_WAR) && (d.flags & GC_DIPLO_F_CAN_DECLARE_WAR) &&
					!(d.flags & GC_DIPLO_F_CAN_MAKE_PEACE) && d.war_locked_turns == 0 && d.team == (int)teamB,
					"player %d sees %d: found %d flags 0x%04x team %d locked %d (query rc %d)", A, B, gotB ? 1 : 0, gotB ? (unsigned)d.flags : 0u, gotB ? (int)d.team : -1, gotB ? (int)d.war_locked_turns : -1, qrc);
				gcq_diplo dAi;
				const bool gotAi = GetDiplo(inst, A, aiPlayer, &dAi);
				c.add("diplo_ai", gotAi && (dAi.flags & GC_DIPLO_F_MET) && !(dAi.flags & GC_DIPLO_F_HUMAN) && !(dAi.flags & GC_DIPLO_F_CAN_MAKE_PEACE),
					"player %d sees the AI %d: found %d flags 0x%04x", A, aiPlayer, gotAi ? 1 : 0, gotAi ? (unsigned)dAi.flags : 0u);
				{
					// One record per other living major civ, not the server seat; bad arguments refused.
					int expect = 0;
					for (int i = 0; i < MAX_MAJOR_CIVS; ++i)
						if (i != A && GET_PLAYER((PlayerTypes)i).isAlive() && (int)ServerSeat() != i) ++expect;
					const int n = DiploCount(inst, A);
					gc_buf b = {0, 0, 0, 0};
					const int32_t none = gc_query(inst, GC_Q_DIPLO, NULL, 0, &b, &err);
					const int32_t seatArg[1] = {(int32_t)ServerSeat()};
					const int32_t seat = gc_query(inst, GC_Q_DIPLO, seatArg, 1, &b, &err);
					const int32_t bigArg[1] = {63};
					const int32_t big = gc_query(inst, GC_Q_DIPLO, bigArg, 1, &b, &err);
					c.add("diplo_args", n == expect && none == GC_ERR_INVALID_ARG && seat == GC_ERR_INVALID_ARG && big == GC_ERR_INVALID_ARG,
						"%d record(s), want %d; no argument rc %d, the server seat rc %d, player 63 rc %d (want %d)", n, expect, none, seat, big, GC_ERR_INVALID_ARG);
				}

				// ---- MAKE_PEACE at peace is refused; the queries and the refusal change nothing ----
				{
					const std::string before = Digests(inst);
					const int32_t targ[1] = {(int32_t)teamB};
					gc_command mp = Cmd(GC_CMD_MAKE_PEACE, A, targ, 1);
					const int32_t mrc = gc_apply_command(inst, &mp, &err);
					GetDiplo(inst, A, B, &d);
					const std::string after = Digests(inst);
					c.add("peace_without_war", mrc == GC_ERR_REJECTED && err.reason == GC_REASON_RULE && before == after, "MAKE_PEACE at peace: rc %d reason %d \"%s\"; digests %s -> %s", mrc, (int)err.reason, err.message,
						before.c_str(), after.c_str());
				}

				// ---- the argument checks of MAKE_PEACE (still at peace: they fail before the war test) ----
				{
					const int32_t aiTeam[1] = {(int32_t)teamAI}, ownTeam[1] = {(int32_t)teamA}, badTeam[1] = {(int32_t)MAX_TEAMS};
					gc_command noArg = Cmd(GC_CMD_MAKE_PEACE, A);
					gc_command forAi = Cmd(GC_CMD_MAKE_PEACE, aiPlayer, aiTeam, 1);
					gc_command onAi = Cmd(GC_CMD_MAKE_PEACE, A, aiTeam, 1);
					gc_command onSelf = Cmd(GC_CMD_MAKE_PEACE, A, ownTeam, 1);
					gc_command onBad = Cmd(GC_CMD_MAKE_PEACE, A, badTeam, 1);
					const int32_t r1 = gc_apply_command(inst, &noArg, &err);
					const int32_t r2 = gc_apply_command(inst, &forAi, &err);
					const int r2reason = (int)err.reason;
					const int32_t r3 = gc_apply_command(inst, &onAi, &err);
					const int r3reason = (int)err.reason;
					const int32_t r4 = gc_apply_command(inst, &onSelf, &err);
					const int r4reason = (int)err.reason;
					const int32_t r5 = gc_apply_command(inst, &onBad, &err);
					const int r5reason = (int)err.reason;
					c.add("peace_args", r1 == GC_ERR_INVALID_ARG && r2 == GC_ERR_REJECTED && r2reason == GC_REASON_NOT_HUMAN && r3 == GC_ERR_REJECTED && r3reason == GC_REASON_BAD_TARGET &&
						r4 == GC_ERR_REJECTED && r4reason == GC_REASON_BAD_TARGET && r5 == GC_ERR_REJECTED && r5reason == GC_REASON_BAD_TARGET,
						"no argument %d; an AI as the player %d/%d; an AI team %d/%d; the own team %d/%d; team %d %d/%d", r1, r2, r2reason, r3, r3reason, r4, r4reason, (int)MAX_TEAMS, r5, r5reason);
				}

				// ---- DECLARE_WAR of A on the team of B: delivered by the next gc_pump ----
				{
					const int32_t targ[1] = {(int32_t)teamB};
					gc_command dw = Cmd(GC_CMD_DECLARE_WAR, A, targ, 1);
					const int32_t wrc = gc_apply_command(inst, &dw, &err);
					const std::string wmsg = err.message;
					int pumps = 0;
					while (wrc == GC_OK && !GET_TEAM(teamA).isAtWar(teamB) && pumps < 20)
					{
						gc_pump(inst, 1, &pr, &err);
						++pumps;
					}
					warTurn = GC.getGame().getGameTurn();
					c.add("declare_war", wrc == GC_OK && GET_TEAM(teamA).isAtWar(teamB) && GET_TEAM(teamB).isAtWar(teamA) && seen.events[GC_EVENT_HUMAN_WAR_DECLARED] == 1 &&
						seen.warStateEvents >= 1 && seen.warStateLast[2] == 1,
						"DECLARE_WAR rc %d %s; at war %d/%d after %d single-slice gc_pump(s); HUMAN_WAR_DECLARED %d, WAR_STATE %d (last at_war %d)", wrc, wmsg.c_str(), GET_TEAM(teamA).isAtWar(teamB) ? 1 : 0,
						GET_TEAM(teamB).isAtWar(teamA) ? 1 : 0, pumps, seen.events[GC_EVENT_HUMAN_WAR_DECLARED], seen.warStateEvents, seen.warStateLast[2]);
				}
				{
					gcq_diplo dA, dB;
					const bool ga = GetDiplo(inst, A, B, &dA), gb = GetDiplo(inst, B, A, &dB);
					const unsigned wantWar = GC_DIPLO_F_MET | GC_DIPLO_F_HUMAN | GC_DIPLO_F_AT_WAR | GC_DIPLO_F_CAN_MAKE_PEACE;
					c.add("diplo_war", ga && gb && (dA.flags & wantWar) == wantWar && (dB.flags & wantWar) == wantWar && !(dA.flags & GC_DIPLO_F_CAN_DECLARE_WAR) && !(dB.flags & GC_DIPLO_F_CAN_DECLARE_WAR) &&
						dA.war_locked_turns == 0 && dB.war_locked_turns == 0,
						"A sees flags 0x%04x locked %d; B sees flags 0x%04x locked %d (want at war, may make peace, may not declare war)", ga ? (unsigned)dA.flags : 0u, ga ? (int)dA.war_locked_turns : -1,
						gb ? (unsigned)dB.flags : 0u, gb ? (int)dB.war_locked_turns : -1);
				}

				// ---- a locked war: no peace yet ----
				const int kLock = 2;
				GET_TEAM(teamA).SetNumTurnsLockedIntoWar(teamB, kLock);  // scenario-only: the coop-war lock (CvDealClasses.cpp:2729), the one rule that holds a war
				bool lockedOk = true;
				std::string lockNote;
				for (int left = kLock; left >= 1 && stopReason.empty(); --left)
				{
					const std::string before = Digests(inst);
					gcq_diplo dl;
					const bool g = GetDiplo(inst, B, A, &dl);
					const int32_t targ[1] = {(int32_t)teamA};
					gc_command mp = Cmd(GC_CMD_MAKE_PEACE, B, targ, 1);
					const int32_t mrc = gc_apply_command(inst, &mp, &err);
					const std::string after = Digests(inst);
					const bool ok = g && dl.war_locked_turns == left && !(dl.flags & GC_DIPLO_F_CAN_MAKE_PEACE) && (dl.flags & GC_DIPLO_F_AT_WAR) && mrc == GC_ERR_REJECTED && err.reason == GC_REASON_RULE &&
						GET_TEAM(teamA).isAtWar(teamB) && before == after;
					if (!ok) lockedOk = false;
					lockNote += Format("%sturn %d: locked %d, flags 0x%04x, MAKE_PEACE rc %d reason %d, at war %d", lockNote.empty() ? "" : "; ", GC.getGame().getGameTurn(), g ? (int)dl.war_locked_turns : -1,
						g ? (unsigned)dl.flags : 0u, mrc, (int)err.reason, GET_TEAM(teamA).isAtWar(teamB) ? 1 : 0);
					if (!EndTurnAndAdvance(inst, session, humanList, &pr, &stopReason)) break;
				}
				c.add("peace_locked", lockedOk && stopReason.empty(), "%s", lockNote.c_str());

				// ---- the lock is gone: B's MAKE_PEACE is accepted ----
				if (stopReason.empty())
				{
					gcq_diplo dl;
					const bool g = GetDiplo(inst, B, A, &dl);
					c.add("lock_gone", g && dl.war_locked_turns == 0 && (dl.flags & GC_DIPLO_F_CAN_MAKE_PEACE), "turn %d: locked %d flags 0x%04x", GC.getGame().getGameTurn(), g ? (int)dl.war_locked_turns : -1,
						g ? (unsigned)dl.flags : 0u);
					const int warEventsBefore = seen.warStateEvents;
					const int32_t targ[1] = {(int32_t)teamA};
					gc_command mp = Cmd(GC_CMD_MAKE_PEACE, B, targ, 1);
					const int32_t mrc = gc_apply_command(inst, &mp, &err);
					peaceTurn = GC.getGame().getGameTurn();
					gc_pump(inst, 1, &pr, &err);  // the events (WAR_STATE of the peace) reach the host with the next gc_pump
					c.add("make_peace", mrc == GC_OK && !GET_TEAM(teamA).isAtWar(teamB) && !GET_TEAM(teamB).isAtWar(teamA) && seen.warStateEvents > warEventsBefore && seen.warStateLast[2] == 0 &&
						GET_TEAM(teamA).isForcePeace(teamB),
						"MAKE_PEACE rc %d reason %d %s; at war %d/%d, forced peace %d, WAR_STATE events %d -> %d (last at_war %d)", mrc, (int)err.reason, err.message, GET_TEAM(teamA).isAtWar(teamB) ? 1 : 0,
						GET_TEAM(teamB).isAtWar(teamA) ? 1 : 0, GET_TEAM(teamA).isForcePeace(teamB) ? 1 : 0, warEventsBefore, seen.warStateEvents, seen.warStateLast[2]);
					gcq_diplo dA, dB;
					const bool ga = GetDiplo(inst, A, B, &dA), gb = GetDiplo(inst, B, A, &dB);
					c.add("diplo_after_peace", ga && gb && !(dA.flags & (GC_DIPLO_F_AT_WAR | GC_DIPLO_F_CAN_MAKE_PEACE | GC_DIPLO_F_CAN_DECLARE_WAR)) && !(dB.flags & (GC_DIPLO_F_AT_WAR | GC_DIPLO_F_CAN_MAKE_PEACE | GC_DIPLO_F_CAN_DECLARE_WAR)),
						"A sees flags 0x%04x, B sees flags 0x%04x (want no war, no peace button, no war button while the treaty runs)", ga ? (unsigned)dA.flags : 0u, gb ? (unsigned)dB.flags : 0u);
					// The peace is real in the commands: a second MAKE_PEACE and a DECLARE_WAR are refused for the rule.
					const int32_t targB[1] = {(int32_t)teamB};
					gc_command mp2 = Cmd(GC_CMD_MAKE_PEACE, B, targ, 1);
					const int32_t r1 = gc_apply_command(inst, &mp2, &err);
					const int r1reason = (int)err.reason;
					gc_command dw = Cmd(GC_CMD_DECLARE_WAR, A, targB, 1);
					const int32_t r2 = gc_apply_command(inst, &dw, &err);
					const int r2reason = (int)err.reason;
					c.add("peace_holds", r1 == GC_ERR_REJECTED && r1reason == GC_REASON_RULE && r2 == GC_ERR_REJECTED && r2reason == GC_REASON_RULE,
						"second MAKE_PEACE rc %d reason %d; DECLARE_WAR rc %d reason %d (want REJECTED / RULE twice)", r1, r1reason, r2, r2reason);
				}

				// ---- the treaty ends after its game length: war may be declared again ----
				if (stopReason.empty())
				{
					const int limit = GC.getPEACE_TREATY_LENGTH() + 4;
					int waited = 0;
					bool back = false;
					for (; waited <= limit && stopReason.empty(); ++waited)
					{
						gcq_diplo dA;
						if (GetDiplo(inst, A, B, &dA) && (dA.flags & GC_DIPLO_F_CAN_DECLARE_WAR)) { back = true; treatyEndTurn = GC.getGame().getGameTurn(); break; }
						if (!EndTurnAndAdvance(inst, session, humanList, &pr, &stopReason)) break;
					}
					c.add("treaty_ends", back && waited >= 1 && waited <= GC.getPEACE_TREATY_LENGTH() + 2, "CAN_DECLARE_WAR came back after %d turn(s) of peace (PEACE_TREATY_LENGTH %d; peace made at turn %d, back at turn %d)%s%s",
						waited, GC.getPEACE_TREATY_LENGTH(), peaceTurn, treatyEndTurn, stopReason.empty() ? "" : "; ", stopReason.c_str());
				}
				if (turns > 0 && stopReason.empty() && (int)(GC.getGame().getGameTurn() - startTurn) < (int)turns)
				{
					// The rest of the turn budget: plain turns, the war and peace commands must leave nothing behind that stops the game.
					while ((int)(GC.getGame().getGameTurn() - startTurn) < (int)turns && stopReason.empty())
						if (!EndTurnAndAdvance(inst, session, humanList, &pr, &stopReason)) break;
				}
			}
			endTurn = GC.getGame().getGameTurn();
			c.add("active_rule", CvPreGame::activePlayer() == ServerSeat(), "the active player is %d, the server seat is %d", (int)CvPreGame::activePlayer(), (int)ServerSeat());
		}
	}
	for (size_t i = 0; i < seen.lines.size(); ++i) printf("peace-event|%s\n", seen.lines[i].c_str());
	c.add("no_stop", stopReason.empty(), "%s", stopReason.empty() ? "no STUCK, no GAME_OVER, no gc_pump error" : stopReason.c_str());
	c.add("lua_errors", seen.luaErrors == 0, "%d LUA_ERROR event(s)", seen.luaErrors);
	c.add("log_errors", seen.logErrors == 0, "%d error(s), %d warning(s) logged", seen.logErrors, seen.logWarnings);

	const std::string out = "{\"failed\":" + Num(c.failed) + ",\"mode\":\"" + turnMode + "\",\"seed\":" + Num(seed) + ",\"humans\":" + Num(humans) + ",\"ai\":" + Num(ai) + ",\"start_turn\":" + Num(startTurn) +
		",\"end_turn\":" + Num(endTurn) + ",\"war_turn\":" + Num(warTurn) + ",\"peace_turn\":" + Num(peaceTurn) + ",\"treaty_end_turn\":" + Num(treatyEndTurn) + ",\"log_errors\":" + Num(seen.logErrors) +
		",\"lua_errors\":" + Num(seen.luaErrors) + ",\"checks\":[" + c.json + "]}";
	if (report && report_len)
	{
		strncpy(report, out.c_str(), report_len - 1);
		report[report_len - 1] = '\0';
	}
	printf("peace-summary|%s|seed %u|humans %u|ai %u|turns %d -> %d|war at %d|peace at %d|war allowed again at %d|failed %d\n", turnMode.c_str(), seed, humans, ai, startTurn, endTurn, warTurn, peaceTurn, treatyEndTurn, c.failed);
	gc_destroy(inst);
	return c.failed == 0 ? 0 : 1;
}
