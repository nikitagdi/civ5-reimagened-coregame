// Scenario `autochoice2` (task F4; run.sh --scenario autochoice2 --turns N --seed S --ai A): rt_autochoice part 2 (SIMP-002):
// promotion, free / faith / Maya great person, pantheon, religion, enhancement, reformation, captured city.
// RtSession::Load, NewGame({"seed":S,"ai_count":A,"auto_choices":{"religion":0,"promotion":0,"great_person":0,"captured_city":0}}) (the human in
// slot 0); the human founds a city (the scenario plays the blockers that stay with the human: production, units), then for each kind in turn:
//  1. state setup by scenario-only gamecore calls where natural play is too slow (unit experience, ChangeNumFreeGreatPeople /
//     ChangeNumFaithGreatPeople / ChangeNumMayaBoosts, faith for the pantheon, a Great Prophet unit on the capital plus the found / enhance
//     religion mission through the loopback, a policy with the reformation belief (the notification comes at once), an AI city created next to the capital and acquired by the
//     human with CvPlayer::acquireCity as a conquest does);
//  2. flag OFF: the pump stops WAITING_HUMAN on the kind's blocker (captured city: the city keeps its IsIgnoreCityForHappiness flag), "end turn"
//     is REJECTED_BLOCKED with it, no AUTO_CHOICE event of the kind appears;
//  3. flag ON (RtAutoChoice::SetFlags): the next pump clears the blocker by itself, one AUTO_CHOICE event of the kind appears and the
//     gamecore state shows the choice (a promotion gained, a great person unit, the pantheon / religion / enhancement / reformation
//     created, the city a puppet).
//  Task F5 adds the kinds World Congress proposals, World Congress votes, archaeology, ruin reward (goody hut) and tech steal (flags league, archaeology,
//  goody_hut, steal_tech), reached by scenario-only gamecore calls: CvGameLeagues::FoundLeague(human) + CvLeague::SetTurnsUntilSession(1); an archaeologist
//  and an antiquity site (AddArchaeologicalRecord) with the dig build completed by CvPlot::changeBuildProgress; a unit with the goody hut picker promotion
//  and CvPlayer::doGoody on a plot with a goody hut; the techs of a medieval tech given to an AI (its prerequisites to the human) with a stealable-tech list
//  and a SPY_STOLE_TECH notification. For every blocker kind the flag-off phase also checks the STUCK semantics: with RtSession::SetNoHumanInput(true) the same
//  pump returns STUCK, naming the blocker and the auto_choices flag. The run ends with the isolation check: research auto-choice off + a free
//  technology -> WAITING_HUMAN with a human, STUCK without one, resolved by switching the flag on.
// --turns is the turn budget for the whole run (default 40). Prints "autochoice2-phase|..." and "autochoice2-event|turn|kind|type|rule|text" lines.
#include "CvGameCoreDLLPCH.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "CvDllContext.h"
#include "CvCultureClasses.h"
#include "CvEspionageClasses.h"
#include "CvGameCoreUtils.h"
#include "CvImprovementClasses.h"
#include "CvPolicyClasses.h"
#include "CvReligionClasses.h"
#include "CvTechClasses.h"
#include "CvTypes.h"
#include "CvVotingClasses.h"
#include "rt_autochoice.h"
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
		printf("autochoice2: %-4s %s: %s\n", ok ? "ok" : "FAIL", name, detail);
		if (!ok) ++failed;
	}
};

void ForwardLog(void* user, int level, const char* msg)
{
	const gc_host* host = static_cast<const gc_host*>(user);
	if (host && host->log) host->log(host->user, level, msg);
}

enum Phase
{
	PH_PROMOTION, PH_FREE_GP, PH_FAITH_GP, PH_MAYA_GP, PH_PANTHEON, PH_RELIGION, PH_ENHANCEMENT, PH_REFORMATION, PH_CAPTURED, PH_LEAGUE_PROPOSAL, PH_LEAGUE_VOTE, PH_ARCHAEOLOGY, PH_GOODY, PH_STEAL, PH_COUNT
};

struct PhaseInfo
{
	const char* name;
	int blocker;                       // EndTurnBlockingTypes the kind blocks with (none for a captured city)
	RtAutoChoiceEvent::Kind kind;
	const char* flag;                  // the auto_choices flag
	int advance;                       // > 0: end turns (up to this many) until the blocker appears
};

const PhaseInfo kPhases[PH_COUNT] = {
	{"promotion", ENDTURN_BLOCKING_UNIT_PROMOTION, RtAutoChoiceEvent::PROMOTION, "promotion", 0},
	{"free_great_person", ENDTURN_BLOCKING_FREE_ITEMS, RtAutoChoiceEvent::FREE_GREAT_PERSON, "great_person", 0},
	{"faith_great_person", ENDTURN_BLOCKING_FAITH_GREAT_PERSON, RtAutoChoiceEvent::FAITH_GREAT_PERSON, "great_person", 0},
	{"maya_great_person", ENDTURN_BLOCKING_MAYA_LONG_COUNT, RtAutoChoiceEvent::MAYA_GREAT_PERSON, "great_person", 0},
	{"pantheon", ENDTURN_BLOCKING_FOUND_PANTHEON, RtAutoChoiceEvent::PANTHEON, "religion", 0},
	{"religion", ENDTURN_BLOCKING_FOUND_RELIGION, RtAutoChoiceEvent::RELIGION, "religion", 0},
	{"enhancement", ENDTURN_BLOCKING_ENHANCE_RELIGION, RtAutoChoiceEvent::ENHANCEMENT, "religion", 0},
	{"reformation", ENDTURN_BLOCKING_ADD_REFORMATION_BELIEF, RtAutoChoiceEvent::REFORMATION, "religion", 0},
	{"captured_city", NO_ENDTURN_BLOCKING_TYPE, RtAutoChoiceEvent::CAPTURED_CITY, "captured_city", 0},
	{"league_proposal", ENDTURN_BLOCKING_LEAGUE_CALL_FOR_PROPOSALS, RtAutoChoiceEvent::LEAGUE_PROPOSAL, "league", 3},
	{"league_vote", ENDTURN_BLOCKING_LEAGUE_CALL_FOR_VOTES, RtAutoChoiceEvent::LEAGUE_VOTE, "league", 8},
	{"archaeology", ENDTURN_BLOCKING_CHOOSE_ARCHAEOLOGY, RtAutoChoiceEvent::ARCHAEOLOGY, "archaeology", 0},
	{"goody_hut", NO_ENDTURN_BLOCKING_TYPE, RtAutoChoiceEvent::GOODY_HUT, "goody_hut", 0},
	{"steal_tech", ENDTURN_BLOCKING_STEAL_TECH, RtAutoChoiceEvent::STEAL_TECH, "steal_tech", 0}};

