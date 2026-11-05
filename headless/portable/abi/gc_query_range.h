// G1b: Q_RANGE_TARGETS (record layout gcq_range_target in gamecore_abi.h); v2 (N3.9): Q_REACHABLE_TREE, Q_COMBAT_PREVIEW and the
// node flags of Q_PATH / Q_REACHABLE. Read-only over the gamecore objects; one buffer per call, owned by the caller (gc_buf release frees it). C++03.
#pragma once

#include <stdint.h>
#include <string>

#include "gamecore_abi.h"

class CvUnit;
class CvPlot;

namespace lekmod_portable {

// Answers GC_Q_RANGE_TARGETS, args = {player, shooter}: shooter >= 0 is a unit id of the player, shooter < 0 is -(city id) of the player.
// The targets are exactly the plots the matching command (GC_CMD_UNIT_RANGE_ATTACK: CheckUnitCommand; GC_CMD_CITY_RANGE_ATTACK:
// CheckCityRangeAttack) accepts now, by y then x. GC_OK, or GC_ERR_INVALID_ARG (wrong argc, player out of range, no such unit / city);
// on error msg says why and out is untouched.
int32_t QueryRangeTargets(const int32_t* args, uint32_t argc, gc_buf* out, std::string& msg);

// GC_REACH_F_* of the step of `unit` from plot `from` onto plot `to` (a path node, a tree node): `moves` = moves left on arrival (1/60), `last` = the step ends the path, the
// only one that can attack or capture. CITY, EMBARK / DISEMBARK (land unit between land and water), STOP (no moves left on arrival: ZOC or the end of the moves),
// UNREVEALED (the plot is not revealed to the unit's team); on the last step also ATTACK (a visible enemy defender, or an enemy city that still stands) and CAPTURE (an enemy
// civilian alone on the plot, or an enemy city with no hit points left); UNREACHABLE is Q_PATH's own.
unsigned PathStepFlags(CvUnit* unit, const CvPlot* from, const CvPlot* to, int moves, bool last);

// Q_REACHABLE_TREE, args = {player, unit [, horizon 1..3, default 1]}: the plots the unit reaches within `horizon` turns, each with its parent. Built from the same interface path
// search as UNIT_MOVE_TO and Q_PATH (FindUnitPath), one search per revealed plot within reach: the node of a plot is exactly what Q_PATH to that plot ends with (turn,
// moves_left, the step before the end as the parent). By y then x. GC_ERR_INVALID_ARG for a wrong argc, horizon, player or unit.
int32_t QueryReachableTree(const int32_t* args, uint32_t argc, gc_buf* out, std::string& msg);

// Q_COMBAT_PREVIEW, args = {player, attacker, x, y}: attacker >= 0 a unit id of the player, < 0 = -(city id). The expected result of the attack on plot (x, y), computed the way
// the combat itself computes it but without the random roll and without any side effect; zero records when the plot is not visible to the player's team or the attacker
// cannot attack it (ranged: the command check of the range attack; melee: an enemy there and a path to the plot). GC_ERR_INVALID_ARG for a wrong argc, player, attacker or plot.
int32_t QueryCombatPreview(const int32_t* args, uint32_t argc, gc_buf* out, std::string& msg);

}  // namespace lekmod_portable
