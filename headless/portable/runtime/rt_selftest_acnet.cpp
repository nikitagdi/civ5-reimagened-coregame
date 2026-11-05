// Scenarios `autochoice` and `autochoice2` in the network game type (task N3.14; run.sh --scenario autochoice|autochoice2
// --game-type network --humans N --ai A --turns T): rt_autochoice for each human of a Pitboss game (SIMP-002), the AI's requests to the humans
// (SIMP-003), the attempt counters per human.
// A network game with N test-bot humans (rt_autoplay, one per human; the choices without a screen are left to rt_autochoice as for a person) and A AI.
//  part 1 (`autochoice`):
//   1. auto_choices.research = 0: at turn 0 every human waits on his own research blocker, "end turn" of each is REJECTED_BLOCKED with it, no research
//      event; then the flag goes on and every human's blocker is cleared by one RESEARCH event with his player (N events, N players);
//   2. T turns with every flag on: no human ever waits on a part-1 blocker (research, free tech, policy, free policy, ideology);
//   3. halfway every human gets 1 free technology and 1 free policy (as his own action: ActiveAs), each must come out as FREE_TECH and POLICY
//      events of that human;
//   4. every human's team is moved to ERA_MODERN (scenario-only), up to 5 more turns bring an IDEOLOGY event for each human.
//  part 2 (`autochoice2`): after a few bot turns, with promotion / great_person / goody_hut off, each human gets a different state to decide
//   (a promotion ready, a free great person, a faith great person, a Maya Long Count great person, a ruin reward with the picker promotion; two
//   humans get the ruin reward, to catch a shared list): each human waits on his own blocker (a ruin reward: on his own pending popup), then with the
//   flags on each state is resolved by one event of that human and shows in the gamecore.
//  Diplomacy (both parts): the AI's requests to each human are counted; none may stay open (RtDiplo::Pending) at a WAITING step and none may hold the turn
//  (no STUCK); TRADE_AI_MAKES_OFFER does not occur in the network game type (CvDiplomacyRequests.cpp:245-254).
// Prints "acnet-event|turn|player|kind|text" lines.
#include "CvGameCoreDLLPCH.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "CvDiplomacyRequests.h"
#include "CvDllContext.h"
#include "CvPolicyClasses.h"
#include "CvReligionClasses.h"
#include "CvTechClasses.h"
#include "CvTypes.h"
#include "rt_active.h"
#include "rt_autochoice.h"
#include "rt_autoplay.h"
#include "rt_diplo.h"
#include "rt_selftest.h"
#include "rt_session.h"
#include "rt_ui.h"
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

std::string Num(long long v) { return Format("%lld", v); }

struct Check
{
	std::string json;
	int failed;
	const char* scenario;
	Check() : failed(0), scenario("autochoice") {}

	void add(const char* name, bool ok, const char* fmt, ...) __attribute__((format(printf, 4, 5)))
	{
		char detail[1024];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(detail, sizeof(detail), fmt, ap);
		va_end(ap);
		json += std::string(json.empty() ? "" : ",") + "{\"check\":\"" + name + "\",\"ok\":" + (ok ? "true" : "false") + ",\"detail\":\"" + Escape(detail) + "\"}";
		printf("%s: %-4s %s: %s\n", scenario, ok ? "ok" : "FAIL", name, detail);
		if (!ok) ++failed;
	}
};

void ForwardLog(void* user, int level, const char* msg)
{
	const gc_host* host = static_cast<const gc_host*>(user);
	if (host && host->log) host->log(host->user, level, msg);
}

bool IsPart1Blocker(int b)
{
	return b == ENDTURN_BLOCKING_RESEARCH || b == ENDTURN_BLOCKING_FREE_TECH || b == ENDTURN_BLOCKING_POLICY || b == ENDTURN_BLOCKING_FREE_POLICY ||
		b == ENDTURN_BLOCKING_CHOOSE_IDEOLOGY;
}

struct X
{
	RtSession* s;
	RtAutoChoice* ac;
	std::vector<int> humans;
	std::vector<RtAutoplay> bots;
	std::vector<RtAutoChoiceEvent> events;  // every event drained so far
	RtSession::PumpResult r;
	std::string stop;
	int stuck, rounds, aiWithDiploOpen;
	X() : s(NULL), ac(NULL), bots(MAX_PLAYERS), stuck(0), rounds(0), aiWithDiploOpen(0) {}

	bool IsHuman(int p) const
	{
		for (size_t i = 0; i < humans.size(); ++i)
			if (humans[i] == p) return true;
		return false;
	}
	// Events of `kind` (any kind: -1) of `player` (any: -1) from index `from` on.
	int Count(int kind, int player, size_t from = 0) const
	{
		int n = 0;
		for (size_t i = from; i < events.size(); ++i)
			if ((kind < 0 || (int)events[i].kind == kind) && (player < 0 || events[i].player == player)) ++n;
		return n;
	}
};

void Drain(X& x)
{
	RtAutoChoiceEvent ev;
	while (x.ac->PopEvent(ev))
	{
		x.events.push_back(ev);
		printf("acnet-event|%d|%d|%s|%s\n", ev.turn, ev.player, RtAutoChoice::KindName(ev.kind), ev.text.c_str());
	}
}