struct X
{
	RtSession* s;
	RtAutoChoice* ac;
	std::string stop;
	RtSession::PumpResult r;
	std::vector<RtAutoChoiceEvent> events;
	int kindCount[RtAutoChoiceEvent::STEAL_TECH + 1];
	int turnsLeft;
	int stuck;
	int humanCommands;
	X() : s(NULL), ac(NULL), turnsLeft(0), stuck(0), humanCommands(0) { memset(kindCount, 0, sizeof(kindCount)); }
	CvPlayer& Human() { return GET_PLAYER(s->Engine().GetActivePlayer()); }
};

void Drain(X& x)
{
	RtAutoChoiceEvent ev;
	while (x.ac->PopEvent(ev))
	{
		x.events.push_back(ev);
		++x.kindCount[ev.kind];
		printf("autochoice2-event|%d|%s|%s|%s|%s\n", ev.turn, RtAutoChoice::KindName(ev.kind), ev.type.c_str(), ev.rule.c_str(), ev.text.c_str());
	}
}

// Pump until the human is up (or something else happens); x.r is the result.
bool ToHuman(X& x, int budget = 64)
{
	for (int i = 0; i < budget; ++i)
	{
		x.r = x.s->Pump(RtSession::kMaxSlicesPerTurn);
		if (x.r.status != RtSession::PUMP_AI_RUNNING) break;
	}
	Drain(x);
	if (x.r.status == RtSession::PUMP_STUCK) ++x.stuck;
	if (x.r.status != RtSession::PUMP_WAITING_HUMAN)
	{
		x.stop = Format("expected WAITING_HUMAN, got %s at turn %d: %s", RtSession::PumpStatusName(x.r.status), x.r.turn, x.r.reason.c_str());
		return false;
	}
	return true;
}

// The human's own command for a blocker the scenario plays itself (empty = cannot).
std::string ResolveHuman(X& x, int blocking, const RtSession::BlockTarget& t)
{
	RtEngine& e = x.s->Engine();
	CvPlayer& p = x.Human();
	switch (blocking)
	{
	case ENDTURN_BLOCKING_PRODUCTION:
	{
		CvCity* city = t.kind == RtSession::TARGET_CITY ? p.getCity(t.id) : NULL;
		if (!city) return "";
		for (int i = 0; i < GC.getNumUnitInfos(); ++i)
			if (city->canTrain((UnitTypes)i))
			{
				e.sendPushOrder(city->GetID(), ORDER_TRAIN, i, false, false, false);
				return Format("sendPushOrder(city %d, ORDER_TRAIN, %s)", city->GetID(), GC.getUnitInfo((UnitTypes)i)->GetType());
			}
		return "";
	}
	case ENDTURN_BLOCKING_UNITS:
	case ENDTURN_BLOCKING_STACKED_UNITS:
	{
		CvUnit* u = t.kind == RtSession::TARGET_UNIT ? p.getUnit(t.id) : NULL;
		if (!u) return "";
		MissionTypes m;
		const char* name;
		if (u->canFound(u->plot())) { m = CvTypes::getMISSION_FOUND(); name = "MISSION_FOUND"; }
		else if (u->canFortify(u->plot())) { m = CvTypes::getMISSION_FORTIFY(); name = "MISSION_FORTIFY"; }
		else if (u->canSleep(u->plot())) { m = CvTypes::getMISSION_SLEEP(); name = "MISSION_SLEEP"; }
		else if (u->canHold(u->plot())) { m = CvTypes::getMISSION_SKIP(); name = "MISSION_SKIP"; }
		else
		{
			// STACKED_UNITS (two units that cannot share the plot, a great person next to a combat unit): walk this one to a neighbour plot.
			for (int d = 0; d < NUM_DIRECTION_TYPES; ++d)
			{
				CvPlot* to = plotDirection(u->getX(), u->getY(), (DirectionTypes)d);
				if (to && to->getNumUnits() == 0 && u->canMoveInto(*to))
				{
					e.sendPushMission(u->GetID(), CvTypes::getMISSION_MOVE_TO(), to->getX(), to->getY(), 0, false);
					return Format("sendPushMission(unit %d %s, MISSION_MOVE_TO (%d,%d))", u->GetID(), u->getUnitInfo().GetType(), to->getX(), to->getY());
				}
			}
			return "";
		}
		e.sendPushMission(u->GetID(), m, -1, -1, 0, false);
		return Format("sendPushMission(unit %d %s, %s)", u->GetID(), u->getUnitInfo().GetType(), name);
	}
	default:
		return "";
	}
}

// Play the human's blockers until the blocker is `wanted` (NO_ENDTURN_BLOCKING_TYPE: until none is left). x.r is current on entry.
bool Settle(X& x, int wanted)
{
	std::string last;
	int repeats = 0;
	for (int i = 0; i < 64; ++i)
	{
		if (x.r.blocking == wanted) return true;
		if (x.r.blocking == NO_ENDTURN_BLOCKING_TYPE)
		{
			x.stop = Format("turn %d: no blocker, expected %s", x.r.turn, RtSession::BlockingName(wanted));
			return false;
		}
		const std::string cmd = ResolveHuman(x, x.r.blocking, x.r.target);
		if (cmd.empty())
		{
			x.stop = Format("turn %d: blocker %s is not what the scenario waits for (%s) and it cannot resolve it", x.r.turn, RtSession::BlockingName(x.r.blocking),
				RtSession::BlockingName(wanted));
			return false;
		}
		if (cmd == last && ++repeats >= 3)
		{
			x.stop = Format("turn %d: %s did not go away after the command \"%s\" three times", x.r.turn, RtSession::BlockingName(x.r.blocking), cmd.c_str());
			return false;
		}
		if (cmd != last) { last = cmd; repeats = 0; }
		++x.humanCommands;
		printf("autochoice2-human|%d|%s|%s\n", x.r.turn, RtSession::BlockingName(x.r.blocking), cmd.c_str());
		if (!ToHuman(x)) return false;
	}
	x.stop = "too many commands";
	return false;
}

