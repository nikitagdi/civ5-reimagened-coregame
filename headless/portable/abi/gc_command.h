// Human commands for gc_apply_command and the unit helpers the queries share (Q_UNIT_ACTIONS, Q_PATH,
// Q_REACHABLE). A command is validated against the gamecore, then sent as the human through the loopback (gDLL->send*), which delivers
// it to the net message handler on the next RtSession::Pump. Nothing here changes the gamecore.
#pragma once

#include <stdint.h>
#include <string>
#include <vector>

#include "../runtime/rt_session.h"
#include "gamecore_abi.h"

class CvUnit;
class CvCity;
class CvPlayer;

namespace lekmod_portable {

// Applies cmd: GC_OK (queued, or for END_TURN / FORCE_END_TURN already sent), GC_ERR_REJECTED (msg = reason, nothing queued),
// GC_ERR_INVALID_ARG (wrong argc, unknown kind) or GC_ERR_NOT_IMPLEMENTED (known kind of a later task).
// v2: cmd.player is the human the command is for (any seated human); it runs as that human (ActiveAs). When reason is not
// NULL it gets the GC_REASON_* of a refusal (GC_REASON_NONE otherwise).
int32_t ApplyHumanCommand(RtSession& session, const gc_command& cmd, std::string& msg, int32_t* reason = NULL);

// HumanMayAct for other entries (B4 gc_net_submit): player is a seated human, the AI has moved, its turn is active and not ended.
bool CheckHumanMayAct(RtSession& session, int player, std::string& msg, int32_t* reason);

// Can this unit take unit command kind (GC_CMD_UNIT_*) now? haveTarget: x, y are the command's target (UNIT_MOVE_TO, UNIT_RANGE_ATTACK),
// otherwise only the target-independent part is tested. False + msg (the reason) when not; reason (when not NULL) gets its GC_REASON_*.
bool CheckUnitCommand(CvUnit* unit, int kind, bool haveTarget, int x, int y, std::string& msg, int32_t* reason = NULL);

// Does the command kind apply to this unit at all (a Settler has no ranged attack)? Q_UNIT_ACTIONS lists only these.
bool UnitCommandApplies(CvUnit* unit, int kind);

struct PathStep
{
	int x, y;
	int turn;   // turns from now (0 = this turn)
	int moves;  // moves left on arrival, 1/60
};
// The gamecore's interface pathfinder from the unit's plot to (x, y), first step = the unit's plot. False when there is no path.
bool FindUnitPath(CvUnit* unit, int x, int y, std::vector<PathStep>& steps);

// ---- G1a: city commands (CITY_SET_PRODUCTION 20, CITY_PURCHASE 21, CITY_RANGE_ATTACK 22) and DECLARE_WAR 30 ----
// Shared with Q_CITY_BUILDS (gc_query_city.cpp). order is GC_ORDER_*, item the core's type index.

// Head of the city's order queue (the current production); false when the queue is empty.
bool CityHeadOrder(const CvCity* city, int* order, int* item);

// Can the city start this production now (canTrain / canConstruct / canCreate / canMaintain)? Not when it is already the head of
// the queue. False + why (the core's own text, tags stripped, or a plain sentence) when not.
bool CheckCityOrder(CvCity* city, int order, int item, std::string& why);

// Cost of buying order/item with yield (GC_YIELD_GOLD or GC_YIELD_FAITH); -1 when this city can never buy it (IsCanPurchase with the
// cost and trainability tests off fails). Gold on hand and the item being startable right now are not looked at.
int CityPurchaseCost(CvCity* city, int order, int item, int yield);

// Would CITY_PURCHASE be accepted now? False + why (not enough gold, puppet, resistance, nothing to buy ...) when not.
bool CheckCityPurchase(CvCity* city, int order, int item, int yield, std::string& why);

// Can the city shoot at (x, y) now? False + why when not.
bool CheckCityRangeAttack(CvCity* city, int x, int y, std::string& why, int32_t* reason = NULL);

// ---- P9 (N3.10): war and peace between humans; shared with Q_DIPLO (gc_diplo.cpp) ----
// Is declaring war on `team` possible for the player's team? why = the reason when not; reason (not NULL) gets the GC_REASON_*.
bool CheckDeclareWar(CvPlayer& human, int team, std::string& why, int32_t* reason);
// Is white peace with `team` possible now (MAKE_PEACE): the team has a seated human (returned in *other), the war goes on, the mods allow a peace
// treaty and no coop-war lock holds it? why = the reason when not.
bool CheckMakePeace(CvPlayer& human, int team, int* other, std::string& why, int32_t* reason);
// Turns the war between two teams is still locked (CvTeam::GetNumTurnsLockedIntoWar, the larger of the two sides); 0 = free.
int WarLockedTurns(int teamA, int teamB);
// The seated human of a team (one player per team, SIMP-054), -1 when there is none.
int SeatedHumanOfTeam(int team);

// The core's refusal text turned into one plain line: [TAGS] removed, [NEWLINE] -> "; ", trimmed.
std::string PlainReason(const char* text);

}  // namespace lekmod_portable
