// gc_apply_command: validates a human command and sends it through the loopback as the human
// (RtEngine::send*, delivered by the next RtSession::Pump to the net message handler, like the UI's own commands). C++03.
//
//  kind | name                  | validation                                          | loopback
//   1   UNIT_MOVE_TO            target on the map, not the unit's plot, immobile?,    sendPushMission(MISSION_MOVE_TO, x, y)
//                               interface pathfinder finds a path
//   2   UNIT_RANGE_ATTACK       target on the map, CanStartMission(RANGE_ATTACK)      sendPushMission(MISSION_RANGE_ATTACK, x, y)
//   3   UNIT_FOUND_CITY         canFound(plot) via CanStartMission(FOUND)             sendPushMission(MISSION_FOUND)
//   4   UNIT_FORTIFY            CanStartMission(FORTIFY)                              sendPushMission(MISSION_FORTIFY)
//   5   UNIT_SLEEP              CanStartMission(SLEEP)                                sendPushMission(MISSION_SLEEP)
//   6   UNIT_SKIP               CanStartMission(SKIP)                                 sendPushMission(MISSION_SKIP)
//   7   UNIT_HEAL               CanStartMission(HEAL)                                 sendPushMission(MISSION_HEAL)
//   8   UNIT_AUTOMATE_BUILD     canDoCommand(COMMAND_AUTOMATE, AUTOMATE_BUILD)        sendDoCommand(COMMAND_AUTOMATE, AUTOMATE_BUILD)
//   9   UNIT_AUTOMATE_EXPLORE   canDoCommand(COMMAND_AUTOMATE, AUTOMATE_EXPLORE)      sendDoCommand(COMMAND_AUTOMATE, AUTOMATE_EXPLORE)
//  10   UNIT_CANCEL             canDoCommand(COMMAND_CANCEL_ALL), or STOP_AUTOMATION  sendDoCommand(COMMAND_CANCEL_ALL / _STOP_AUTOMATION)
//                               for an automated unit
//  11   UNIT_DISBAND            canDoCommand(COMMAND_DELETE) = canScrap               sendDoCommand(COMMAND_DELETE) (the UI's disband;
//                                                                                    gDLL has no ResponseDestroyUnit sender)
//  12   UNIT_WAKE               canDoCommand(COMMAND_WAKE)                            sendDoCommand(COMMAND_WAKE)
//  20   CITY_SET_PRODUCTION      city exists, not a puppet, canTrain / canConstruct /  sendPushOrder(city, order, item, alt 0, pop 1, append 1)
//                               canCreate / canMaintain (with the core's refusal    = the production popup's click: the queue is replaced
//                               text), not already the current production
//  21   CITY_PURCHASE           IsCanPurchase(gold or faith): names the reason      sendPurchase(city, unit|building|project, yield)
//                               (not enough gold, puppet, resistance, ...)
//  22   CITY_RANGE_ATTACK       target on the map, canRangeStrike, visible, in       sendDoTask(city, TASK_RANGED_ATTACK, x, y)
//                               range, canRangeStrikeAt
//  30   DECLARE_WAR             team exists and is alive, not ours, met, not at war, sendChangeWar(team, true)
//                               no forced peace / pact, canDeclareWar
//  40   END_TURN                RtSession::EndTurn(false)                             CONTROL_ENDTURN
//  41   FORCE_END_TURN          RtSession::EndTurn(true)                              CONTROL_FORCEENDTURN (UNITS blocker only)
//  42   CANCEL_END_TURN         network type; the turn ended and not every human has    sendTurnUnready (NetTurnUnready), core end-turn flag off
//  43   END_TURN_TIMEOUT        network type; HumanMayAct only, no blocker checks       sendTurnComplete (RW-23)
// Every command first needs (HumanMayAct, v2): cmd.player = a seated human (any of them; the command runs as that human, ActiveAs), the
// AI civs and unit AI done, the human's turn active and not ended. A refusal carries a GC_REASON_* (gc_error.reason). A unit command also:
// the unit exists for that player, not dying and not busy. A city command: the city exists for that player.
#include "CvGameCoreDLLPCH.h"

#include "CvEconomicAI.h"
#include "CvGameCoreUtils.h"
#include "CvTypes.h"
#include "gc_command.h"
#include "gc_replica.h"
#include "../runtime/rt_active.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace lekmod_portable {