// One pump call; false (and x.stop) on STUCK / GAME_OVER. x.r is the result.
bool PumpOnce(X& x, uint32_t slices = 64)
{
	x.r = x.s->Pump(slices);
	Drain(x);
	if (x.r.status == RtSession::PUMP_STUCK) { ++x.stuck; x.stop = Format("STUCK at turn %d: %s", x.r.turn, x.r.reason.c_str()); return false; }
	if (x.r.status == RtSession::PUMP_GAME_OVER) { x.stop = Format("GAME_OVER at turn %d", x.r.turn); return false; }
	return true;
}

int BlockerOf(X& x, int who) { return x.s->CurrentBlocking(NULL, (PlayerTypes)who); }

// Pump until the humans are up (WAITING_HUMAN).
bool ToWaiting(X& x, int budget = 20000)
{
	for (int i = 0; i < budget; ++i)
	{
		if (!PumpOnce(x)) return false;
		if (x.r.status == RtSession::PUMP_WAITING_HUMAN) return true;
	}
	x.stop = Format("turn %d: no WAITING_HUMAN within %d pump(s)", x.r.turn, budget);
	return false;
}

// A bot round for each waiting human. False (x.stop) when a bot cannot go on.
bool BotRounds(X& x, int skipBlocker = -2)
{
	const std::vector<PlayerTypes> waiting = x.s->WaitingHumans();
	for (size_t j = 0; j < waiting.size(); ++j)
	{
		const PlayerTypes who = waiting[(j + (size_t)x.rounds) % waiting.size()];
		if (!x.s->IsHumanWaiting(who)) continue;
		if (skipBlocker != -2 && BlockerOf(x, (int)who) == skipBlocker) continue;  // this human waits for the scenario
		std::string why;
		++x.rounds;
		if (!x.bots[(int)who].ActAs(*x.s, (int)who, why))
		{
			x.stop = Format("turn %d human %d: %s", x.r.turn, (int)who, why.c_str());
			// What the bot saw: the cities of the human and what each can start (a production blocker the bot could not answer).
			CvPlayer& p = GET_PLAYER(who);
			int loop;
			for (CvCity* city = p.firstCity(&loop); city; city = p.nextCity(&loop))
			{
				int units = 0, buildings = 0;
				for (int i = 0; i < GC.getNumUnitInfos(); ++i) if (city->canTrain((UnitTypes)i)) ++units;
				for (int i = 0; i < GC.getNumBuildingInfos(); ++i) if (city->canConstruct((BuildingTypes)i)) ++buildings;
				printf("acnet-debug|player %d city %d (%d,%d): can train %d, can construct %d, orders %d, resistance %d, razing %d, puppet %d\n", (int)who, city->GetID(), city->getX(), city->getY(), units, buildings,
					city->getOrderQueueLength(), city->IsResistance() ? 1 : 0, city->IsRazing() ? 1 : 0, city->IsPuppet() ? 1 : 0);
			}
			return false;
		}
	}
	return true;
}

// Play with the bots until the game turn changes. `violations` counts the humans found waiting on a part-1 blocker (flags on).
bool PlayTurn(X& x, int* violations, std::string* firstViolation)
{
	const int turn = GC.getGame().getGameTurn();
	for (long long i = 0; i < 400000; ++i)
	{
		if (!PumpOnce(x)) return false;
		if (x.r.turn != turn) return true;
		if (x.r.status != RtSession::PUMP_WAITING_HUMAN) continue;
		if (x.s->Diplo().Pending() > 0) ++x.aiWithDiploOpen;  // an AI message nobody answered at a WAITING step
		const std::vector<PlayerTypes> waiting = x.s->WaitingHumans();
		for (size_t j = 0; j < waiting.size(); ++j)
		{
			const int b = BlockerOf(x, (int)waiting[j]);
			if (violations && IsPart1Blocker(b))
			{
				++*violations;
				if (firstViolation && firstViolation->empty()) *firstViolation = Format("turn %d human %d: %s", turn, (int)waiting[j], RtSession::BlockingName(b));
			}
		}
		if (!BotRounds(x)) return false;
	}
	x.stop = Format("turn %d did not end", turn);
	return false;
}

// ---- state setup of part 2 ---------------------------------------------------------------------------------------------------

enum Kind { K_PROMOTION, K_FREE_GP, K_FAITH_GP, K_MAYA_GP, K_GOODY, K_COUNT };

struct KindInfo
{
	const char* name;
	int blocker;  // NO_ENDTURN_BLOCKING_TYPE: none (a popup)
	RtAutoChoiceEvent::Kind event;
};

const KindInfo kKinds[K_COUNT] = {
	{"promotion", ENDTURN_BLOCKING_UNIT_PROMOTION, RtAutoChoiceEvent::PROMOTION},
	{"free_great_person", ENDTURN_BLOCKING_FREE_ITEMS, RtAutoChoiceEvent::FREE_GREAT_PERSON},
	{"faith_great_person", ENDTURN_BLOCKING_FAITH_GREAT_PERSON, RtAutoChoiceEvent::FAITH_GREAT_PERSON},
	{"maya_great_person", ENDTURN_BLOCKING_MAYA_LONG_COUNT, RtAutoChoiceEvent::MAYA_GREAT_PERSON},
	{"goody_hut", NO_ENDTURN_BLOCKING_TYPE, RtAutoChoiceEvent::GOODY_HUT}};

