// Scenario `mpmsg` (task N3.21, HYG-0013; run.sh --scenario mpmsg --humans 2 --ai 1 [--lib-dir-off DIR]): the lines the core shows to its owner
// ("owner is the active player" guards) reach every human of a network game. Two humans (A = slot 0, B = slot 1, played by the scenario, no bots) and an AI (C):
//  1. A human fight: B's archer shoots A's warrior through the command path (B is the active player while the command runs; A is not).
//  2. An AI fight: C's archer shoots A's warrior (scenario-only call, the server seat is the active player).
//  3. Another branch: A's team learns a technology (CvTeam::announceTechToPlayers, "you discovered ..."), called with the server seat as the active player.
// With the flag (LEKMOD_HEADLESS_HYG_MESSAGES_TO_HUMANS, compiled in or not: the scenario knows from the macro) A gets a MESSAGE event in each case, with the text of the
// line and the plot of the fight; without the flag A gets none (the golden rule). The simulation digest after each step is printed ("mpmsg-digest|label|hex"): run.sh runs the
// scenario on a build with and without the flag (--lib-dir-off) and compares them: the flag is UI-only, the digests must be equal.
#include "CvGameCoreDLLPCH.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "../abi/gc_internal.h"
#include "CvGameCoreUtils.h"
#include "CvUnitCombat.h"
#include "rt_active.h"
#include "rt_autoplay.h"
#include "rt_selftest.h"
#include "rt_session.h"

using namespace lekmod_portable;

namespace {

#if defined(LEKMOD_HEADLESS_HYG_MESSAGES_TO_HUMANS)
const bool kFlag = true;
#else
const bool kFlag = false;
#endif

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
		printf("mpmsg: %-4s %s: %s\n", ok ? "ok" : "FAIL", name, detail);
		if (!ok) ++failed;
	}
};

struct Msg
{
	int player, x, y, type;
	std::string text;
};

struct Seen
{
	const gc_host* outer;
	int logErrors, luaErrors;
	std::vector<Msg> messages;
	std::vector<std::string> lines;
	Seen() : outer(NULL), logErrors(0), luaErrors(0) {}
	int CountFor(int player, size_t from = 0) const
	{
		int n = 0;
		for (size_t i = from; i < messages.size(); ++i) if (messages[i].player == player) ++n;
		return n;
	}
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
	if (ev->kind == GC_EVENT_MESSAGE && ev->argc == 6)
	{
		Msg m;
		m.player = ev->args[0];
		m.x = ev->args[1];
		m.y = ev->args[2];
		m.type = ev->args[3];
		m.text = ev->text ? ev->text : "";
		s->messages.push_back(m);
	}
}

gc_command Cmd(int kind, int player, const int32_t* args, unsigned argc)
{
	gc_command c;
	memset(&c, 0, sizeof(c));
	c.kind = (uint16_t)kind;
	c.player = (uint8_t)player;
	c.argc = argc;
	c.args = args;
	return c;
}

CvPlot* FreeLandNextTo(int x, int y, int avoidX, int avoidY)
{
	for (int d = 0; d < NUM_DIRECTION_TYPES; ++d)
	{
		CvPlot* p = plotDirection(x, y, (DirectionTypes)d);
		if (!p || p->isWater() || p->isImpassable() || p->isMountain() || p->isCity() || p->isGoody() || p->getNumUnits() != 0) continue;
		if (p->getX() == avoidX && p->getY() == avoidY) continue;
		return p;
	}
	return NULL;
}

// Pumps until `player` has a MESSAGE event from index `from` on, or 30 calls; the pump runs the queued combat.
void PumpUntilMessage(gc_instance* inst, const Seen& seen, int player, size_t from)
{
	gc_pump_result pr;
	gc_error err;
	memset(&pr, 0, sizeof(pr));
	memset(&err, 0, sizeof(err));
	for (int n = 0; n < 30 && seen.CountFor(player, from) == 0; ++n)
		if (gc_pump(inst, 64, &pr, &err) != GC_OK) break;
}