bool EndTurn(X& x);

// End turns (the scenario plays the other blockers) until the human's blocker is `wanted`; x.r is current on entry.
bool AdvanceTo(X& x, int wanted, int maxTurns)
{
	for (int t = 0; t <= maxTurns; ++t)
	{
		for (int i = 0; i < 64 && x.r.blocking != NO_ENDTURN_BLOCKING_TYPE && x.r.blocking != wanted; ++i)
		{
			const std::string cmd = ResolveHuman(x, x.r.blocking, x.r.target);
			if (cmd.empty())
			{
				x.stop = Format("turn %d: blocker %s is not what the scenario waits for (%s) and it cannot resolve it", x.r.turn, RtSession::BlockingName(x.r.blocking),
					RtSession::BlockingName(wanted));
				return false;
			}
			++x.humanCommands;
			printf("autochoice2-human|%d|%s|%s\n", x.r.turn, RtSession::BlockingName(x.r.blocking), cmd.c_str());
			if (!ToHuman(x)) return false;
		}
		if (x.r.blocking == wanted) return true;
		if (t == maxTurns) break;
		if (!EndTurn(x)) return false;
	}
	x.stop = Format("turn %d: %s did not appear within %d turn(s)", x.r.turn, RtSession::BlockingName(wanted), maxTurns);
	return false;
}

// End the human's turn (the scenario plays the blockers) and pump to the human of the next turn.
bool EndTurn(X& x)
{
	if (x.turnsLeft-- <= 0) { x.stop = "turn budget used up"; return false; }
	const int turn = x.r.turn;
	for (int reopen = 0; reopen < 4; ++reopen)
	{
		while (x.r.blocking != NO_ENDTURN_BLOCKING_TYPE)
			if (!Settle(x, NO_ENDTURN_BLOCKING_TYPE)) return false;
		RtSession::EndTurnResult e = x.s->EndTurn(false);
		if (e.status != RtSession::END_TURN_ACCEPTED)
		{
			x.stop = Format("turn %d: EndTurn without a blocker was %s: %s", turn, RtSession::EndTurnStatusName(e.status), e.reason.c_str());
			return false;
		}
		RtSession::PumpResult n;
		for (int i = 0; i < 64; ++i)
		{
			n = x.s->Pump(RtSession::kMaxSlicesPerTurn, true);
			if (n.status != RtSession::PUMP_AI_RUNNING || n.turnsAdvanced > 0) break;
		}
		Drain(x);
		if (n.status == RtSession::PUMP_WAITING_HUMAN && n.turnsAdvanced == 0 && n.turn == turn)
		{
			x.r = n;  // the gamecore took the end turn back (a unit with moves); play on
			continue;
		}
		if (n.turnsAdvanced != 1 || n.turn != turn + 1)
		{
			if (n.status == RtSession::PUMP_STUCK) ++x.stuck;
			x.stop = Format("turn %d: after the end turn the game did not reach turn %d (%s at turn %d: %s)", turn, turn + 1, RtSession::PumpStatusName(n.status), n.turn,
				n.reason.c_str());
			return false;
		}
		return ToHuman(x);
	}
	x.stop = "the gamecore kept reopening the turn";
	return false;
}

void SetFlag(X& x, Phase ph, int value)
{
	RtAutoChoices f = x.ac->Flags(*x.s);
	switch (ph)
	{
	case PH_PROMOTION: f.promotion = value; break;
	case PH_FREE_GP:
	case PH_FAITH_GP:
	case PH_MAYA_GP: f.great_person = value; break;
	case PH_PANTHEON:
	case PH_RELIGION:
	case PH_ENHANCEMENT:
	case PH_REFORMATION: f.religion = value; break;
	case PH_CAPTURED: f.captured_city = value; break;
	case PH_LEAGUE_PROPOSAL:
	case PH_LEAGUE_VOTE: f.league = value; break;
	case PH_ARCHAEOLOGY: f.archaeology = value; break;
	case PH_GOODY: f.goody_hut = value; break;
	case PH_STEAL: f.steal_tech = value; break;
	default: break;
	}
	x.ac->SetFlags(f);
}

struct PhaseState
{
	int unitsBefore, unitId, levelBefore, cityX, cityY;
	int victim, tech;
	PhaseState() : unitsBefore(0), unitId(-1), levelBefore(0), cityX(-1), cityY(-1), victim(-1), tech(-1) {}
};

CvUnit* SpawnProphet(CvPlayer& p, PhaseState& st)
{
	CvCity* capital = p.getCapitalCity();
	if (!capital) return NULL;
	CvUnit* u = p.initUnit((UnitTypes)GC.getInfoTypeForString("UNIT_PROPHET", true), capital->getX(), capital->getY());
	if (u) st.unitId = u->GetID();
	return u;
}

void GiveTechTree(CvTeam& team, TechTypes tech, PlayerTypes who, bool root, std::vector<char>& seen)
{
	if (tech == NO_TECH || seen[tech]) return;
	seen[tech] = 1;
	const CvTechEntry* e = GC.getTechInfo(tech);
	if (!e) return;
	for (int i = 0; i < GC.getNUM_AND_TECH_PREREQS(); ++i) GiveTechTree(team, (TechTypes)e->GetPrereqAndTechs(i), who, true, seen);
	for (int i = 0; i < GC.getNUM_OR_TECH_PREREQS(); ++i) GiveTechTree(team, (TechTypes)e->GetPrereqOrTechs(i), who, true, seen);
	if (root) team.setHasTech(tech, true, who, false, false);
}

// A land plot next to the capital with no city, unit or improvement that the human may use (not owned by anybody else).
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