// The kind of human number k (two of the humans get the ruin reward whenever there are four or more).
Kind KindOf(size_t k)
{
	static const Kind order[6] = {K_GOODY, K_PROMOTION, K_FREE_GP, K_GOODY, K_FAITH_GP, K_MAYA_GP};
	return order[k % 6];
}

struct HumanState
{
	int player;
	Kind kind;
	int unitsBefore, unitId, levelBefore;
	bool setup;
	std::string what, problem;
	HumanState() : player(-1), kind(K_PROMOTION), unitsBefore(0), unitId(-1), levelBefore(0), setup(false) {}
};

CvPlot* FreePlotNearCapital(CvPlayer& p, int minRadius)
{
	CvCity* capital = p.getCapitalCity();
	if (!capital) return NULL;
	for (int radius = minRadius; radius <= 4; ++radius)
		for (int dx = -radius; dx <= radius; ++dx)
			for (int dy = -radius; dy <= radius; ++dy)
			{
				CvPlot* pl = GC.getMap().plot(capital->getX() + dx, capital->getY() + dy);
				if (pl && !pl->isWater() && !pl->isImpassable() && !pl->isMountain() && !pl->getPlotCity() && pl->getNumUnits() == 0 && pl->getImprovementType() == NO_IMPROVEMENT &&
					(pl->getOwner() == NO_PLAYER || pl->getOwner() == p.GetID()))
					return pl;
			}
	return NULL;
}

// As the human's own action (ActiveAs: the core opens the reward popup and the free-item notifications only for the active player).
void SetupHuman(X& x, HumanState& h)
{
	ActiveAs as((PlayerTypes)h.player);
	CvPlayer& p = GET_PLAYER((PlayerTypes)h.player);
	h.unitsBefore = p.getNumUnits();
	if (!p.getCapitalCity()) { h.problem = "no capital"; return; }
	switch (h.kind)
	{
	case K_PROMOTION:
	{
		int loop;
		CvUnit* unit = NULL;
		for (CvUnit* u = p.firstUnit(&loop); u && !unit; u = p.nextUnit(&loop))
			if (u->IsCombatUnit() && !u->isPromotionReady()) unit = u;
		if (!unit) { h.problem = "no combat unit to promote"; return; }
		h.unitId = unit->GetID();
		h.levelBefore = unit->getLevel();
		unit->changeExperience(unit->experienceNeeded() + 5);
		unit->testPromotionReady();
		h.what = Format("unit %d %s: promotion ready %d", unit->GetID(), unit->getUnitInfo().GetType(), unit->isPromotionReady() ? 1 : 0);
		if (!unit->isPromotionReady()) h.problem = "the experience did not make the promotion ready";
		break;
	}
	case K_FREE_GP:
		p.ChangeNumFreeGreatPeople(1);
		h.what = Format("ChangeNumFreeGreatPeople(1) -> %d", p.GetNumFreeGreatPeople());
		break;
	case K_FAITH_GP:
		p.ChangeNumFaithGreatPeople(1);
		h.what = Format("ChangeNumFaithGreatPeople(1) -> %d", p.GetNumFaithGreatPeople());
		break;
	case K_MAYA_GP:
		p.ChangeNumMayaBoosts(1);
		h.what = Format("ChangeNumMayaBoosts(1) -> %d", p.GetNumMayaBoosts());
		break;
	case K_GOODY:
	{
		CvPlot* hut = FreePlotNearCapital(p, 2);
		if (!hut) { h.problem = "no free plot for the ruin"; return; }
		const ImprovementTypes imp = (ImprovementTypes)GC.getInfoTypeForString("IMPROVEMENT_GOODY_HUT", true);
		if (imp == NO_IMPROVEMENT) { h.problem = "no IMPROVEMENT_GOODY_HUT"; return; }
		CvUnit* u = p.initUnit((UnitTypes)GC.getInfoTypeForString("UNIT_WARRIOR", true), hut->getX(), hut->getY());
		if (!u) { h.problem = "no unit could be created"; return; }
		h.unitId = u->GetID();
		u->setHasPromotion((PromotionTypes)GC.getPROMOTION_GOODY_HUT_PICKER(), true);
		hut->setImprovementType(imp);
		if (!hut->isGoody()) { h.problem = "the plot is not a goody hut"; return; }
		p.doGoody(hut, u);
		h.unitsBefore = p.getNumUnits();  // the warrior is not part of the comparison
		h.what = Format("unit %d with the picker promotion popped a hut at (%d,%d)", u->GetID(), hut->getX(), hut->getY());
		break;
	}
	default: break;
	}
	h.setup = h.problem.empty();
}