// Prints the MESSAGE events from index `from` on.
void DumpMessages(const Seen& seen, const char* label, size_t from)
{
	for (size_t i = from; i < seen.messages.size(); ++i)
		printf("mpmsg-msg|%s|player %d|plot %d,%d|type %d|%s\n", label, seen.messages[i].player, seen.messages[i].x, seen.messages[i].y, seen.messages[i].type, seen.messages[i].text.c_str());
}

std::string Hex(uint64_t v) { return Format("%016llx", (unsigned long long)v); }

}  // namespace

extern "C" int rt_selftest_mpmsg(const gc_host* host, unsigned seed, unsigned turns, unsigned humans, unsigned ai, char* report, size_t report_len)
{
	Check c;
	Seen seen;
	seen.outer = host;
	gc_host mine = {&seen, OnLog, OnReadFile, OnEvent};
	gc_error err;
	memset(&err, 0, sizeof(err));
	if (humans < 2) humans = 2;
	if (ai < 1) ai = 1;

	gc_instance* inst = gc_create(&mine, &err);
	c.add("create", inst != NULL, "gc_create: %d %s; HYG-0013 flag %s", err.code, err.message, kFlag ? "compiled in" : "not compiled in");

	std::string digests;
	int doneTurns = 0;
	std::string stopReason;
	if (inst)
	{
		std::string players;
		for (unsigned i = 0; i < humans; ++i)
			players += Format("%s{\"kind\":\"human\",\"civ\":\"%s\",\"nick\":\"human%u\"}", i ? "," : "", i ? "random" : "CIVILIZATION_ROME", i);
		for (unsigned i = 0; i < ai; ++i) players += ",{\"kind\":\"ai\",\"civ\":\"random\"}";
		const std::string setup = Format("{\"seed\":%u,\"game_type\":\"network\",\"players\":[%s],\"turn_mode\":\"simultaneous\",\"handicap\":\"HANDICAP_CHIEFTAIN\"}", seed, players.c_str());
		const int32_t rc = gc_new_game(inst, setup.c_str(), &err);
		c.add("new_game", rc == GC_OK, "%s: %d %s", setup.c_str(), rc, err.message);
		RtSession* session = InstanceSession(inst);
		if (rc == GC_OK && session)
		{
			CvGame& game = GC.getGame();
			gc_pump_result pr;
			memset(&pr, 0, sizeof(pr));
			for (int n = 0; n < 400 && !(pr.status == GC_PUMP_WAITING_HUMAN && session->HumanPhase() && session->WaitingHumans().size() == humans); ++n)
				if (gc_pump(inst, 64, &pr, &err) != GC_OK) break;
			const int A = 0, B = 1, C = (int)humans;  // the first AI slot follows the humans
			const bool ready = pr.status == GC_PUMP_WAITING_HUMAN && IsSeatedHuman(A) && IsSeatedHuman(B) && !IsSeatedHuman(C) && GET_PLAYER((PlayerTypes)C).isAlive();
			c.add("humans_waiting", ready, "status %d, turn %d; humans %d and %d seated, player %d is the AI", (int)pr.status, (int)pr.turn, A, B, C);
			const UnitTypes warriorType = (UnitTypes)GC.getInfoTypeForString("UNIT_WARRIOR", true);
			const UnitTypes archerType = (UnitTypes)GC.getInfoTypeForString("UNIT_ARCHER", true);
			const TeamTypes tA = GET_PLAYER((PlayerTypes)A).getTeam(), tB = GET_PLAYER((PlayerTypes)B).getTeam(), tC = GET_PLAYER((PlayerTypes)C).getTeam();
			const int startTurn = game.getGameTurn();
			printf("mpmsg-note|game final-initialized: %d (the technology line is announced only then)\n", game.isFinalInitialized() ? 1 : 0);
			if (ready && warriorType != NO_UNIT && archerType != NO_UNIT)
			{
				// Plots: A's warrior (the defender) next to one of A's units, the shooter's plot next to it.
				CvPlot* defAt = NULL;
				CvPlot* atkAt = NULL;
				int loop;
				CvPlayer& pA = GET_PLAYER((PlayerTypes)A);
				for (CvUnit* u = pA.firstUnit(&loop); u && !atkAt; u = pA.nextUnit(&loop))
				{
					if (u->isDelayedDeath() || u->getDomainType() != DOMAIN_LAND) continue;
					defAt = FreeLandNextTo(u->getX(), u->getY(), -1, -1);
					if (defAt) atkAt = FreeLandNextTo(defAt->getX(), defAt->getY(), u->getX(), u->getY());
				}
				c.add("plots", defAt && atkAt, "defender plot %d,%d, shooter plot %d,%d", defAt ? defAt->getX() : -1, defAt ? defAt->getY() : -1, atkAt ? atkAt->getX() : -1, atkAt ? atkAt->getY() : -1);
				if (defAt && atkAt)
				{
					// Everybody has met everybody and A is at war with B and with the AI (scenario-only meetings; the war with B through the command path).
					if (!GET_TEAM(tA).isHasMet(tB)) GET_TEAM(tA).meet(tB, true);
					if (!GET_TEAM(tA).isHasMet(tC)) GET_TEAM(tA).meet(tC, true);
					if (!GET_TEAM(tB).isHasMet(tC)) GET_TEAM(tB).meet(tC, true);
					const int32_t warArgs[1] = {(int32_t)tA};
					gc_command war = Cmd(GC_CMD_DECLARE_WAR, B, warArgs, 1);
					const int32_t wrc = gc_apply_command(inst, &war, &err);
					// The command is queued: the pump carries it out (the war of the second pair is scenario-only).
					for (int n = 0; wrc == GC_OK && n < 20 && !GET_TEAM(tB).isAtWar(tA); ++n) gc_pump(inst, 64, &pr, &err);
					if (!GET_TEAM(tB).isAtWar(tA)) GET_TEAM(tB).declareWar(tA);
					if (!GET_TEAM(tC).isAtWar(tA)) GET_TEAM(tC).declareWar(tA);
					c.add("war", GET_TEAM(tB).isAtWar(tA) && GET_TEAM(tC).isAtWar(tA), "B -> A DECLARE_WAR rc %d %s; B at war with A: %d, AI at war with A: %d", wrc, err.message, GET_TEAM(tB).isAtWar(tA) ? 1 : 0, GET_TEAM(tC).isAtWar(tA) ? 1 : 0);
					digests += Format("mpmsg-digest|start|%s\n", Hex(RtSession::StateDigest()).c_str());

					struct Fight { const char* label; int shooter; };
					const Fight fights[2] = {{"human", B}, {"ai", C}};
					for (int f = 0; f < 2; ++f)
					{
						CvPlayer& shooterOwner = GET_PLAYER((PlayerTypes)fights[f].shooter);
						CvUnit* defender = pA.initUnit(warriorType, defAt->getX(), defAt->getY(), NO_UNITAI, NO_DIRECTION, false, false);
						CvUnit* archer = shooterOwner.initUnit(archerType, atkAt->getX(), atkAt->getY(), NO_UNITAI, NO_DIRECTION, false, false);
						if (!defender || !archer) { c.add("fight", false, "initUnit failed (%s)", fights[f].label); break; }
						const int defId = defender->GetID(), atkId = archer->GetID();
						const size_t from = seen.messages.size();
						int rcAttack = 0;
						if (f == 0)
						{
							const int32_t args[3] = {(int32_t)atkId, (int32_t)defAt->getX(), (int32_t)defAt->getY()};
							gc_command cmd = Cmd(GC_CMD_UNIT_RANGE_ATTACK, B, args, 3);
							rcAttack = gc_apply_command(inst, &cmd, &err);
						}
						else
						{
							rcAttack = (int)CvUnitCombat::AttackRanged(*archer, defAt->getX(), defAt->getY(), CvUnitCombat::ATTACK_OPTION_NONE);
						}
						// The combat is queued (CvUnitCombat::ResolveCombat runs from the pump); wait for A's line (with the flag) or a few calls (without).
						PumpUntilMessage(inst, seen, A, from);
						const int forA = seen.CountFor(A, from), forShooter = seen.CountFor(fights[f].shooter, from), forAi = seen.CountFor(C, from) - (fights[f].shooter == C ? forShooter : 0);
						// The line of A: text and plot of the fight (the defender's line points at the shooter or the target).
						std::string first;
						bool plotOk = true, textOk = true;
						for (size_t i = from; i < seen.messages.size(); ++i)
							if (seen.messages[i].player == A)
							{
								if (first.empty()) first = seen.messages[i].text;
								const Msg& m = seen.messages[i];
								if (!((m.x == defAt->getX() && m.y == defAt->getY()) || (m.x == atkAt->getX() && m.y == atkAt->getY()))) plotOk = false;
								if (m.text.empty() || m.text.compare(0, 8, "TXT_KEY_") == 0) textOk = false;
							}
						const char* name = f == 0 ? "human_fight" : "ai_fight";
						if (kFlag)
							c.add(name, rcAttack >= 0 && forA > 0 && plotOk && textOk, "%s shoots A (flag on): rc %d %s, MESSAGE events for A %d (plot at the fight %d, text localized %d), for the shooter %d, for the AI %d; first for A: \"%s\"", fights[f].label, rcAttack, err.message,
								forA, plotOk ? 1 : 0, textOk ? 1 : 0, forShooter, forAi, first.c_str());
						else
							c.add(name, forA == 0, "%s shoots A (flag off, golden rule): rc %d, MESSAGE events for A %d (none expected), for the shooter %d", fights[f].label, rcAttack, forA, forShooter);
						c.add(f == 0 ? "human_fight_nonhuman" : "ai_fight_nonhuman", f == 0 ? forAi == 0 : true, "no MESSAGE event for the AI (%d)", forAi);
						DumpMessages(seen, fights[f].label, from);
						digests += Format("mpmsg-digest|after-%s-fight|%s\n", fights[f].label, Hex(RtSession::StateDigest()).c_str());
						if (CvUnit* a = shooterOwner.getUnit(atkId)) a->kill(false);
						if (CvUnit* d = pA.getUnit(defId)) d->kill(false);
					}

					// Another branch (CvTeam::DoDeclareWar, "you declared war on ..."): B's team declares war on the AI with the server seat as the active player.
					{
						const size_t from = seen.messages.size();
						const bool warBefore = GET_TEAM(tB).isAtWar(tC);
						if (!warBefore) GET_TEAM(tB).declareWar(tC);
						PumpUntilMessage(inst, seen, B, from);  // the queued events are emitted by the next call
						const int forB = seen.CountFor(B, from), forA = seen.CountFor(A, from);
						std::string first;
						for (size_t i = from; i < seen.messages.size(); ++i) if (seen.messages[i].player == B) { first = seen.messages[i].text; break; }
						if (kFlag) c.add("war_branch", GET_TEAM(tB).isAtWar(tC) && forB > 0, "B declares war on the AI (flag on; at war before: %d): MESSAGE events for B %d, for A %d; first for B: \"%s\"", warBefore ? 1 : 0, forB, forA, first.c_str());
						else c.add("war_branch", GET_TEAM(tB).isAtWar(tC) && forB == 0, "B declares war on the AI (flag off, golden rule): MESSAGE events for B %d (none expected), for A %d", forB, forA);
						DumpMessages(seen, "war", from);
						digests += Format("mpmsg-digest|after-war|%s\n", Hex(RtSession::StateDigest()).c_str());
					}

					// Another branch: A's team learns a technology (announced only when the game is final-initialized).
					if (game.isFinalInitialized())
					{
						TechTypes tech = NO_TECH;
						for (int t = 0; t < GC.getNumTechInfos() && tech == NO_TECH; ++t)
							if (!GET_TEAM(tA).GetTeamTechs()->HasTech((TechTypes)t)) tech = (TechTypes)t;
						const size_t from = seen.messages.size();
						if (tech != NO_TECH) GET_TEAM(tA).setHasTech(tech, true, (PlayerTypes)A, false, true);
						PumpUntilMessage(inst, seen, A, from);  // the queued events are emitted by the next call
						const int forA = seen.CountFor(A, from), forB = seen.CountFor(B, from);
						std::string first;
						for (size_t i = from; i < seen.messages.size(); ++i) if (seen.messages[i].player == A) { first = seen.messages[i].text; break; }
						if (kFlag) c.add("tech_branch", tech != NO_TECH && forA > 0, "setHasTech(tech %d) for A (flag on): MESSAGE events for A %d, for B %d; first for A: \"%s\"", (int)tech, forA, forB, first.c_str());
						else c.add("tech_branch", tech != NO_TECH, "setHasTech(tech %d) for A (flag off): MESSAGE events for A %d, for B %d", (int)tech, forA, forB);
						DumpMessages(seen, "tech", from);
						digests += Format("mpmsg-digest|after-tech|%s\n", Hex(RtSession::StateDigest()).c_str());
					}

					// A few turns on (the test bots play both humans): the digests of the turns must not depend on the flag either.
					const int target = startTurn + (int)turns;
					int turn = startTurn;
					const long long maxCalls = 4000LL * (turns + 1);
					std::string endNote;
					std::vector<RtAutoplay> bots(MAX_PLAYERS);
					for (long long calls = 0; turn < target; ++calls)
					{
						if (calls > maxCalls) { stopReason = Format("turn %d: too many gc_pump calls", turn); break; }
						if (gc_pump(inst, 64, &pr, &err) != GC_OK || pr.status == GC_PUMP_STUCK || pr.status == GC_PUMP_GAME_OVER) { stopReason = Format("gc_pump at turn %d: status %d %s", pr.turn, (int)pr.status, err.message); break; }
						if (pr.status == GC_PUMP_WAITING_HUMAN)
						{
							// The test bots (rt_autoplay) play the waiting humans: orders for their units, then the end of the turn.
							const std::vector<PlayerTypes> waiting = session->WaitingHumans();
							for (size_t i = 0; i < waiting.size(); ++i)
							{
								std::string why;
								if (!bots[(int)waiting[i]].ActAs(*session, (int)waiting[i], why) && endNote.empty()) endNote = Format("turn %d player %d: the bot cannot go on: %s", turn, (int)waiting[i], why.c_str());
							}
						}
						if (pr.turn != turn)
						{
							doneTurns += pr.turn - turn;
							turn = pr.turn;
							digests += Format("mpmsg-digest|turn-%d|%s\n", turn, Hex(RtSession::StateDigest()).c_str());
						}
					}
					c.add("turns", stopReason.empty() && doneTurns == (int)turns, "%d/%u turn(s) after the fights%s%s", doneTurns, turns, stopReason.empty() ? "" : ": ", stopReason.c_str());
					if (!endNote.empty()) printf("mpmsg-note|%s\n", endNote.c_str());
				}
			}
			else if (ready) c.add("unit_types", false, "no UNIT_WARRIOR / UNIT_ARCHER");
		}
	}
	for (size_t i = 0; i < seen.lines.size(); ++i) printf("mpmsg-event|%s\n", seen.lines[i].c_str());
	c.add("lua_errors", seen.luaErrors == 0, "%d LUA_ERROR event(s)", seen.luaErrors);
	c.add("log_errors", seen.logErrors == 0, "%d error(s) logged", seen.logErrors);
	printf("%s", digests.c_str());
	const std::string out = Format("{\"failed\":%d,\"flag\":%s,\"seed\":%u,\"humans\":%u,\"ai\":%u,\"messages\":%d,\"lua_errors\":%d,\"log_errors\":%d,\"checks\":[%s]}", c.failed, kFlag ? "true" : "false", seed, humans, ai,
		(int)seen.messages.size(), seen.luaErrors, seen.logErrors, c.json.c_str());
	if (report && report_len)
	{
		strncpy(report, out.c_str(), report_len - 1);
		report[report_len - 1] = '\0';
	}
	printf("mpmsg-summary|flag %s|seed %u|%d MESSAGE event(s)|%d check(s) failed\n", kFlag ? "on" : "off", seed, (int)seen.messages.size(), c.failed);
	if (inst) gc_destroy(inst);
	return c.failed == 0 ? 0 : 1;
}