// Scenario-only state setup for the phase. Returns an empty string when it worked, else the problem.
std::string Setup(X& x, Phase ph, PhaseState& st, std::string& what)
{
	CvPlayer& p = x.Human();
	RtEngine& e = x.s->Engine();
	st.unitsBefore = p.getNumUnits();
	switch (ph)
	{
	case PH_PROMOTION:
	{
		int loop;
		CvUnit* unit = NULL;
		for (CvUnit* u = p.firstUnit(&loop); u && !unit; u = p.nextUnit(&loop))
			if (u->IsCombatUnit() && !u->isPromotionReady()) unit = u;
		if (!unit) return "no combat unit to promote";
		st.unitId = unit->GetID();
		st.levelBefore = unit->getLevel();
		unit->changeExperience(unit->experienceNeeded() + 5);
		unit->testPromotionReady();  // the gamecore does it after combat; the experience here is granted directly
		what = Format("unit %d %s: experience %d (needs %d), level %d, promotion ready %d", unit->GetID(), unit->getUnitInfo().GetType(), unit->getExperience(),
			unit->experienceNeeded(), unit->getLevel(), unit->isPromotionReady() ? 1 : 0);
		return unit->isPromotionReady() ? "" : "the experience did not make the promotion ready";
	}
	case PH_FREE_GP:
		p.ChangeNumFreeGreatPeople(1);
		what = Format("ChangeNumFreeGreatPeople(1) -> %d", p.GetNumFreeGreatPeople());
		return "";
	case PH_FAITH_GP:
		p.ChangeNumFaithGreatPeople(1);
		what = Format("ChangeNumFaithGreatPeople(1) -> %d", p.GetNumFaithGreatPeople());
		return "";
	case PH_MAYA_GP:
		p.ChangeNumMayaBoosts(1);
		what = Format("ChangeNumMayaBoosts(1) -> %d", p.GetNumMayaBoosts());
		return "";
	case PH_PANTHEON:
	{
		const int need = GC.getGame().GetGameReligions()->GetMinimumFaithNextPantheon();
		p.SetFaith(need);
		what = Format("SetFaith(%d) (pantheon cost); the pantheon notification comes with the next turn", need);
		return "";  // the caller ends the turn
	}
	case PH_RELIGION:
	case PH_ENHANCEMENT:
	{
		CvUnit* u = SpawnProphet(p, st);
		if (!u) return "no Great Prophet could be created";
		e.sendPushMission(u->GetID(), ph == PH_RELIGION ? CvTypes::getMISSION_FOUND_RELIGION() : CvTypes::getMISSION_ENHANCE_RELIGION(), -1, -1, 0, false);
		what = Format("Great Prophet %d on the capital, %s through the loopback", u->GetID(), ph == PH_RELIGION ? "MISSION_FOUND_RELIGION" : "MISSION_ENHANCE_RELIGION");
		return "";
	}
	case PH_REFORMATION:
	{
		for (int i = 0; i < GC.getNumPolicyInfos(); ++i)
		{
			const CvPolicyEntry* pe = GC.getPolicyInfo((PolicyTypes)i);
			if (pe && pe->IsAddReformationBelief())
			{
				p.setHasPolicy((PolicyTypes)i, true);
				what = Format("setHasPolicy(%s); HasPolicyGrantingReformationBelief=%d", pe->GetType(),
					p.GetPlayerPolicies()->HasPolicyGrantingReformationBelief() ? 1 : 0);
				return "";
			}
		}
		return "no policy with a reformation belief";
	}
	case PH_CAPTURED:
	{
		CvCity* capital = p.getCapitalCity();
		if (!capital) return "no capital";
		PlayerTypes aiId = NO_PLAYER;
		for (int i = 0; i < MAX_MAJOR_CIVS && aiId == NO_PLAYER; ++i)
			if ((PlayerTypes)i != p.GetID() && GET_PLAYER((PlayerTypes)i).isAlive() && GET_PLAYER((PlayerTypes)i).isMajorCiv()) aiId = (PlayerTypes)i;
		if (aiId == NO_PLAYER) return "no AI civilization";
		CvPlot* site = NULL;
		for (int radius = 4; radius <= 9 && !site; ++radius)
			for (int dx = -radius; dx <= radius && !site; ++dx)
				for (int dy = -radius; dy <= radius && !site; ++dy)
				{
					CvPlot* pl = GC.getMap().plot(capital->getX() + dx, capital->getY() + dy);
					if (pl && !pl->isWater() && !pl->isImpassable() && !pl->getPlotCity() && pl->getOwner() == NO_PLAYER && p.canFound(pl->getX(), pl->getY())) site = pl;
				}
		if (!site) return "no free plot for the AI city";
		CvCity* ai = GET_PLAYER(aiId).initCity(site->getX(), site->getY());
		if (!ai) return "initCity failed";
		st.cityX = site->getX();
		st.cityY = site->getY();
		p.acquireCity(ai, true, false);  // as a conquest does: for the active human CvPlayer.cpp:3450-3467 flags the city and adds the popup
		what = Format("AI player %d city created at (%d,%d) and acquired by the human with CvPlayer::acquireCity(conquest)", (int)aiId, st.cityX, st.cityY);
		return "";
	}
	case PH_LEAGUE_PROPOSAL:
	{
		CvGameLeagues* leagues = GC.getGame().GetGameLeagues();
		if (!leagues) return "no CvGameLeagues";
		if (!leagues->GetActiveLeague()) leagues->FoundLeague(p.GetID());
		CvLeague* league = leagues->GetActiveLeague();
		if (!league) return "FoundLeague created no league (GAMEOPTION_NO_LEAGUES?)";
		what = Format("FoundLeague(human): league %d, %d members, host %d, human may propose %d (%d proposal(s)); the notification comes with the next turn start",
			(int)league->GetID(), league->GetNumMembers(), (int)league->GetHostMember(), league->CanPropose(p.GetID()) ? 1 : 0, league->GetRemainingProposalsForMember(p.GetID()));
		return league->CanPropose(p.GetID()) ? "" : "the human may not propose";
	}
	case PH_LEAGUE_VOTE:
	{
		CvLeague* league = GC.getGame().GetGameLeagues() ? GC.getGame().GetGameLeagues()->GetActiveLeague() : NULL;
		if (!league) return "no league";
		league->SetTurnsUntilSession(1);
		what = Format("SetTurnsUntilSession(1): the session starts with the next league turn; in session %d, proposals pending %d, human proposals left %d", league->IsInSession() ? 1 : 0,
			league->IsAnythingProposed() ? 1 : 0, league->GetRemainingProposalsForMember(p.GetID()));
		return league->IsAnythingProposed() ? "" : "nothing is proposed, so there is nothing to vote on";
	}
	case PH_ARCHAEOLOGY:
	{
		CvPlot* site = FreePlotNearCapital(p, 1);
		if (!site) return "no free plot next to the capital";
		BuildTypes build = NO_BUILD;
		for (int i = 0; i < GC.getNumBuildInfos() && build == NO_BUILD; ++i)
		{
			const CvBuildInfo* bi = GC.getBuildInfo((BuildTypes)i);
			const CvImprovementEntry* imp = (bi && bi->getImprovement() != NO_IMPROVEMENT) ? GC.getImprovementInfo((ImprovementTypes)bi->getImprovement()) : NULL;
			if (imp && imp->IsPromptWhenComplete()) build = (BuildTypes)i;
		}
		if (build == NO_BUILD) return "no build whose improvement prompts when complete";
		site->AddArchaeologicalRecord(CvTypes::getARTIFACT_ANCIENT_RUIN(), p.GetID(), NO_PLAYER);
		CvUnit* u = p.initUnit((UnitTypes)GC.getInfoTypeForString("UNIT_ARCHAEOLOGIST", true), site->getX(), site->getY());
		if (!u) return "no archaeologist could be created";
		st.unitId = u->GetID();
		st.cityX = site->getX();
		st.cityY = site->getY();
		site->changeBuildProgress(build, 30000, p.GetID());
		what = Format("antiquity site at (%d,%d), archaeologist %d (unit AI %d), %s completed with changeBuildProgress; dig-complete plots: %s, open choices %d", site->getX(), site->getY(), u->GetID(),
			(int)u->AI_getUnitAIType(), GC.getBuildInfo(build)->GetType(), p.GetCulture()->GetNextDigCompletePlot() ? "yes" : "none", p.GetNumArchaeologyChoices());
		return p.GetNumArchaeologyChoices() > 0 ? "" : "the dig did not complete (no archaeology choice open)";
	}
	case PH_GOODY:
	{
		CvPlot* hut = FreePlotNearCapital(p, 2);
		if (!hut) return "no free plot for the ruin";
		const ImprovementTypes imp = (ImprovementTypes)GC.getInfoTypeForString("IMPROVEMENT_GOODY_HUT", true);
		if (imp == NO_IMPROVEMENT) return "no IMPROVEMENT_GOODY_HUT";
		CvUnit* u = p.initUnit((UnitTypes)GC.getInfoTypeForString("UNIT_WARRIOR", true), hut->getX(), hut->getY());
		if (!u) return "no unit could be created";
		st.unitId = u->GetID();
		st.cityX = hut->getX();
		st.cityY = hut->getY();
		u->setHasPromotion((PromotionTypes)GC.getPROMOTION_GOODY_HUT_PICKER(), true);
		hut->setImprovementType(imp);
		if (!hut->isGoody()) return "the plot is not a goody hut";
		p.doGoody(hut, u);  // what CvUnit::... does when the unit steps on the hut: for a unit with the picker promotion it opens the reward popup
		what = Format("unit %d with the goody hut picker promotion popped a hut at (%d,%d); pending ruin rewards %d", u->GetID(), hut->getX(), hut->getY(), x.ac->PendingGoodies(*x.s));
		return x.ac->PendingGoodies(*x.s) == 1 ? "" : "doGoody opened no reward popup";
	}
	case PH_STEAL:
	{
		PlayerTypes victim = NO_PLAYER;
		for (int i = 0; i < MAX_MAJOR_CIVS && victim == NO_PLAYER; ++i)
			if ((PlayerTypes)i != p.GetID() && GET_PLAYER((PlayerTypes)i).isAlive() && GET_PLAYER((PlayerTypes)i).isMajorCiv()) victim = (PlayerTypes)i;
		if (victim == NO_PLAYER) return "no AI civilization";
		const int medieval = GC.getInfoTypeForString("ERA_MEDIEVAL", true);
		TechTypes tech = NO_TECH;
		for (int i = 0; i < GC.getNumTechInfos() && tech == NO_TECH; ++i)
		{
			const CvTechEntry* e2 = GC.getTechInfo((TechTypes)i);
			if (e2 && e2->GetEra() == medieval && !GET_TEAM(p.getTeam()).GetTeamTechs()->HasTech((TechTypes)i) && !GET_TEAM(GET_PLAYER(victim).getTeam()).GetTeamTechs()->HasTech((TechTypes)i))
				tech = (TechTypes)i;
		}
		if (tech == NO_TECH) return "no medieval tech";
		std::vector<char> seenHuman(GC.getNumTechInfos(), 0), seenAi(GC.getNumTechInfos(), 0);
		GiveTechTree(GET_TEAM(GET_PLAYER(victim).getTeam()), tech, victim, true, seenAi);  // the victim knows the tech and everything below
		GiveTechTree(GET_TEAM(p.getTeam()), tech, p.GetID(), false, seenHuman);            // the human knows everything below it
		CvPlayerEspionage* es = p.GetEspionage();
		es->BuildStealableTechList(victim);
		es->m_aiNumTechsToStealList[victim] = 1;
		if (es->m_aaPlayerScienceToStealList[victim].size() == 0) es->m_aaPlayerScienceToStealList[victim].push_back(50);
		p.GetNotifications()->Add(NOTIFICATION_SPY_STOLE_TECH, "scenario: a spy stole a technology", "scenario spy", -1, -1, (int)victim);
		st.victim = (int)victim;
		st.tech = (int)tech;
		what = Format("%s known to AI player %d (and its prerequisites to both), a tech to steal (1), stealable list %d, human can research it %d, SPY_STOLE_TECH notification",
			GC.getTechInfo(tech)->GetType(), (int)victim, (int)es->m_aaPlayerStealableTechList[victim].size(), p.GetPlayerTechs()->CanResearch(tech) ? 1 : 0);
		return (es->IsTechStealable(victim, tech) && p.GetPlayerTechs()->CanResearch(tech)) ? "" : "the tech is not stealable";
	}
	default:
		return "?";
	}
}