std::string VerifyHuman(X& x, const HumanState& h)
{
	CvPlayer& p = GET_PLAYER((PlayerTypes)h.player);
	switch (h.kind)
	{
	case K_PROMOTION:
	{
		CvUnit* u = p.getUnit(h.unitId);
		if (!u) return "the unit is gone";
		return (u->getLevel() > h.levelBefore && !u->isPromotionReady()) ? "" : Format("level %d (was %d), promotion ready %d", u->getLevel(), h.levelBefore, u->isPromotionReady() ? 1 : 0);
	}
	case K_FREE_GP:
		return (p.GetNumFreeGreatPeople() == 0 && p.getNumUnits() == h.unitsBefore + 1) ? "" : Format("free great people %d, units %d (was %d)", p.GetNumFreeGreatPeople(), p.getNumUnits(), h.unitsBefore);
	case K_FAITH_GP:
		return (p.GetNumFaithGreatPeople() == 0 && p.getNumUnits() == h.unitsBefore + 1) ? "" : Format("faith great people %d, units %d (was %d)", p.GetNumFaithGreatPeople(), p.getNumUnits(), h.unitsBefore);
	case K_MAYA_GP:
		return (p.GetNumMayaBoosts() == 0 && p.getNumUnits() == h.unitsBefore + 1) ? "" : Format("Maya boosts %d, units %d (was %d)", p.GetNumMayaBoosts(), p.getNumUnits(), h.unitsBefore);
	case K_GOODY:
		return x.ac->PendingGoodies(*x.s, (PlayerTypes)h.player) == 0 ? "" : "the ruin reward is still pending";
	default: return "?";
	}
}

void DiploChecks(X& x, Check& c, int turnsRun)
{
	RtDiplo& dp = x.s->Diplo();
	std::map<int, int> to, requests;
	int offers = 0, notHuman = 0;
	const std::vector<RtDiploEvent>& evs = dp.Events();
	for (size_t i = 0; i < evs.size(); ++i)
	{
		++to[evs[i].to];
		if (evs[i].request) ++requests[evs[i].to];
		if (evs[i].state == DIPLO_UI_STATE_TRADE_AI_MAKES_OFFER) ++offers;
		if (!x.IsHuman(evs[i].to)) ++notHuman;
	}
	std::string per;
	for (size_t h = 0; h < x.humans.size(); ++h) per += Format("%sp%d %d (%d request(s))", per.empty() ? "" : ", ", x.humans[h], to[x.humans[h]], requests[x.humans[h]]);
	c.add("diplo_answered", dp.Pending() == 0 && x.aiWithDiploOpen == 0 && x.stuck == 0,
		"AI messages to the humans over %d turn(s): %d in all (%s); %d still queued at the end, %d WAITING step(s) with one open, %d STUCK; no AI request held a turn", turnsRun, dp.Total(),
		per.c_str(), dp.Pending(), x.aiWithDiploOpen, x.stuck);
	c.add("diplo_target", notHuman == 0, "%d AI message(s) answered for a player who is not a human (the server seat)", notHuman);
	c.add("no_offers_in_network", offers == 0, "%d TRADE_AI_MAKES_OFFER message(s) (the core makes deals to the active player only, and the server seat is active in the AI phase)", offers);
}

}  // namespace