namespace {

std::string Fmt(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string Fmt(const char* fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return buf;
}

bool OnMap(int x, int y)
{
	CvMap& map = GC.getMap();
	return x >= 0 && y >= 0 && x < map.getGridWidth() && y < map.getGridHeight();
}

const char* UnitTypeName(const CvUnit* u)
{
	const char* t = u->getUnitInfo().GetType();
	return t ? t : "?";
}

MissionTypes MissionOf(int kind)
{
	switch (kind)
	{
	case GC_CMD_UNIT_MOVE_TO: return CvTypes::getMISSION_MOVE_TO();
	case GC_CMD_UNIT_RANGE_ATTACK: return CvTypes::getMISSION_RANGE_ATTACK();
	case GC_CMD_UNIT_FOUND_CITY: return CvTypes::getMISSION_FOUND();
	case GC_CMD_UNIT_FORTIFY: return CvTypes::getMISSION_FORTIFY();
	case GC_CMD_UNIT_SLEEP: return CvTypes::getMISSION_SLEEP();
	case GC_CMD_UNIT_SKIP: return CvTypes::getMISSION_SKIP();
	case GC_CMD_UNIT_HEAL: return CvTypes::getMISSION_HEAL();
	}
	return (MissionTypes)-1;
}

// HumanMayAct(P): P is a seated human, the AI has moved, P's turn is active and P has not ended it: the state in which the
// UI would let P order a unit. The caller has made P the active player (ActiveAs). reason gets the GC_REASON_* of a refusal.
// `needOpenTurn` false (CANCEL_END_TURN): P may have ended the turn.
bool HumanMayAct(RtSession& s, int player, CvPlayer** out, std::string& msg, int32_t* reason, bool needOpenTurn = true)
{
	RtEngine& e = s.Engine();
	*reason = GC_REASON_NOT_HUMAN;
	if (!IsSeatedHuman(player))
	{
		msg = Fmt("player %d is not the human player of this game", player);
		return false;
	}
	CvPlayer& p = GET_PLAYER((PlayerTypes)player);
	*reason = GC_REASON_AI_RUNNING;
	if (!e.allAICivsProcessedThisTurn() || !GC.getGame().allUnitAIProcessed())
	{
		msg = "it is not the human's turn yet: the AI civilizations are still moving";
		return false;
	}
	if (needOpenTurn && e.TurnCompleteSent((PlayerTypes)player))
	{
		msg = "the turn was already ended";
		*reason = GC_REASON_TURN_ENDED;
		return false;
	}
	if (!p.isTurnActive())
	{
		msg = Fmt("it is not player %d's turn now", player);
		*reason = GC_REASON_NOT_YOUR_TURN;
		return false;
	}
	*reason = GC_REASON_NONE;
	*out = &p;
	return true;
}

}  // namespace

bool UnitCommandApplies(CvUnit* u, int kind)
{
	switch (kind)
	{
	case GC_CMD_UNIT_MOVE_TO: return !u->IsImmobile();
	case GC_CMD_UNIT_RANGE_ATTACK: return u->IsCanAttackRanged();
	case GC_CMD_UNIT_FOUND_CITY: return u->canFound(u->plot(), true);
	case GC_CMD_UNIT_FORTIFY: return u->isFortifyable(true);
	case GC_CMD_UNIT_SLEEP: return !u->isFortifyable(true);  // a unit that can fortify fortifies instead (CvUnit::canSleep)
	case GC_CMD_UNIT_SKIP:
	case GC_CMD_UNIT_CANCEL:
	case GC_CMD_UNIT_WAKE: return true;
	case GC_CMD_UNIT_HEAL: return u->IsCombatUnit() || u->IsHurt();
	case GC_CMD_UNIT_AUTOMATE_BUILD: return u->CanAutomate(AUTOMATE_BUILD, true);
	case GC_CMD_UNIT_AUTOMATE_EXPLORE: return u->CanAutomate(AUTOMATE_EXPLORE, true);
	case GC_CMD_UNIT_DISBAND: return u->canScrap(true);
	}
	return false;
}

bool FindUnitPath(CvUnit* u, int x, int y, std::vector<PathStep>& steps)
{
	steps.clear();
	CvTwoLayerPathFinder& finder = GC.getInterfacePathFinder();
	finder.SetData(u);
	if (!finder.GeneratePath(u->getX(), u->getY(), x, y, MOVE_DECLARE_WAR, false)) return false;
	finder.SetData(u);
	for (CvAStarNode* n = finder.GetLastNode(); n != NULL; n = n->m_pParent)
	{
		PathStep s;
		s.x = n->m_iX;
		s.y = n->m_iY;
		s.turn = n->m_iData2 > 0 ? n->m_iData2 - 1 : 0;  // the pathfinder counts turns from 1 (the current one)
		s.moves = n->m_iData1;
		steps.push_back(s);
	}
	for (size_t i = 0, j = steps.size(); i + 1 < j; ++i, --j) std::swap(steps[i], steps[j - 1]);
	return !steps.empty();
}

bool CheckUnitCommand(CvUnit* u, int kind, bool haveTarget, int x, int y, std::string& msg, int32_t* reason)
{
	// v2: the refusal's GC_REASON_*; RULE unless a branch below says otherwise.
	int32_t scratch = 0;
	int32_t& why_ = reason ? *reason : scratch;
	why_ = GC_REASON_RULE;
	if (u->isDelayedDeath())
	{
		msg = "the unit is dying";
		why_ = GC_REASON_UNIT_BUSY;
		return false;
	}
	if (u->IsBusy())
	{
		msg = "the unit is busy (moving or fighting)";
		why_ = GC_REASON_UNIT_BUSY;
		return false;
	}
	CvPlot* here = u->plot();
	switch (kind)
	{
	case GC_CMD_UNIT_MOVE_TO:
	{
		if (u->IsImmobile() && u->getDomainType() != DOMAIN_AIR)
		{
			msg = "the unit cannot move";
			return false;
		}
		if (!haveTarget) return true;
		why_ = GC_REASON_BAD_TARGET;
		if (!OnMap(x, y))
		{
			msg = Fmt("target (%d,%d) is outside the map", x, y);
			return false;
		}
		if (u->getX() == x && u->getY() == y)
		{
			msg = "the unit is already on the target plot";
			return false;
		}
		if (!u->CanStartMission(CvTypes::getMISSION_MOVE_TO(), x, y, NULL, false))
		{
			msg = Fmt("the unit cannot start moving to (%d,%d)", x, y);
			return false;
		}
		std::vector<PathStep> path;
		if (!FindUnitPath(u, x, y, path))
		{
			msg = Fmt("no path for the unit to (%d,%d)", x, y);
			return false;
		}
		why_ = GC_REASON_NONE;
		return true;
	}
	case GC_CMD_UNIT_RANGE_ATTACK:
	{
		if (!u->canRangeStrike())
		{
			msg = "the unit cannot make a ranged attack now (no ranged strength or range, or embarked)";
			return false;
		}
		if (!haveTarget) return true;
		why_ = GC_REASON_BAD_TARGET;
		if (!OnMap(x, y))
		{
			msg = Fmt("target (%d,%d) is outside the map", x, y);
			return false;
		}
		if (!u->CanStartMission(CvTypes::getMISSION_RANGE_ATTACK(), x, y, NULL, false))
		{
			msg = Fmt("no ranged attack target at (%d,%d) (out of range, not visible, or no enemy there)", x, y);
			return false;
		}
		why_ = GC_REASON_NONE;
		return true;
	}
	case GC_CMD_UNIT_FOUND_CITY:
		if (!u->canFound(here, true))
		{
			msg = Fmt("%s cannot found a city", UnitTypeName(u));
			return false;
		}
		if (!u->CanStartMission(CvTypes::getMISSION_FOUND(), -1, -1, NULL, false))
		{
			msg = "a city cannot be founded on this plot (too close to another city, foreign land or unsuitable terrain)";
			return false;
		}
		return true;
	case GC_CMD_UNIT_FORTIFY:
	case GC_CMD_UNIT_SLEEP:
	case GC_CMD_UNIT_SKIP:
	case GC_CMD_UNIT_HEAL:
	{
		const MissionTypes m = MissionOf(kind);
		if (u->CanStartMission(m, -1, -1, NULL, false)) return true;
		switch (kind)
		{
		case GC_CMD_UNIT_FORTIFY: msg = "the unit cannot fortify here (civilian, embarked, already fortified or not fortifiable)"; break;
		case GC_CMD_UNIT_SLEEP: msg = "the unit cannot sleep here (units that can fortify fortify instead)"; break;
		case GC_CMD_UNIT_SKIP: msg = "the unit cannot skip its turn here (plot is over the stacking limit)"; break;
		default: msg = "the unit cannot heal here (full health or not allowed on this plot)"; break;
		}
		return false;
	}
	case GC_CMD_UNIT_AUTOMATE_BUILD:
		if (!u->canDoCommand(COMMAND_AUTOMATE, AUTOMATE_BUILD, -1, false, true))
		{
			msg = "the unit cannot automate building (not a worker, or worker automation is off)";
			return false;
		}
		return true;
	case GC_CMD_UNIT_AUTOMATE_EXPLORE:
	{
		// The "nothing left to explore" test (CvHomelandAI::IsAnyValidExploreMoves) reads the owner's exploration plots through CvEconomicAI::GetExplorationPlots, which rebuilds
		// them when the dirty flag is set (UpdatePlots) and stores them in the serialized economic AI: a check that writes (found by the purity test, AC-N1.4; class C5 like
		// audit F-13). So the test is made only while the cache is clean; a dirty one is not refreshed by a check (the unit is offered the command, and the core's own
		// CanAutomate runs the test when the command is carried out).
		CvEconomicAI* economic = GET_PLAYER(u->getOwner()).GetEconomicAI();
		const bool cacheDirty = economic && economic->m_bExplorationPlotsDirty;
		if (!u->canDoCommand(COMMAND_AUTOMATE, AUTOMATE_EXPLORE, -1, cacheDirty, true))
		{
			msg = "the unit cannot explore automatically (no combat strength, air or immobile, or nothing left to explore)";
			return false;
		}
		return true;
	}
	case GC_CMD_UNIT_CANCEL:
		if (u->IsAutomated())
		{
			if (u->canDoCommand(COMMAND_STOP_AUTOMATION, -1, -1, false, true)) return true;
		}
		else if (u->canDoCommand(COMMAND_CANCEL_ALL, -1, -1, false, true))
			return true;
		msg = "the unit has no orders to cancel";
		return false;
	case GC_CMD_UNIT_DISBAND:
		if (!u->canDoCommand(COMMAND_DELETE, -1, -1, false, true))
		{
			msg = "the unit cannot be disbanded now (fighting, no moves left, or disbanding is disabled)";
			return false;
		}
		return true;
	case GC_CMD_UNIT_WAKE:
		if (!u->canDoCommand(COMMAND_WAKE, -1, -1, false, true))
		{
			msg = "the unit is not sleeping, fortified or waiting";
			return false;
		}
		return true;
	}
	msg = "not a unit command";
	return false;
}

// ---- G1a: city commands and DECLARE_WAR ----

bool CityHeadOrder(const CvCity* city, int* order, int* item)
{
	const OrderData* head = city->headOrderQueueNode();
	if (!head) return false;
	*order = (int)head->eOrderType;
	*item = head->iData1;
	return true;
}

std::string PlainReason(const char* text)
{
	std::string out;
	if (!text) return out;
	for (const char* p = text; *p;)
	{
		if (*p == '[')
		{
			const char* end = strchr(p, ']');
			if (end)
			{
				if (strncmp(p, "[NEWLINE]", 9) == 0 && !out.empty() && out[out.size() - 1] != ' ') out += "; ";
				p = end + 1;
				continue;
			}
		}
		out += *p++;
	}
	size_t a = 0, b = out.size();
	while (a < b && (out[a] == ' ' || out[a] == ';' || out[a] == '\n')) ++a;
	while (b > a && (out[b - 1] == ' ' || out[b - 1] == ';' || out[b - 1] == '\n' || out[b - 1] == '\r')) --b;
	return out.substr(a, b - a);
}

namespace {

bool OrderItemExists(int order, int item)
{
	if (item < 0) return false;
	switch (order)
	{
	case GC_ORDER_TRAIN: return item < (int)GC.getNumUnitInfos();
	case GC_ORDER_CONSTRUCT: return item < (int)GC.getNumBuildingInfos();
	case GC_ORDER_CREATE: return item < (int)GC.getNumProjectInfos();
	case GC_ORDER_MAINTAIN: return item < (int)GC.getNumProcessInfos();
	}
	return false;
}

const char* OrderItemType(int order, int item)
{
	const char* t = NULL;
	switch (order)
	{
	case GC_ORDER_TRAIN: { CvUnitEntry* e = GC.getUnitInfo((UnitTypes)item); t = e ? e->GetType() : NULL; break; }
	case GC_ORDER_CONSTRUCT: { CvBuildingEntry* e = GC.getBuildingInfo((BuildingTypes)item); t = e ? e->GetType() : NULL; break; }
	case GC_ORDER_CREATE: { CvProjectEntry* e = GC.getProjectInfo((ProjectTypes)item); t = e ? e->GetType() : NULL; break; }
	case GC_ORDER_MAINTAIN: { CvProcessInfo* e = GC.getProcessInfo((ProcessTypes)item); t = e ? e->GetType() : NULL; break; }
	}
	return t ? t : "?";
}

// IsCanPurchase for order/item/yield with the two test flags (cost + readiness, trainability).
bool CanPurchaseTest(CvCity* city, int order, int item, int yield, bool testCost, bool testTrainable)
{
	const UnitTypes u = order == GC_ORDER_TRAIN ? (UnitTypes)item : NO_UNIT;
	const BuildingTypes b = order == GC_ORDER_CONSTRUCT ? (BuildingTypes)item : NO_BUILDING;
	const ProjectTypes p = order == GC_ORDER_CREATE ? (ProjectTypes)item : NO_PROJECT;
	return city->IsCanPurchase(testCost, testTrainable, u, b, p, (YieldTypes)yield);
}

}  // namespace

bool CheckCityOrder(CvCity* city, int order, int item, std::string& why)
{
	if (order != GC_ORDER_TRAIN && order != GC_ORDER_CONSTRUCT && order != GC_ORDER_CREATE && order != GC_ORDER_MAINTAIN)
	{
		why = Fmt("order type %d is not a production order (0 unit, 1 building, 2 project, 4 process)", order);
		return false;
	}
	if (!OrderItemExists(order, item))
	{
		why = Fmt("there is no item %d of order type %d", item, order);
		return false;
	}
	int headOrder, headItem;
	if (CityHeadOrder(city, &headOrder, &headItem) && headOrder == order && headItem == item)
	{
		why = Fmt("%s is already the current production", OrderItemType(order, item));
		return false;
	}
	CvString tip;
	bool ok = false;
	switch (order)
	{
	// Same call as CvCity::pushOrder, without a tooltip sink (with a sink the core only writes the text for e.g. the settler size
	// limit and still answers true). The sink call afterwards is for the reason only.
	case GC_ORDER_TRAIN: ok = city->canTrain((UnitTypes)item); if (!ok) city->canTrain((UnitTypes)item, false, false, false, false, &tip); break;
	case GC_ORDER_CONSTRUCT: ok = city->canConstruct((BuildingTypes)item); if (!ok) city->canConstruct((BuildingTypes)item, false, false, false, &tip); break;
	case GC_ORDER_CREATE: ok = city->canCreate((ProjectTypes)item, false, false); break;
	case GC_ORDER_MAINTAIN: ok = city->canMaintain((ProcessTypes)item, false); break;
	}
	if (ok) return true;
	const std::string reason = PlainReason(tip.c_str());
	why = Fmt("%s cannot be started here now%s%s", OrderItemType(order, item), reason.empty() ? "" : ": ", reason.c_str());
	return false;
}

int CityPurchaseCost(CvCity* city, int order, int item, int yield)
{
	if ((yield != GC_YIELD_GOLD && yield != GC_YIELD_FAITH) || !OrderItemExists(order, item)) return -1;
	if (order != GC_ORDER_TRAIN && order != GC_ORDER_CONSTRUCT && order != GC_ORDER_CREATE) return -1;
	if (!CanPurchaseTest(city, order, item, yield, false, false)) return -1;
	int cost = -1;
	if (yield == GC_YIELD_GOLD)
	{
		switch (order)
		{
		case GC_ORDER_TRAIN: cost = city->GetPurchaseCost((UnitTypes)item); break;
		case GC_ORDER_CONSTRUCT: cost = city->GetPurchaseCost((BuildingTypes)item); break;
		case GC_ORDER_CREATE: cost = city->GetPurchaseCost((ProjectTypes)item); break;
		}
	}
	else
	{
		switch (order)
		{
		case GC_ORDER_TRAIN: cost = city->GetFaithPurchaseCost((UnitTypes)item, true); break;
		case GC_ORDER_CONSTRUCT: cost = city->GetFaithPurchaseCost((BuildingTypes)item); break;
		}
	}
	return cost;
}

bool CheckCityPurchase(CvCity* city, int order, int item, int yield, std::string& why)
{
	if (yield != GC_YIELD_GOLD && yield != GC_YIELD_FAITH)
	{
		why = Fmt("yield %d cannot buy anything (%d gold or %d faith)", yield, (int)GC_YIELD_GOLD, (int)GC_YIELD_FAITH);
		return false;
	}
	if (order != GC_ORDER_TRAIN && order != GC_ORDER_CONSTRUCT && order != GC_ORDER_CREATE)
	{
		why = Fmt("order type %d cannot be bought (0 unit, 1 building, 2 project)", order);
		return false;
	}
	if (!OrderItemExists(order, item))
	{
		why = Fmt("there is no item %d of order type %d", item, order);
		return false;
	}
	const char* what = OrderItemType(order, item);
	const char* with = yield == GC_YIELD_GOLD ? "gold" : "faith";
	if (CanPurchaseTest(city, order, item, yield, true, true)) return true;
	if (city->IsPuppet())
	{
		why = "puppet cities cannot buy anything";
		return false;
	}
	if (!CanPurchaseTest(city, order, item, yield, false, true))
	{
		// Not startable here right now: the core's text for the same test.
		CvString tip;
		if (order == GC_ORDER_TRAIN) city->canTrain((UnitTypes)item, false, false, false, true, &tip);
		else if (order == GC_ORDER_CONSTRUCT) city->canConstruct((BuildingTypes)item, false, false, false, &tip);
		const std::string reason = PlainReason(tip.c_str());
		why = Fmt("%s cannot be bought with %s in this city now%s%s", what, with, reason.empty() ? " (not available, not allowed, or already built)" : ": ", reason.c_str());
		return false;
	}
	if (city->IsResistance() || city->IsRazing())
	{
		why = "nothing can be bought in a city that is in resistance or being razed";
		return false;
	}
	const int cost = CityPurchaseCost(city, order, item, yield);
	const CvPlayer& owner = GET_PLAYER(city->getOwner());
	const int have = yield == GC_YIELD_GOLD ? owner.GetTreasury()->GetGold() : owner.GetFaith();
	if (cost >= 0 && cost > have)
	{
		why = Fmt("not enough %s: %s costs %d, you have %d", with, what, cost, have);
		return false;
	}
	if (order == GC_ORDER_TRAIN && !city->CanPlaceUnitHere((UnitTypes)item))
	{
		why = Fmt("there is no room to place %s in this city", what);
		return false;
	}
	why = Fmt("%s cannot be bought now", what);
	return false;
}

bool CheckCityRangeAttack(CvCity* city, int x, int y, std::string& why, int32_t* reason)
{
	int32_t scratch = 0;
	int32_t& why_ = reason ? *reason : scratch;
	why_ = GC_REASON_BAD_TARGET;
	if (!OnMap(x, y))
	{
		why = Fmt("target (%d,%d) is outside the map", x, y);
		return false;
	}
	if (!city->canRangeStrike())
	{
		if (city->isMadeAttack()) why = "the city has already attacked this turn";
		else if (city->IsResistance() || city->IsRazing()) why = "a city in resistance or being razed cannot attack";
		else why = "the city cannot make a ranged attack now";
		why_ = GC_REASON_RULE;
		return false;
	}
	CvPlot* target = GC.getMap().plot(x, y);
	if (!target || !target->isVisible(city->getTeam()))
	{
		why = Fmt("target (%d,%d) is not visible", x, y);
		return false;
	}
	const int range = GC.getCITY_ATTACK_RANGE();
	if (plotDistance(city->getX(), city->getY(), x, y) > range)
	{
		why = Fmt("target (%d,%d) is out of range (%d)", x, y, range);
		return false;
	}
	if (target->isCity())
	{
		why = "a city cannot shoot at a city";
		return false;
	}
	if (!city->canRangeStrikeAt(x, y))
	{
		why = Fmt("no enemy unit to shoot at (%d,%d), or it cannot be seen from the city", x, y);
		return false;
	}
	why_ = GC_REASON_NONE;
	return true;
}

// Is declaring war on `team` possible for the human's team? why = the reason when not. (Declared in gc_command.h: Q_DIPLO uses it.)
bool CheckDeclareWar(CvPlayer& human, int team, std::string& why, int32_t* reason)
{
	*reason = GC_REASON_BAD_TARGET;
	if (team < 0 || team >= MAX_TEAMS)
	{
		why = Fmt("team %d does not exist", team);
		return false;
	}
	const TeamTypes mine = human.getTeam();
	const TeamTypes theirs = (TeamTypes)team;
	CvTeam& t = GET_TEAM(mine);
	const CvTeam& other = GET_TEAM(theirs);
	if (theirs == mine)
	{
		why = "that is the human's own team";
		return false;
	}
	if (!other.isAlive())
	{
		why = Fmt("team %d is not alive", team);
		return false;
	}
	*reason = GC_REASON_RULE;
	if (t.isAtWar(theirs))
	{
		why = Fmt("already at war with team %d", team);
		return false;
	}
	if (!t.isHasMet(theirs))
	{
		why = Fmt("team %d has not been met", team);
		return false;
	}
	if (t.isForcePeace(theirs))
	{
		why = Fmt("a peace treaty with team %d is in force", team);
		return false;
	}
	if (other.IsHasDefensivePact(mine))
	{
		why = Fmt("team %d has a defensive pact with us", team);
		return false;
	}
	if (!t.canChangeWarPeace(theirs))
	{
		why = Fmt("war and peace with team %d cannot be changed (game option or permanent)", team);
		return false;
	}
	if (!t.canDeclareWar(theirs))
	{
		why = Fmt("war on team %d is not allowed now (game rules or a mod rule)", team);
		return false;
	}
	return true;
}

// ---- P9 (N3.10): white peace between two humans ----

int SeatedHumanOfTeam(int team)
{
	for (int i = 0; i < MAX_MAJOR_CIVS; ++i)
		if (IsSeatedHuman(i) && (int)GET_PLAYER((PlayerTypes)i).getTeam() == team) return i;
	return -1;
}

int WarLockedTurns(int teamA, int teamB)
{
	if (teamA < 0 || teamA >= MAX_TEAMS || teamB < 0 || teamB >= MAX_TEAMS) return 0;
	const int x = GET_TEAM((TeamTypes)teamA).GetNumTurnsLockedIntoWar((TeamTypes)teamB);
	const int y = GET_TEAM((TeamTypes)teamB).GetNumTurnsLockedIntoWar((TeamTypes)teamA);
	return x > y ? x : y;
}

bool CheckMakePeace(CvPlayer& human, int team, int* other, std::string& why, int32_t* reason)
{
	*reason = GC_REASON_BAD_TARGET;
	if (team < 0 || team >= MAX_TEAMS)
	{
		why = Fmt("team %d does not exist", team);
		return false;
	}
	const TeamTypes mine = human.getTeam();
	const TeamTypes theirs = (TeamTypes)team;
	if (theirs == mine)
	{
		why = "that is the player's own team";
		return false;
	}
	if (!GET_TEAM(theirs).isAlive())
	{
		why = Fmt("team %d is not alive", team);
		return false;
	}
	// SIMPLIFIED(SIMP-057): peace without bargaining, between humans only; the host keeps the offer and both answers.
	const int them = SeatedHumanOfTeam(team);
	if (them < 0)
	{
		why = Fmt("team %d has no human player: only humans make peace with this command", team);
		return false;
	}
	*reason = GC_REASON_RULE;
	CvTeam& t = GET_TEAM(mine);
	if (!t.isAtWar(theirs))
	{
		why = Fmt("not at war with team %d", team);
		return false;
	}
	if (!t.canChangeWarPeace(theirs))
	{
		why = Fmt("war and peace with team %d cannot be changed (game option or permanent)", team);
		return false;
	}
	const int locked = WarLockedTurns((int)mine, team);
	if (locked > 0)
	{
		why = Fmt("the war with team %d is locked for %d more turn(s)", team, locked);
		return false;
	}
	// The same test the deal screen makes for the peace treaty item, from both sides (at war on both, and the mods' IsAbleToMakePeace hook).
	CvDeal probe(human.GetID(), (PlayerTypes)them);
	if (!probe.IsPossibleToTradeItem(human.GetID(), (PlayerTypes)them, TRADE_ITEM_PEACE_TREATY) || !probe.IsPossibleToTradeItem((PlayerTypes)them, human.GetID(), TRADE_ITEM_PEACE_TREATY))
	{
		why = Fmt("a peace treaty with team %d is not possible now (a game or mod rule)", team);
		return false;
	}
	if (other) *other = them;
	return true;
}

namespace {
int32_t ApplyHumanCommandImpl(RtSession& s, const gc_command& cmd, std::string& msg, int32_t* reasonOut);
}

bool CheckHumanMayAct(RtSession& s, int player, std::string& msg, int32_t* reason)
{
	CvPlayer* who = NULL;
	return HumanMayAct(s, player, &who, msg, reason);
}

// B4 gc-replica: an accepted command is an input of the game; a server journals it for its replicas (gc_journal_take).
int32_t ApplyHumanCommand(RtSession& s, const gc_command& cmd, std::string& msg, int32_t* reasonOut)
{
	const int32_t rc = ApplyHumanCommandImpl(s, cmd, msg, reasonOut);
	if (rc == GC_OK && JournalOn())
	{
		JournalRecord r;
		r.type = GC_JOURNAL_COMMAND;
		r.player = (int)cmd.player;
		r.kind = (int)cmd.kind;
		for (uint32_t i = 0; i < cmd.argc; ++i) r.args.push_back(cmd.args[i]);
		r.hasText = cmd.text != NULL;
		if (cmd.text) r.text = cmd.text;
		JournalAppend(r);
	}
	return rc;
}

namespace {
int32_t ApplyHumanCommandImpl(RtSession& s, const gc_command& cmd, std::string& msg, int32_t* reasonOut)
{
	int32_t reasonScratch = GC_REASON_NONE;
	int32_t& reason = reasonOut ? *reasonOut : reasonScratch;
	reason = GC_REASON_NONE;
	const int kind = (int)cmd.kind;
	const int player = (int)cmd.player;
	const int32_t* a = cmd.args;

	// the command is checked and sent as player P (the sender of every send* is the active player; the loopback delivers the
	// message as P too). Not a seated human: nothing is switched and the check below refuses it.
	ActiveAs as((PlayerTypes)player, IsSeatedHuman(player));

	if (kind == GC_CMD_CANCEL_END_TURN || kind == GC_CMD_END_TURN_TIMEOUT)
	{
		const bool cancel = kind == GC_CMD_CANCEL_END_TURN;
		const char* name = cancel ? "CANCEL_END_TURN" : "END_TURN_TIMEOUT";
		if (cmd.argc != 0)
		{
			msg = Fmt("%s takes no arguments", name);
			return GC_ERR_INVALID_ARG;
		}
		RtEngine& e = s.Engine();
		if (!e.NetworkGame())
		{
			msg = Fmt("%s: only the network game type has a turn handshake that can be taken back or forced", name);
			reason = GC_REASON_RULE;
			return GC_ERR_REJECTED;
		}
		CvPlayer* human = NULL;
		if (!HumanMayAct(s, player, &human, msg, &reason, !cancel))
		{
			msg = Fmt("%s: %s", name, msg.c_str());
			return GC_ERR_REJECTED;
		}
		if (cancel)
		{
			// The end of turn is taken back by the exe's NetTurnUnready, which it allows until NetTurnAllComplete went out (RtEngine::sendTurnUnready).
			// An end of turn P has not given is nothing to take back: the command is idempotent.
			if (!e.TurnCompleteSent((PlayerTypes)player)) return GC_OK;
			if (!e.sendTurnUnready())
			{
				msg = "CANCEL_END_TURN: every human has ended the turn already, the turn is being closed";
				reason = GC_REASON_TURN_ENDED;
				return GC_ERR_REJECTED;
			}
			// CancelActivePlayerEndTurn (CvPlayer.cpp:31713): the core's own end-turn flag goes with it.
			if (human->isEndTurn()) human->setEndTurn(false);
			return GC_OK;
		}
		// RW-23: the turn timer ran out. The expiry is ours, not the exe's: it is HumanMayAct, then sendTurnComplete, no blocker checks. The leftover
		// blockers (choices, cities without production) stay as they are; the next turn's start deals with them as it does for the AI.
		e.sendTurnComplete();
		return GC_OK;
	}

	if (kind == GC_CMD_MAKE_PEACE)
	{
		// P9 (N3.10): both humans agreed (the host keeps the offer, GH-R11); the brain checks the war and the rules for peace and
		// then concludes the "peace treaty" deal from both sides, the path of an accepted deal (CvDealAI::DoAcceptedDeal: AddProposedDeal, FinalizeDeal):
		// makePeace and setForcePeace for the treaty's game length (PEACE_TREATY_LENGTH turns, the end of the item lifts it).
		if (cmd.argc != 1)
		{
			msg = "MAKE_PEACE takes one argument: team";
			return GC_ERR_INVALID_ARG;
		}
		if (!IsSeatedHuman(player))
		{
			msg = Fmt("MAKE_PEACE refused: player %d is not the human player of this game", player);
			reason = GC_REASON_NOT_HUMAN;
			return GC_ERR_REJECTED;
		}
		if (!s.Engine().allAICivsProcessedThisTurn() || !GC.getGame().allUnitAIProcessed())
		{
			msg = "MAKE_PEACE refused: the AI civilizations are still moving";
			reason = GC_REASON_AI_RUNNING;
			return GC_ERR_REJECTED;
		}
		CvPlayer& me = GET_PLAYER((PlayerTypes)player);
		int them = -1;
		std::string why;
		if (!CheckMakePeace(me, (int)a[0], &them, why, &reason))
		{
			msg = Fmt("MAKE_PEACE: %s", why.c_str());
			return GC_ERR_REJECTED;
		}
		CvDeal deal(me.GetID(), (PlayerTypes)them);
		deal.SetPeaceTreatyType(PEACE_TREATY_WHITE_PEACE);
		deal.AddPeaceTreaty(me.GetID(), GC.getPEACE_TREATY_LENGTH());
		deal.AddPeaceTreaty((PlayerTypes)them, GC.getPEACE_TREATY_LENGTH());
		CvGameDeals* deals = GC.getGame().GetGameDeals();
		deals->AddProposedDeal(deal);
		if (!deals->FinalizeDeal(me.GetID(), (PlayerTypes)them, true))
		{
			msg = Fmt("MAKE_PEACE: the peace treaty with team %d was not accepted by the game", (int)a[0]);
			reason = GC_REASON_RULE;
			return GC_ERR_REJECTED;
		}
		reason = GC_REASON_NONE;
		return GC_OK;
	}

	if (kind == GC_CMD_END_TURN || kind == GC_CMD_FORCE_END_TURN)
	{
		const char* name = kind == GC_CMD_END_TURN ? "END_TURN" : "FORCE_END_TURN";
		if (cmd.argc != 0)
		{
			msg = Fmt("%s takes no arguments", name);
			return GC_ERR_INVALID_ARG;
		}
		CvPlayer* who = NULL;
		if (!HumanMayAct(s, player, &who, msg, &reason))
		{
			msg = Fmt("%s refused: %s", name, msg.c_str());
			return GC_ERR_REJECTED;
		}
		const PlayerTypes active = (PlayerTypes)player;
		if (kind == GC_CMD_FORCE_END_TURN && s.IsHumanWaiting((PlayerTypes)player))
		{
			// CvGame::UpdateMoves lets only one awake unit consume the force flag: with 2 or more the turn would not end.
			CvPlayer& hp = GET_PLAYER(active);
			const int blocking = (int)hp.GetEndTurnBlockingType();
			int awake = 0, loop;
			for (CvUnit* u = hp.firstUnit(&loop); u; u = hp.nextUnit(&loop))
				if (!u->isDelayedDeath() && u->ReadyToMove()) ++awake;
			if ((blocking == ENDTURN_BLOCKING_UNITS || blocking == ENDTURN_BLOCKING_STACKED_UNITS) && awake >= 2)
			{
				msg = Fmt("FORCE_END_TURN refused: %d awake units need orders and the gamecore lets only one consume the force flag; give them orders (skip, sleep, fortify) first", awake);
				reason = GC_REASON_BLOCKED;
				return GC_ERR_REJECTED;
			}
		}
		const RtSession::EndTurnResult r = s.EndTurn(kind == GC_CMD_FORCE_END_TURN);
		if (r.status == RtSession::END_TURN_ACCEPTED) return GC_OK;
		msg = Fmt("%s refused: %s", name, r.reason.c_str());
		if (r.status == RtSession::END_TURN_REJECTED_BLOCKED && r.target.kind != RtSession::TARGET_NONE)
		{
			msg += Fmt("; %s %d at (%d,%d)", RtSession::TargetKindName(r.target.kind), r.target.id, r.target.x, r.target.y);
		}
		// Blocked: a blocker holds the turn. Not ready: a queued command or a busy unit/city asks for a gc_pump first.
		reason = r.status == RtSession::END_TURN_REJECTED_BLOCKED ? GC_REASON_BLOCKED : GC_REASON_PUMP_FIRST;
		return GC_ERR_REJECTED;
	}

	if (kind == GC_CMD_CITY_SET_PRODUCTION || kind == GC_CMD_CITY_PURCHASE || kind == GC_CMD_CITY_RANGE_ATTACK || kind == GC_CMD_DECLARE_WAR)
	{
		static const char* const names[] = {"CITY_SET_PRODUCTION", "CITY_PURCHASE", "CITY_RANGE_ATTACK", "DECLARE_WAR"};
		static const char* const usage[] = {"city, order_type, item", "city, order_type, item, yield", "city, x, y", "team"};
		static const uint32_t wants[] = {3u, 4u, 3u, 1u};
		const int idx = kind == GC_CMD_CITY_SET_PRODUCTION ? 0 : kind == GC_CMD_CITY_PURCHASE ? 1 : kind == GC_CMD_CITY_RANGE_ATTACK ? 2 : 3;
		if (cmd.argc != wants[idx])
		{
			msg = Fmt("%s takes %u argument(s): %s", names[idx], (unsigned)wants[idx], usage[idx]);
			return GC_ERR_INVALID_ARG;
		}
		CvPlayer* human = NULL;
		if (!HumanMayAct(s, player, &human, msg, &reason)) return GC_ERR_REJECTED;
		RtEngine& e = s.Engine();
		std::string why;
		if (kind == GC_CMD_DECLARE_WAR)
		{
			if (!CheckDeclareWar(*human, (int)a[0], why, &reason))
			{
				msg = Fmt("DECLARE_WAR: %s", why.c_str());
				return GC_ERR_REJECTED;
			}
			e.sendChangeWar((TeamTypes)a[0], true);
			return GC_OK;
		}
		reason = GC_REASON_BAD_TARGET;
		CvCity* city = human->getCity(a[0]);
		if (!city)
		{
			msg = Fmt("player %d has no city %d", player, (int)a[0]);
			return GC_ERR_REJECTED;
		}
		reason = GC_REASON_RULE;
		if (kind == GC_CMD_CITY_RANGE_ATTACK)
		{
			if (!CheckCityRangeAttack(city, (int)a[1], (int)a[2], why, &reason))
			{
				msg = Fmt("city %d (%s): %s", (int)a[0], city->getName().c_str(), why.c_str());
				return GC_ERR_REJECTED;
			}
			reason = GC_REASON_NONE;
			e.sendDoTask(city->GetID(), TASK_RANGED_ATTACK, (int)a[1], (int)a[2], false, false, false, false);
			return GC_OK;
		}
		if (kind == GC_CMD_CITY_SET_PRODUCTION)
		{
			if (city->IsPuppet())
			{
				msg = Fmt("city %d (%s): a puppet city chooses its own production", (int)a[0], city->getName().c_str());
				return GC_ERR_REJECTED;
			}
			if (!CheckCityOrder(city, (int)a[1], (int)a[2], why))
			{
				msg = Fmt("city %d (%s): %s", (int)a[0], city->getName().c_str(), why.c_str());
				return GC_ERR_REJECTED;
			}
			// What the production popup sends for a click: replace the queue (pop), append the new head (the core's ctrl flag).
			e.sendPushOrder(city->GetID(), (OrderTypes)a[1], (int)a[2], false, true, true);
			return GC_OK;
		}
		if (!CheckCityPurchase(city, (int)a[1], (int)a[2], (int)a[3], why))
		{
			msg = Fmt("city %d (%s): %s", (int)a[0], city->getName().c_str(), why.c_str());
			return GC_ERR_REJECTED;
		}
		e.sendPurchase(city->GetID(), a[1] == GC_ORDER_TRAIN ? (UnitTypes)a[2] : NO_UNIT, a[1] == GC_ORDER_CONSTRUCT ? (BuildingTypes)a[2] : NO_BUILDING,
			a[1] == GC_ORDER_CREATE ? (ProjectTypes)a[2] : NO_PROJECT, (int)a[3]);
		return GC_OK;
	}

	if (kind < GC_CMD_UNIT_MOVE_TO || kind > GC_CMD_UNIT_WAKE)
	{
		msg = Fmt("unknown command kind %d", kind);
		return GC_ERR_INVALID_ARG;
	}

	const bool targeted = kind == GC_CMD_UNIT_MOVE_TO || kind == GC_CMD_UNIT_RANGE_ATTACK;
	const uint32_t want = targeted ? 3u : 1u;
	if (cmd.argc != want)
	{
		msg = Fmt("command %d takes %u argument(s): unit%s", kind, (unsigned)want, targeted ? ", x, y" : "");
		return GC_ERR_INVALID_ARG;
	}

	CvPlayer* human = NULL;
	if (!HumanMayAct(s, player, &human, msg, &reason)) return GC_ERR_REJECTED;
	CvUnit* unit = human->getUnit(a[0]);
	if (!unit)
	{
		msg = Fmt("player %d has no unit %d", player, (int)a[0]);
		reason = GC_REASON_BAD_TARGET;
		return GC_ERR_REJECTED;
	}
	const int x = targeted ? (int)a[1] : -1;
	const int y = targeted ? (int)a[2] : -1;
	std::string why;
	if (!CheckUnitCommand(unit, kind, targeted, x, y, why, &reason))
	{
		msg = Fmt("unit %d (%s): %s", (int)a[0], UnitTypeName(unit), why.c_str());
		return GC_ERR_REJECTED;
	}
	reason = GC_REASON_NONE;

	RtEngine& e = s.Engine();
	const int id = unit->GetID();
	switch (kind)
	{
	case GC_CMD_UNIT_MOVE_TO:
	case GC_CMD_UNIT_RANGE_ATTACK:
	case GC_CMD_UNIT_FOUND_CITY:
	case GC_CMD_UNIT_FORTIFY:
	case GC_CMD_UNIT_SLEEP:
	case GC_CMD_UNIT_SKIP:
	case GC_CMD_UNIT_HEAL:
		e.sendPushMission(id, MissionOf(kind), x, y, 0, false);
		break;
	case GC_CMD_UNIT_AUTOMATE_BUILD: e.sendDoCommand(id, COMMAND_AUTOMATE, AUTOMATE_BUILD, -1, false); break;
	case GC_CMD_UNIT_AUTOMATE_EXPLORE: e.sendDoCommand(id, COMMAND_AUTOMATE, AUTOMATE_EXPLORE, -1, false); break;
	case GC_CMD_UNIT_CANCEL: e.sendDoCommand(id, unit->IsAutomated() ? COMMAND_STOP_AUTOMATION : COMMAND_CANCEL_ALL, -1, -1, false); break;
	case GC_CMD_UNIT_DISBAND: e.sendDoCommand(id, COMMAND_DELETE, -1, -1, false); break;
	case GC_CMD_UNIT_WAKE: e.sendDoCommand(id, COMMAND_WAKE, -1, -1, false); break;
	}
	return GC_OK;
}

}  // namespace

}  // namespace lekmod_portable