CvCity* CapturedCity(X& x, const PhaseState& st)
{
	CvPlot* pl = GC.getMap().plot(st.cityX, st.cityY);
	CvCity* c = pl ? pl->getPlotCity() : NULL;
	return (c && c->getOwner() == x.Human().GetID()) ? c : NULL;
}

// The state the choice must have produced. Returns "" when it shows, else what is missing.
std::string Verify(X& x, Phase ph, const PhaseState& st)
{
	CvPlayer& p = x.Human();
	switch (ph)
	{
	case PH_PROMOTION:
	{
		CvUnit* u = p.getUnit(st.unitId);
		if (!u) return "the unit is gone";
		return (u->getLevel() > st.levelBefore && !u->isPromotionReady()) ? "" : Format("level %d (was %d), promotion ready %d", u->getLevel(), st.levelBefore, u->isPromotionReady() ? 1 : 0);
	}
	case PH_FREE_GP:
		return (p.GetNumFreeGreatPeople() == 0 && p.getNumUnits() == st.unitsBefore + 1) ? "" : Format("free great people %d, units %d (was %d)", p.GetNumFreeGreatPeople(), p.getNumUnits(), st.unitsBefore);
	case PH_FAITH_GP:
		return (p.GetNumFaithGreatPeople() == 0 && p.getNumUnits() == st.unitsBefore + 1) ? "" : Format("faith great people %d, units %d (was %d)", p.GetNumFaithGreatPeople(), p.getNumUnits(), st.unitsBefore);
	case PH_MAYA_GP:
		return (p.GetNumMayaBoosts() == 0 && p.getNumUnits() == st.unitsBefore + 1) ? "" : Format("Maya boosts %d, units %d (was %d)", p.GetNumMayaBoosts(), p.getNumUnits(), st.unitsBefore);
	case PH_PANTHEON:
		return p.GetReligions()->HasCreatedPantheon() ? "" : "no pantheon";
	case PH_RELIGION:
		return (p.GetReligions()->HasCreatedReligion() && !p.getUnit(st.unitId)) ? "" : "no religion, or the prophet is still there";
	case PH_ENHANCEMENT:
	{
		const ReligionTypes r = GC.getGame().GetGameReligions()->GetReligionCreatedByPlayer(p.GetID());
		const CvReligion* rel = r == NO_RELIGION ? NULL : GC.getGame().GetGameReligions()->GetReligion(r, p.GetID());
		return (rel && rel->m_bEnhanced && !p.getUnit(st.unitId)) ? "" : "not enhanced, or the prophet is still there";
	}
	case PH_REFORMATION:
		return GC.getGame().GetGameReligions()->HasAddedReformationBelief(p.GetID()) ? "" : "no reformation belief";
	case PH_CAPTURED:
	{
		CvCity* c = CapturedCity(x, st);
		if (!c) return "the city is not ours (any more)";
		return (c->IsPuppet() && !c->IsIgnoreCityForHappiness()) ? "" : Format("puppet %d, flag %d", c->IsPuppet() ? 1 : 0, c->IsIgnoreCityForHappiness() ? 1 : 0);
	}
	case PH_LEAGUE_PROPOSAL:
	{
		CvLeague* league = GC.getGame().GetGameLeagues() ? GC.getGame().GetGameLeagues()->GetActiveLeague() : NULL;
		if (!league) return "no league";
		return (league->GetNumProposalsByMember(p.GetID()) > 0 && !league->CanPropose(p.GetID())) ? "" :
			Format("proposals by the human %d, may still propose %d", league->GetNumProposalsByMember(p.GetID()), league->CanPropose(p.GetID()) ? 1 : 0);
	}
	case PH_LEAGUE_VOTE:
	{
		CvLeague* league = GC.getGame().GetGameLeagues() ? GC.getGame().GetGameLeagues()->GetActiveLeague() : NULL;
		if (!league) return "no league";
		return !league->CanVote(p.GetID()) ? "" : Format("%d delegate(s) left", league->GetRemainingVotesForMember(p.GetID()));
	}
	case PH_ARCHAEOLOGY:
		return (p.GetNumArchaeologyChoices() == 0 && !p.getUnit(st.unitId)) ? "" : Format("open choices %d, archaeologist %s", p.GetNumArchaeologyChoices(), p.getUnit(st.unitId) ? "still there" : "gone");
	case PH_GOODY:
		return x.ac->PendingGoodies(*x.s) == 0 ? "" : Format("%d pending ruin reward(s)", x.ac->PendingGoodies(*x.s));
	case PH_STEAL:
	{
		CvPlayerEspionage* es = p.GetEspionage();
		CvTeamTechs* tt = GET_TEAM(p.getTeam()).GetTeamTechs();
		const bool progressed = tt->HasTech((TechTypes)st.tech) || tt->GetResearchProgress((TechTypes)st.tech) > 0;
		return (es->m_aiNumTechsToStealList[st.victim] == 0 && progressed) ? "" : Format("techs to steal %d, progress on the tech %d", es->m_aiNumTechsToStealList[st.victim], tt->GetResearchProgress((TechTypes)st.tech));
	}
	default:
		return "?";
	}
}

}  // namespace