extern "C" int rt_selftest_autochoice_net(const gc_host* host, unsigned seed, unsigned turns, unsigned humans, unsigned ai, int part, char* report, size_t report_len)
{
	SetLogSink(ForwardLog, const_cast<gc_host*>(host));
	Check c;
	c.scenario = part == 2 ? "autochoice2" : "autochoice";
	X x;

	RtSession* session = new RtSession();  // left alive on purpose, as in scenario `load`
	RtSession::LoadStatus ls;
	const bool loaded = session->Load(*host, ls);
	c.add("load", loaded, "boot reached step %s%s%s", RtSession::StepName(ls.step), ls.error.empty() ? "" : ": ", ls.error.c_str());

	std::string players;
	for (unsigned i = 0; i < humans; ++i)
		players += Format("%s{\"kind\":\"human\",\"civ\":\"%s\",\"nick\":\"bot%u\"}", i ? "," : "", i ? "random" : "CIVILIZATION_ROME", i);
	for (unsigned i = 0; i < ai; ++i) players += std::string(humans || i ? "," : "") + "{\"kind\":\"ai\",\"civ\":\"random\"}";
	// Part 1 starts with the research choice off; part 2 with everything on (the bots play on until the states are set up).
	const std::string flags = part == 2 ? std::string() : std::string(",\"auto_choices\":{\"research\":0}");
	const std::string setup = Format("{\"seed\":%u,\"game_type\":\"network\",\"players\":[%s],\"turn_mode\":\"simultaneous\"%s}", seed, players.c_str(), flags.c_str());
	RtSession::NewGameStatus st;
	const bool started = loaded && session->NewGame(*host, setup, st);
	c.add("new_game", started, "%s reached step %s%s%s", setup.c_str(), RtSession::NewGameStepName(st.step), st.error.empty() ? "" : ": ", st.error.c_str());

	int startTurn = -1, endTurn = -1, doneTurns = 0, luaErrors = 0;
	std::string summary;
	if (started)
	{
		x.s = session;
		x.ac = &session->AutoChoice();
		RtScriptSystem* ss = session->Scripts();
		const int luaBefore = ss ? ss->GetErrorCount() : 0;
		for (int i = 0; i < MAX_PLAYERS; ++i)
		{
			const CvPlayer& p = GET_PLAYER((PlayerTypes)i);
			if (p.isAlive() && p.isHuman() && !p.isObserver()) x.humans.push_back(i);
		}
		// Scenario-only: every team has met every other (the AI sends its requests to a human it has met; on a small map that takes many turns).
		for (int a = 0; a < MAX_MAJOR_CIVS; ++a)
			for (int b = a + 1; b < MAX_MAJOR_CIVS; ++b)
			{
				if (!GET_PLAYER((PlayerTypes)a).isAlive() || !GET_PLAYER((PlayerTypes)b).isAlive() || GET_PLAYER((PlayerTypes)a).isObserver() || GET_PLAYER((PlayerTypes)b).isObserver()) continue;
				if (!GET_TEAM(GET_PLAYER((PlayerTypes)a).getTeam()).isHasMet(GET_PLAYER((PlayerTypes)b).getTeam())) GET_TEAM(GET_PLAYER((PlayerTypes)a).getTeam()).meet(GET_PLAYER((PlayerTypes)b).getTeam(), true);
			}
		startTurn = GC.getGame().getGameTurn();
		c.add("humans", x.humans.size() == humans, "%d human(s) seated, server seat %d", (int)x.humans.size(), (int)ServerSeat());

		if (part == 1 || part == 0)
		{
			// 1. research off: every human waits on his own blocker.
			// The research blocker comes after the unit blockers (a settler to move): the bots play the other blockers until every human is on research.
			bool ok = ToWaiting(x);
			for (long long i = 0; ok && i < 100000; ++i)
			{
				bool all = true;
				for (size_t h = 0; h < x.humans.size(); ++h)
					if (BlockerOf(x, x.humans[h]) != ENDTURN_BLOCKING_RESEARCH) all = false;
				if (all) break;
				if (x.r.turn != startTurn) { x.stop = Format("turn %d: the turn changed before every human was on the research blocker", x.r.turn); ok = false; break; }
				if (x.r.status == RtSession::PUMP_WAITING_HUMAN && !BotRounds(x, ENDTURN_BLOCKING_RESEARCH)) { ok = false; break; }
				if (!PumpOnce(x)) { ok = false; break; }
			}
			if (ok && x.r.status != RtSession::PUMP_WAITING_HUMAN) ok = ToWaiting(x);
			int waitingOnResearch = 0, rejected = 0, wrongPlayer = 0;
			std::string detail;
			if (ok)
			{
				const std::vector<PlayerTypes> waiting = x.s->WaitingHumans();
				for (size_t h = 0; h < x.humans.size(); ++h)
				{
					const int who = x.humans[h];
					const int b = BlockerOf(x, who);
					if (b == ENDTURN_BLOCKING_RESEARCH) ++waitingOnResearch;
					else detail += Format("%shuman %d: %s", detail.empty() ? "" : "; ", who, RtSession::BlockingName(b));
					RtSession::EndTurnResult e = x.s->EndTurn(false, (PlayerTypes)who);
					if (e.status == RtSession::END_TURN_REJECTED_BLOCKED && e.blocking == ENDTURN_BLOCKING_RESEARCH) ++rejected;
				}
				c.add("research_off_waits", waitingOnResearch == (int)x.humans.size() && rejected == (int)x.humans.size() && waiting.size() == x.humans.size() && x.Count(-1, -1) == 0,
					"research auto off, turn %d: %d of %d human(s) wait on their own research blocker (%d waiting), end turn rejected with it for %d, %d event(s)%s%s", x.r.turn,
					waitingOnResearch, (int)x.humans.size(), (int)waiting.size(), rejected, x.Count(-1, -1), detail.empty() ? "" : "; other: ", detail.c_str());
				RtAutoChoices f = x.ac->Flags(*x.s);
				f.research = 1;
				x.ac->SetFlags(f);
				ok = ToWaiting(x);
			}
			if (ok)
			{
				std::set<int> players;
				for (size_t i = 0; i < x.events.size(); ++i)
					if (x.events[i].kind == RtAutoChoiceEvent::RESEARCH) { players.insert(x.events[i].player); if (!x.IsHuman(x.events[i].player)) ++wrongPlayer; }
				int still = 0;
				for (size_t h = 0; h < x.humans.size(); ++h)
					if (BlockerOf(x, x.humans[h]) == ENDTURN_BLOCKING_RESEARCH) ++still;
				c.add("research_on_resolves", players.size() == x.humans.size() && wrongPlayer == 0 && still == 0,
					"after switching research auto on: RESEARCH events for %d of %d human(s) (%d for a non-human), %d human(s) still on the research blocker", (int)players.size(),
					(int)x.humans.size(), wrongPlayer, still);
			}

			// 2-4. the turns with every flag on; free grants halfway; the ideology.
			int violations = 0;
			std::string firstViolation;
			size_t grantFrom = 0;
			bool granted = false, eraDone = false, injected = false;
			int eraTurn = -1, injectedWarnings = 0, injectedStatements = 0;
			const int target = startTurn + (int)turns;
			while (x.stop.empty() && GC.getGame().getGameTurn() < target + 5)
			{
				const int turn = GC.getGame().getGameTurn();
				if (!granted && turn >= startTurn + (int)turns / 2)
				{
					granted = true;
					grantFrom = x.events.size();
					for (size_t h = 0; h < x.humans.size(); ++h)
					{
						ActiveAs as((PlayerTypes)x.humans[h]);
						CvPlayer& p = GET_PLAYER((PlayerTypes)x.humans[h]);
						p.chooseTech(1, "free technology (autochoice scenario)", NO_TECH);
						p.ChangeNumFreePolicies(1);
					}
					printf("acnet-grant|%d|1 free tech, 1 free policy to each of %d human(s)\n", turn, (int)x.humans.size());
				}
				if (!injected && turn >= startTurn + 3)
				{
					// The AI's requests to each human by the gamecore's own sender (CvDiplomacyRequests::SendRequest queues them per human in a network game): a warning with
					// buttons (answered "no") and a plain statement (dismissed). Both must come out answered, for the right human, without holding the turn.
					injected = true;
					PlayerTypes from = NO_PLAYER;
					for (int i = 0; i < MAX_MAJOR_CIVS && from == NO_PLAYER; ++i)
						if (GET_PLAYER((PlayerTypes)i).isAlive() && !GET_PLAYER((PlayerTypes)i).isHuman() && GET_PLAYER((PlayerTypes)i).isMajorCiv()) from = (PlayerTypes)i;
					for (size_t h = 0; h < x.humans.size() && from != NO_PLAYER; ++h)
					{
						CvDiplomacyRequests::SendRequest(from, (PlayerTypes)x.humans[h], DIPLO_UI_STATE_DISCUSS_AGGRESSIVE_MILITARY_WARNING, "scenario: a warning", LEADERHEAD_ANIM_NEGATIVE, 0);
						CvDiplomacyRequests::SendRequest(from, (PlayerTypes)x.humans[h], DIPLO_UI_STATE_BLANK_DISCUSSION, "scenario: a statement", LEADERHEAD_ANIM_POSITIVE, -1);
						++injectedWarnings;
						++injectedStatements;
					}
					printf("acnet-diplo|%d|AI player %d queued a warning and a statement for each of %d human(s)\n", turn, (int)from, (int)x.humans.size());
				}
				if (!eraDone && turn >= target)
				{
					eraDone = true;
					eraTurn = turn;
					const EraTypes modern = (EraTypes)GC.getInfoTypeForString("ERA_MODERN");
					for (size_t h = 0; h < x.humans.size(); ++h) GET_TEAM(GET_PLAYER((PlayerTypes)x.humans[h]).getTeam()).SetCurrentEra(modern);
					printf("acnet-era|%d|ERA_MODERN for the %d human team(s)\n", turn, (int)x.humans.size());
				}
				if (turn >= target)
				{
					bool all = true;
					for (size_t h = 0; h < x.humans.size(); ++h)
						if (x.Count(RtAutoChoiceEvent::IDEOLOGY, x.humans[h]) == 0) all = false;
					if (all) break;
				}
				if (!PlayTurn(x, &violations, &firstViolation)) break;
				++doneTurns;
			}
			endTurn = GC.getGame().getGameTurn();
			c.add("turns", x.stop.empty() && endTurn >= target, "%d turn(s) played (turn %d -> %d) for %d human(s)%s%s", doneTurns, startTurn, endTurn, (int)x.humans.size(), x.stop.empty() ? "" : ": ", x.stop.c_str());
			c.add("never_stopped_on_auto", violations == 0, "%d WAITING step(s) with a human on a part-1 blocker with its flag on%s%s", violations, firstViolation.empty() ? "" : ": ", firstViolation.c_str());
			// Per human: the kinds of choices he got.
			std::string per;
			int missingResearch = 0, missingPolicy = 0, missingFree = 0, missingFreePolicy = 0, missingIdeology = 0, nonHuman = 0;
			for (size_t i = 0; i < x.events.size(); ++i)
				if (!x.IsHuman(x.events[i].player)) ++nonHuman;
			for (size_t h = 0; h < x.humans.size(); ++h)
			{
				const int who = x.humans[h];
				const int research = x.Count(RtAutoChoiceEvent::RESEARCH, who), freeTech = x.Count(RtAutoChoiceEvent::FREE_TECH, who, grantFrom);
				const int policy = x.Count(RtAutoChoiceEvent::POLICY, who) + x.Count(RtAutoChoiceEvent::POLICY_BRANCH, who);
				const int freePolicy = x.Count(RtAutoChoiceEvent::POLICY, who, grantFrom) + x.Count(RtAutoChoiceEvent::POLICY_BRANCH, who, grantFrom);
				const int ideology = x.Count(RtAutoChoiceEvent::IDEOLOGY, who);
				if (!research) ++missingResearch;
				if (!policy) ++missingPolicy;
				if (granted && !freeTech) ++missingFree;
				if (granted && !freePolicy) ++missingFreePolicy;
				if (!ideology) ++missingIdeology;
				per += Format("%sp%d research %d policy %d free tech %d free policy %d ideology %d", per.empty() ? "" : "; ", who, research, policy, freeTech, freePolicy, ideology);
			}
			c.add("choices_per_human", missingResearch == 0 && missingPolicy == 0 && nonHuman == 0, "%s; %d event(s) for a non-human", per.c_str(), nonHuman);
			c.add("free_choices_per_human", granted && missingFree == 0 && missingFreePolicy == 0, "1 free tech + 1 free policy granted to each human: %d human(s) without a FREE_TECH, %d without a policy choice after it",
				missingFree, missingFreePolicy);
			int treeMissing = 0;
			for (size_t h = 0; h < x.humans.size(); ++h)
				if (GET_PLAYER((PlayerTypes)x.humans[h]).GetPlayerPolicies()->GetLateGamePolicyTree() == NO_POLICY_BRANCH_TYPE) ++treeMissing;
			c.add("ideology_per_human", eraDone && missingIdeology == 0 && treeMissing == 0, "ERA_MODERN at turn %d: %d human(s) without an IDEOLOGY event, %d without a late-game policy tree", eraTurn, missingIdeology, treeMissing);
			summary = per;
			int warnings = 0, statements = 0, wrongTarget = 0;
			std::map<int, int> warningsTo;
			const std::vector<RtDiploEvent>& dev = x.s->Diplo().Events();
			for (size_t i = 0; i < dev.size(); ++i)
			{
				if (dev[i].text == "scenario: a warning") { ++warnings; ++warningsTo[dev[i].to]; if (dev[i].answer != RTDIPLO_REPLIED_NO || !x.IsHuman(dev[i].to)) ++wrongTarget; }
				if (dev[i].text == "scenario: a statement") { ++statements; if (dev[i].answer != RTDIPLO_DISMISSED || !x.IsHuman(dev[i].to)) ++wrongTarget; }
			}
			for (size_t h = 0; h < x.humans.size(); ++h)
				if (warningsTo[x.humans[h]] != 1) ++wrongTarget;  // each human got his own warning, once
			c.add("diplo_requests_answered", injected && warnings == injectedWarnings && statements == injectedStatements && wrongTarget == 0,
				"AI requests queued for the humans by CvDiplomacyRequests::SendRequest: %d warning(s) of %d answered \"no\", %d statement(s) of %d dismissed, %d answered wrongly or for a non-human", warnings, injectedWarnings,
				statements, injectedStatements, wrongTarget);
		}
		else
		{
			// part 2: play until every human has a capital and a combat unit, then set the states up.
			std::vector<HumanState> hs(x.humans.size());
			for (size_t h = 0; h < hs.size(); ++h) { hs[h].player = x.humans[h]; hs[h].kind = KindOf(h); }
			bool assigned = false;
			int firstTurn = startTurn + 6;
			const int limit = startTurn + (int)turns;
			while (x.stop.empty() && !assigned && GC.getGame().getGameTurn() < limit)
			{
				const int turn = GC.getGame().getGameTurn();
				const int before = turn;
				bool ready = turn >= firstTurn;
				for (size_t h = 0; h < x.humans.size() && ready; ++h)
				{
					CvPlayer& p = GET_PLAYER((PlayerTypes)x.humans[h]);
					bool combat = false;
					int loop;
					for (CvUnit* u = p.firstUnit(&loop); u && !combat; u = p.nextUnit(&loop))
						combat = u->IsCombatUnit() && !u->isPromotionReady();
					if (!p.getCapitalCity() || !combat) ready = false;
				}
				// The setup is at the first WAITING step of a turn with every human waiting and nobody's turn ended.
				for (long long i = 0; i < 400000 && x.stop.empty(); ++i)
				{
					if (!PumpOnce(x)) break;
					if (x.r.turn != before) break;
					if (x.r.status != RtSession::PUMP_WAITING_HUMAN) continue;
					if (ready && x.s->WaitingHumans().size() == x.humans.size())
					{
						assigned = true;
						break;
					}
					if (!BotRounds(x)) break;
				}
				if (assigned) break;
				if (x.stop.empty()) ++doneTurns;
			}
			c.add("setup_reached", assigned && x.stop.empty(), "turn %d: every human waits with a capital and a combat unit%s%s", GC.getGame().getGameTurn(), x.stop.empty() ? "" : ": ", x.stop.c_str());
			if (assigned)
			{
				RtAutoChoices f = x.ac->Flags(*x.s);
				f.promotion = 0;
				f.great_person = 0;
				f.goody_hut = 0;
				x.ac->SetFlags(f);
				const size_t eventsFrom = x.events.size();
				int setups = 0;
				std::string problems;
				for (size_t h = 0; h < hs.size(); ++h)
				{
					SetupHuman(x, hs[h]);
					if (hs[h].setup) ++setups;
					else problems += Format("%sp%d %s: %s", problems.empty() ? "" : "; ", hs[h].player, kKinds[hs[h].kind].name, hs[h].problem.c_str());
					printf("acnet-setup|player %d|%s|%s\n", hs[h].player, kKinds[hs[h].kind].name, hs[h].setup ? hs[h].what.c_str() : hs[h].problem.c_str());
				}
				c.add("states_set_up", setups == (int)hs.size(), "%d of %d human(s) got a state to decide%s%s", setups, (int)hs.size(), problems.empty() ? "" : "; ", problems.c_str());
				// Flags off: each human waits on his own blocker / pending reward; nothing is chosen.
				bool ok = ToWaiting(x);
				int waits = 0, rejected = 0, rejectedExpected = 0, goodyOk = 0, goodyExpected = 0;
				std::string off;
				if (ok)
					for (size_t h = 0; h < hs.size(); ++h)
					{
						if (!hs[h].setup) continue;
						const KindInfo& k = kKinds[hs[h].kind];
						const int b = BlockerOf(x, hs[h].player);
						if (k.blocker == NO_ENDTURN_BLOCKING_TYPE)
						{
							++goodyExpected;
							if (x.ac->PendingGoodies(*x.s, (PlayerTypes)hs[h].player) == 1) ++goodyOk;
							else off += Format("%sp%d: %d pending reward(s)", off.empty() ? "" : "; ", hs[h].player, x.ac->PendingGoodies(*x.s, (PlayerTypes)hs[h].player));
							++waits;
						}
						else
						{
							if (b == k.blocker) ++waits;
							else off += Format("%sp%d %s: blocker %s", off.empty() ? "" : "; ", hs[h].player, k.name, RtSession::BlockingName(b));
							++rejectedExpected;
							RtSession::EndTurnResult e = x.s->EndTurn(false, (PlayerTypes)hs[h].player);
							if (e.status == RtSession::END_TURN_REJECTED_BLOCKED && e.blocking == k.blocker) ++rejected;
							else off += Format("%sp%d end turn: %s (%s) %s", off.empty() ? "" : "; ", hs[h].player, RtSession::EndTurnStatusName(e.status), RtSession::BlockingName(e.blocking), e.reason.c_str());
						}
					}
				c.add("flags_off_wait", ok && waits == setups && rejected == rejectedExpected && goodyOk == goodyExpected && x.events.size() == eventsFrom,
					"flags off: %d of %d human(s) wait on their own state, end turn rejected with it for %d of %d, %d of %d ruin reward(s) pending for the right human, %d event(s)%s%s", waits, setups, rejected,
					rejectedExpected, goodyOk, goodyExpected, (int)(x.events.size() - eventsFrom), off.empty() ? "" : "; ", off.c_str());
				// Flags on: each state is resolved by one event of that human.
				f.promotion = 1;
				f.great_person = 1;
				f.goody_hut = 1;
				x.ac->SetFlags(f);
				if (ok) ok = ToWaiting(x);
				int events = 0, state = 0, cross = 0;
				std::string on;
				for (size_t h = 0; h < hs.size() && ok; ++h)
				{
					if (!hs[h].setup) continue;
					const KindInfo& k = kKinds[hs[h].kind];
					const int mine = x.Count((int)k.event, hs[h].player, eventsFrom);
					if (mine == 1) ++events;
					else on += Format("%sp%d %s: %d event(s)", on.empty() ? "" : "; ", hs[h].player, k.name, mine);
					const std::string bad = VerifyHuman(x, hs[h]);
					if (bad.empty()) ++state;
					else on += Format("%sp%d %s: %s", on.empty() ? "" : "; ", hs[h].player, k.name, bad.c_str());
				}
				for (size_t i = eventsFrom; i < x.events.size(); ++i)
				{
					bool ofTheTest = false, expected = false;
					for (int k = 0; k < K_COUNT; ++k)
						if ((int)kKinds[k].event == (int)x.events[i].kind) ofTheTest = true;
					for (size_t h = 0; h < hs.size(); ++h)
						if (hs[h].player == x.events[i].player &&
							((int)kKinds[hs[h].kind].event == (int)x.events[i].kind || (hs[h].kind == K_GOODY && x.events[i].kind == RtAutoChoiceEvent::PROMOTION)))  // a ruin reward may be a promotion
							expected = true;
					if (ofTheTest && !expected)
					{
						++cross;
						on += Format("%sp%d: unexpected %s", on.empty() ? "" : "; ", x.events[i].player, RtAutoChoice::KindName(x.events[i].kind));
					}
				}
				c.add("flags_on_resolve", ok && events == setups && state == setups && cross == 0,
					"flags on: one event of the right kind for %d of %d human(s), the state shows it for %d, %d event(s) for another human or kind%s%s", events, setups, state, cross, on.empty() ? "" : "; ", on.c_str());
				for (size_t h = 0; h < hs.size(); ++h)
					summary += Format("%sp%d %s", summary.empty() ? "" : "; ", hs[h].player, kKinds[hs[h].kind].name);
				// And the game goes on: two more turns with the bots.
				int violations = 0;
				std::string firstViolation;
				for (int t = 0; t < 2 && x.stop.empty(); ++t)
					if (PlayTurn(x, &violations, &firstViolation)) ++doneTurns;
				c.add("play_on", x.stop.empty(), "two more turns with the bots after the choices%s%s", x.stop.empty() ? "" : ": ", x.stop.c_str());
			}
			endTurn = GC.getGame().getGameTurn();
		}
		luaErrors = ss ? ss->GetErrorCount() - luaBefore : 0;
		DiploChecks(x, c, doneTurns);
		c.add("no_stuck", x.stuck == 0, "%d stuck pump(s)", x.stuck);
		c.add("lua_errors", luaErrors == 0, "%d Lua error(s)", luaErrors);
	}

	const std::string out = "{\"failed\":" + Num(c.failed) + ",\"part\":" + Num(part == 2 ? 2 : 1) + ",\"seed\":" + Num(seed) + ",\"humans\":" + Num(humans) + ",\"ai_count\":" + Num(ai) +
		",\"turns_requested\":" + Num(turns) + ",\"turns_done\":" + Num(doneTurns) + ",\"start_turn\":" + Num(startTurn) + ",\"end_turn\":" + Num(endTurn) + ",\"events\":" + Num((long long)x.events.size()) +
		",\"stop_reason\":\"" + Escape(x.stop) + "\",\"summary\":\"" + Escape(summary) + "\",\"checks\":[" + c.json + "]}";
	if (report && report_len)
	{
		strncpy(report, out.c_str(), report_len - 1);
		report[report_len - 1] = '\0';
	}
	printf("%s-summary|seed %u|humans %u|ai %u|turns %d|events %d|%s\n", c.scenario, seed, humans, ai, doneTurns, (int)x.events.size(), summary.c_str());
	SetLogSink(NULL, NULL);
	return c.failed == 0 ? 0 : 1;
}