extern "C" int rt_selftest_autochoice2(const gc_host* host, unsigned seed, unsigned turns, unsigned ai, char* report, size_t report_len)
{
	SetLogSink(ForwardLog, const_cast<gc_host*>(host));
	Check c;
	X x;
	x.turnsLeft = (int)turns;
	RtSession* session = new RtSession();  // left alive on purpose, as in scenario `load`
	RtSession::LoadStatus ls;
	const bool loaded = session->Load(*host, ls);
	c.add("load", loaded, "boot reached step %s%s%s", RtSession::StepName(ls.step), ls.error.empty() ? "" : ": ", ls.error.c_str());

	int phasesDone = 0, luaErrors = 0;
	std::string phaseJson;
	if (loaded)
	{
		RtSession::NewGameStatus st;
		const std::string setup = Format("{\"game_type\":\"single\",\"seed\":%u,\"ai_count\":%u,\"auto_choices\":{\"religion\":0,\"promotion\":0,\"great_person\":0,\"captured_city\":0,\"league\":0,\"archaeology\":0,\"goody_hut\":0,\"steal_tech\":0}}", seed, ai);
		const bool started = session->NewGame(*host, setup, st);
		c.add("new_game", started, "%s reached step %s%s%s", setup.c_str(), RtSession::NewGameStepName(st.step), st.error.empty() ? "" : ": ", st.error.c_str());
		RtScriptSystem* ss = session->Scripts();
		x.s = session;
		x.ac = &session->AutoChoice();
		const RtAutoChoices& f0 = x.ac->Flags(*session);
		c.add("flags_from_setup", f0.religion == 0 && f0.promotion == 0 && f0.great_person == 0 && f0.captured_city == 0 && f0.league == 0 && f0.archaeology == 0 && f0.goody_hut == 0 && f0.steal_tech == 0 && f0.research == 1,
			"auto_choices from the setup JSON: religion=%d promotion=%d great_person=%d captured_city=%d league=%d archaeology=%d goody_hut=%d steal_tech=%d research=%d", f0.religion,
			f0.promotion, f0.great_person, f0.captured_city, f0.league, f0.archaeology, f0.goody_hut, f0.steal_tech, f0.research);
		c.add("no_human_input_default", !session->NoHumanInput() && session->Setup().autoplay_human == 0, "NoHumanInput() follows the setup's autoplay_human (%d): %d", session->Setup().autoplay_human,
			session->NoHumanInput() ? 1 : 0);
		if (started && ss)
		{
			const int luaBefore = ss->GetErrorCount();
			// Bootstrap: a city (the scenario plays the settler, the production).
			bool ok = ToHuman(x);
			for (int i = 0; ok && !x.Human().getCapitalCity() && i < 8; ++i)
			{
				ok = Settle(x, NO_ENDTURN_BLOCKING_TYPE);
				if (ok && !x.Human().getCapitalCity()) ok = EndTurn(x);
			}
			if (ok && x.r.blocking != NO_ENDTURN_BLOCKING_TYPE) ok = Settle(x, NO_ENDTURN_BLOCKING_TYPE);
			c.add("city", ok && x.Human().getCapitalCity() != NULL, "the human has a capital at turn %d%s%s", x.r.turn, x.stop.empty() ? "" : ": ", x.stop.c_str());

			for (int pi = 0; ok && pi < PH_COUNT; ++pi)
			{
				const Phase ph = (Phase)pi;
				const PhaseInfo& info = kPhases[pi];
				PhaseState ps;
				std::string what;
				const int turnStart = x.r.turn;
				const int before = x.kindCount[info.kind];
				SetFlag(x, ph, 0);  // earlier phases left their flag on; this kind starts with its own flag off
				const std::string problem = Setup(x, ph, ps, what);
				if (!problem.empty())
				{
					c.add(info.name, false, "setup: %s (%s)", problem.c_str(), what.c_str());
					ok = false;
					break;
				}
				printf("autochoice2-phase|%d|%s|setup|%s\n", x.r.turn, info.name, what.c_str());
				// The pantheon notification comes with the next turn start (CvGameReligions::DoPlayerTurn); the reformation one with setHasPolicy.
				if (ph == PH_PANTHEON)
				{
					if (!ToHuman(x) || !EndTurn(x)) { c.add(info.name, false, "end turn after the setup: %s", x.stop.c_str()); ok = false; break; }
				}
				else if (!ToHuman(x)) { c.add(info.name, false, "pump after the setup: %s", x.stop.c_str()); ok = false; break; }
				// Flag OFF.
				bool offBlocks = false, rejected = false;
				std::string offDetail;
				if (info.blocker != NO_ENDTURN_BLOCKING_TYPE)
				{
					// other blockers of the turn (production, units) are the human's; the wanted one must be reached and stay
					if (!(info.advance > 0 ? AdvanceTo(x, info.blocker, info.advance) : Settle(x, info.blocker))) { c.add(info.name, false, "flag off: %s", x.stop.c_str()); ok = false; break; }
					offBlocks = x.r.status == RtSession::PUMP_WAITING_HUMAN && x.r.blocking == info.blocker;
					RtSession::EndTurnResult er = session->EndTurn(false);
					rejected = er.status == RtSession::END_TURN_REJECTED_BLOCKED && er.blocking == info.blocker;
					offDetail = Format("WAITING_HUMAN on %s, EndTurn -> %s (%s)", RtSession::BlockingName(x.r.blocking), RtSession::EndTurnStatusName(er.status),
						RtSession::BlockingName(er.blocking));
					// still there after more pumping
					if (!ToHuman(x)) { c.add(info.name, false, "flag off: %s", x.stop.c_str()); ok = false; break; }
					offBlocks = offBlocks && x.r.blocking == info.blocker;
				}
				else if (ph == PH_GOODY)
				{
					offBlocks = x.ac->PendingGoodies(*x.s) == 1;
					rejected = true;
					offDetail = Format("%d ruin reward popup(s) pending, nothing decided", x.ac->PendingGoodies(*x.s));
				}
				else
				{
					CvCity* city = CapturedCity(x, ps);
					offBlocks = city && city->IsIgnoreCityForHappiness() && !city->IsPuppet();
					rejected = true;
					offDetail = Format("city %s, IsIgnoreCityForHappiness=%d IsPuppet=%d (nothing decided)", city ? "ours" : "missing", city && city->IsIgnoreCityForHappiness() ? 1 : 0,
						city && city->IsPuppet() ? 1 : 0);
				}
				if (info.blocker != NO_ENDTURN_BLOCKING_TYPE && offBlocks)
				{
					// STUCK semantics: with nobody to answer the blocker the same pump is STUCK, naming the blocker and the flag that resolves it.
					session->SetNoHumanInput(true);
					const RtSession::PumpResult pr = session->Pump(RtSession::kMaxSlicesPerTurn);
					session->SetNoHumanInput(false);
					const bool named = pr.reason.find(RtSession::BlockingName(info.blocker)) != std::string::npos && pr.reason.find(std::string("auto_choices.") + info.flag) != std::string::npos;
					c.add((std::string(info.name) + "_off_stuck").c_str(), pr.status == RtSession::PUMP_STUCK && named, "no human input: %s; %s", RtSession::PumpStatusName(pr.status), pr.reason.c_str());
					if (!ToHuman(x)) { c.add(info.name, false, "after the STUCK check: %s", x.stop.c_str()); ok = false; break; }
				}
				const bool noEventOff = x.kindCount[info.kind] == before;
				c.add((std::string(info.name) + "_off_blocks").c_str(), offBlocks && rejected && noEventOff, "flag off: %s; %d %s event(s) while off", offDetail.c_str(),
					x.kindCount[info.kind] - before, RtAutoChoice::KindName(info.kind));
				// Flag ON.
				SetFlag(x, ph, 1);
				if (!ToHuman(x)) { c.add(info.name, false, "flag on: %s", x.stop.c_str()); ok = false; break; }
				const bool cleared = info.blocker == NO_ENDTURN_BLOCKING_TYPE || x.r.blocking != info.blocker;
				const std::string state = Verify(x, ph, ps);
				const int made = x.kindCount[info.kind] - before;
				std::string text = "-";
				for (size_t i = 0; i < x.events.size(); ++i)
					if (x.events[i].kind == info.kind) text = x.events[i].text;
				c.add((std::string(info.name) + "_on_chooses").c_str(), cleared && state.empty() && made == 1,
					"flag on: blocker now %s, %d %s event(s) (%s)%s%s", RtSession::BlockingName(x.r.blocking), made, RtAutoChoice::KindName(info.kind), text.c_str(),
					state.empty() ? "" : "; state: ", state.c_str());
				printf("autochoice2-phase|%d|%s|done|turn %d -> %d\n", x.r.turn, info.name, turnStart, x.r.turn);
				if (cleared && state.empty() && made == 1) ++phasesDone;
				{
					char tmp[256];
					snprintf(tmp, sizeof(tmp), "{\"phase\":\"%s\",\"off_blocks\":%s,\"on_chooses\":%s}", info.name, (offBlocks && rejected && noEventOff) ? "true" : "false",
						(cleared && state.empty() && made == 1) ? "true" : "false");
					phaseJson += std::string(phaseJson.empty() ? "" : ",") + tmp;
				}
			}
			if (ok && phasesDone == PH_COUNT)
			{
				// The religion is founded and enhanced: a free great person must not be a Great Prophet any more (flag great_person is on).
				const int before = x.kindCount[RtAutoChoiceEvent::FREE_GREAT_PERSON];
				x.Human().ChangeNumFreeGreatPeople(1);
				ToHuman(x);
				std::string type = "-";
				for (size_t i = 0; i < x.events.size(); ++i)
					if (x.events[i].kind == RtAutoChoiceEvent::FREE_GREAT_PERSON) type = x.events[i].type;
				c.add("gp_rule_no_prophet", x.kindCount[RtAutoChoiceEvent::FREE_GREAT_PERSON] == before + 1 && type != "UNIT_PROPHET",
					"religion founded and enhanced: the next free great person is %s, not UNIT_PROPHET", type.c_str());
			}
			if (ok && phasesDone == PH_COUNT)
			{
				// Isolation (AC-0.3): research auto-choice off -> the turn is held. With a human it is WAITING_HUMAN, with nobody to answer it STUCK.
				RtAutoChoices f = x.ac->Flags(*session);
				f.research = 0;
				x.ac->SetFlags(f);
				x.Human().chooseTech(1);  // a free technology: the FREE_TECH blocker
				bool good = ToHuman(x);
				const int blocker = x.r.blocking;
				const bool waits = good && blocker == ENDTURN_BLOCKING_FREE_TECH;
				session->SetNoHumanInput(true);
				const RtSession::PumpResult pr = session->Pump(RtSession::kMaxSlicesPerTurn);
				const bool stuckNamed = pr.status == RtSession::PUMP_STUCK && pr.reason.find("ENDTURN_BLOCKING_FREE_TECH") != std::string::npos && pr.reason.find("auto_choices.research") != std::string::npos;
				session->SetNoHumanInput(false);
				c.add("isolation_research_off", waits && stuckNamed, "research flag off, free technology: with a human %s on %s; without human input %s: %s",
					RtSession::PumpStatusName(x.r.status), RtSession::BlockingName(blocker), RtSession::PumpStatusName(pr.status), pr.reason.c_str());
				printf("autochoice2-stuck|%s\n", pr.reason.c_str());
				f.research = 1;
				x.ac->SetFlags(f);
				const bool back = ToHuman(x);
				c.add("isolation_research_on", back && x.r.blocking != ENDTURN_BLOCKING_FREE_TECH && x.Human().GetNumFreeTechs() == 0, "research flag on again: blocker %s, free techs %d",
					RtSession::BlockingName(x.r.blocking), x.Human().GetNumFreeTechs());
			}
			luaErrors = ss->GetErrorCount() - luaBefore;
		}
	}

	c.add("stopped", x.stop.empty(), "%s", x.stop.empty() ? "no stop reason" : x.stop.c_str());
	c.add("phases", phasesDone == PH_COUNT, "%d/%d kinds chose automatically (flag off: blocker stays; flag on: chosen with an AUTO_CHOICE event)", phasesDone, (int)PH_COUNT);
	c.add("no_stuck", x.stuck == 0, "%d stuck pump(s)", x.stuck);
	c.add("lua_errors", loaded && luaErrors == 0, "%d Lua error(s)", luaErrors);

	const std::string out = "{\"failed\":" + Format("%d", c.failed) + ",\"seed\":" + Format("%u", seed) + ",\"ai_count\":" + Format("%u", ai) + ",\"phases_done\":" +
		Format("%d", phasesDone) + ",\"human_commands\":" + Format("%d", x.humanCommands) + ",\"stop_reason\":\"" + Escape(x.stop) + "\",\"phases\":[" + phaseJson +
		"],\"checks\":[" + c.json + "]}";
	if (report && report_len)
	{
		strncpy(report, out.c_str(), report_len - 1);
		report[report_len - 1] = '\0';
	}
	printf("autochoice2-summary|seed %u|ai %u|phases %d/%d|human commands %d\n", seed, ai, phasesDone, (int)PH_COUNT, x.humanCommands);
	SetLogSink(NULL, NULL);
	return c.failed == 0 ? 0 : 1;
}
