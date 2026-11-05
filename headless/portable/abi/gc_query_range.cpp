// Q_RANGE_TARGETS (G1b; record gcq_range_target in gamecore_abi.h). C++03.
// The answer is the command check itself run over every plot within the shooter's range, so a listed plot is exactly one that
// GC_CMD_UNIT_RANGE_ATTACK (CheckUnitCommand -> CanStartMission(MISSION_RANGE_ATTACK) -> CvUnit::canRangeStrikeAt, war needed) or
// GC_CMD_CITY_RANGE_ATTACK (CheckCityRangeAttack -> CvCity::canRangeStrikeAt) accepts now. Nothing here changes the game.
#include "CvGameCoreDLLPCH.h"

#include "CvGameCoreUtils.h"
#include "gc_command.h"
#include "gc_gcq_writer.h"
#include "gc_query_range.h"

#include <algorithm>
#include <string.h>

namespace lekmod_portable {

namespace {

#define GCQ_ASSERT_SIZE(type, size) typedef char gcq_size_check_##type[(sizeof(type) == (size)) ? 1 : -1]
GCQ_ASSERT_SIZE(gcq_range_target, 8);

int32_t Invalid(std::string& msg, const char* text)
{
	msg = text;
	return GC_ERR_INVALID_ARG;
}

void AddTarget(GcqBuf& w, const CvPlot* plot)
{
	gcq_range_target r;
	memset(&r, 0, sizeof(r));
	r.x = (int16_t)plot->getX();
	r.y = (int16_t)plot->getY();
	unsigned flags = 0;
	if (plot->isCity()) flags |= GC_RANGE_TARGET_F_CITY;
	r.flags = (uint16_t)flags;
	w.Add(&r);
}

// Every plot within `range` of (x0, y0) (plotDistance, so across the wrap too), by y then x, that `accept` takes.
template <class Accept>
void Scan(GcqBuf& w, int x0, int y0, int range, Accept accept)
{
	CvMap& map = GC.getMap();
	const int yLast = std::min((int)map.getGridHeight() - 1, y0 + range);
	for (int y = std::max(0, y0 - range); y <= yLast; ++y)
	{
		for (int x = 0; x < map.getGridWidth(); ++x)
		{
			if (plotDistance(x0, y0, x, y) > range) continue;
			const CvPlot* plot = map.plot(x, y);
			if (plot && accept(x, y)) AddTarget(w, plot);
		}
	}
}

struct UnitAccept
{
	CvUnit* unit;
	bool operator()(int x, int y) const
	{
		std::string why;
		return CheckUnitCommand(unit, GC_CMD_UNIT_RANGE_ATTACK, true, x, y, why);
	}
};

struct CityAccept
{
	CvCity* city;
	bool operator()(int x, int y) const
	{
		std::string why;
		return CheckCityRangeAttack(city, x, y, why);
	}
};

}  // namespace

int32_t QueryRangeTargets(const int32_t* args, uint32_t argc, gc_buf* out, std::string& msg)
{
	if (argc != 2) return Invalid(msg, "Q_RANGE_TARGETS takes two arguments: player, shooter (unit id, or -city id)");
	const int32_t player = args[0];
	const int32_t shooter = args[1];
	if (player < 0 || player >= MAX_PLAYERS) return Invalid(msg, "Q_RANGE_TARGETS: player out of range");
	CvPlayer& pl = GET_PLAYER((PlayerTypes)player);
	GcqBuf w(GC_Q_RANGE_TARGETS, sizeof(gcq_range_target));
	if (shooter >= 0)
	{
		CvUnit* u = pl.getUnit(shooter);
		if (!u) return Invalid(msg, "Q_RANGE_TARGETS: the player has no such unit");
		std::string why;
		// The target-less part of the command check (dying, busy, canRangeStrike): a unit that cannot shoot now has no targets.
		if (CheckUnitCommand(u, GC_CMD_UNIT_RANGE_ATTACK, false, -1, -1, why))
		{
			UnitAccept a;
			a.unit = u;
			Scan(w, u->getX(), u->getY(), u->GetRange(), a);
		}
	}
	else
	{
		// City ids of the core are never 0 (FFreeListTrashArray ids start at 1 << 13), so -id is unambiguous; INT32_MIN has no negation.
		if (shooter < -0x7FFFFFFF) return Invalid(msg, "Q_RANGE_TARGETS: the player has no such city");
		CvCity* c = pl.getCity(-shooter);
		if (!c) return Invalid(msg, "Q_RANGE_TARGETS: the player has no such city");
		if (c->canRangeStrike())
		{
			CityAccept a;
			a.city = c;
			Scan(w, c->getX(), c->getY(), GC.getCITY_ATTACK_RANGE(), a);
		}
	}
	if (!w.Finish(out))
	{
		msg = "out of memory";
		return GC_ERR_INTERNAL;
	}
	return GC_OK;
}

// ---- v2 (N3.9): node flags, Q_REACHABLE_TREE, Q_COMBAT_PREVIEW ----

unsigned PathStepFlags(CvUnit* u, const CvPlot* from, const CvPlot* to, int moves, bool last)
{
	unsigned f = 0;
	const TeamTypes team = GET_PLAYER(u->getOwner()).getTeam();
	if (!to->isRevealed(team, false)) f |= GC_REACH_F_UNREVEALED;
	if (to->isCity()) f |= GC_REACH_F_CITY;
	if (from && u->getDomainType() == DOMAIN_LAND)
	{
		if (!from->isWater() && to->isWater()) f |= GC_REACH_F_EMBARK;
		else if (from->isWater() && !to->isWater()) f |= GC_REACH_F_DISEMBARK;
	}
	if (moves <= 0) f |= GC_REACH_F_STOP;
	if (last && u->IsCombatUnit())
	{
		if (to->isVisibleEnemyDefender(u)) f |= GC_REACH_F_ATTACK;
		else if (to->isEnemyCity(*u))
		{
			const CvCity* c = to->getPlotCity();
			f |= (c && c->getDamage() >= c->GetMaxHitPoints()) ? GC_REACH_F_CAPTURE : GC_REACH_F_ATTACK;
		}
		else if (to->isVisibleEnemyUnit(u)) f |= GC_REACH_F_CAPTURE;
	}
	return f;
}

namespace {

#define GCQ_ASSERT_SIZE_RANGE(type, size) typedef char gcq_size_check_range_##type[(sizeof(type) == (size)) ? 1 : -1]
GCQ_ASSERT_SIZE_RANGE(gcq_tree_node, 16);
GCQ_ASSERT_SIZE_RANGE(gcq_combat_preview, 20);

int16_t Clamp16(int v) { return (int16_t)(v < -32768 ? -32768 : (v > 32767 ? 32767 : v)); }

// ---- Q_REACHABLE_TREE ----

// The search radius, in plots, for a horizon: the unit's moves per turn x3 (roads) per turn, capped (a plot farther away than the cap is not tried allows a
// one-pass search later if the measure S4 asks for it).
int TreeRadius(const CvUnit* u, int horizon)
{
	const int denom = GC.getMOVE_DENOMINATOR();
	const int moves = horizon == 1 ? u->movesLeft() : u->maxMoves();
	int perTurn = (moves + denom - 1) / denom;
	if (perTurn < 1) perTurn = 1;
	const int cap = horizon == 1 ? 10 : (horizon == 2 ? 14 : 18);
	return std::min(perTurn * 3 * horizon, cap);
}

// ---- Q_COMBAT_PREVIEW ----

struct Preview
{
	int damageToTarget, damageToAttacker;
	int attackerStrength, defenderStrength;
	int targetHp, targetMaxHp;
	unsigned flags;
	Preview() : damageToTarget(0), damageToAttacker(0), attackerStrength(0), defenderStrength(0), targetHp(0), targetMaxHp(0), flags(0) {}
};

int RemainingHp(const CvUnit* u) { return GC.getMAX_HIT_POINTS() - u->getDamage(); }

// A unit shooting at a unit or a city (CvUnitCombat::GenerateRangedCombatInfo without the random roll; this build has no ranged counterattacks, DEL_RANGED_COUNTERATTACKS
// is off, so the shooter takes no damage). The command check of the range attack has passed.
bool PreviewRanged(CvUnit* u, CvPlot* plot, Preview& p)
{
	CvCity* tcity = plot->isEnemyCity(*u) ? plot->getPlotCity() : NULL;
	if (tcity)
	{
		int dmg = u->GetRangeCombatDamage(NULL, tcity, false);
		if (dmg + tcity->getDamage() >= tcity->GetMaxHitPoints()) dmg = tcity->GetMaxHitPoints() - tcity->getDamage() - 1;  // a city cannot be shot below 1 hp
		if (dmg < 0) dmg = 0;
		p.damageToTarget = dmg;
		p.attackerStrength = u->GetMaxRangedCombatStrength(NULL, tcity, true, true);
		p.defenderStrength = tcity->getStrengthValue();
		p.targetMaxHp = tcity->GetMaxHitPoints();
		p.targetHp = p.targetMaxHp - tcity->getDamage();
		p.flags |= GC_COMBAT_F_CITY;
		return true;
	}
	const UnitHandle defender = plot->getBestDefender(NO_PLAYER, u->getOwner(), u, true);
	CvUnit* def = defender.pointer();
	if (!def) return false;
	int dmg = u->GetRangeCombatDamage(def, NULL, false);
	const int left = RemainingHp(def);
	if (dmg > left) dmg = left;
	p.damageToTarget = dmg;
	p.attackerStrength = u->GetMaxRangedCombatStrength(def, NULL, true, true);
	p.defenderStrength = def->GetMaxDefenseStrength(plot, u, true);
	p.targetMaxHp = def->GetMaxHitPoints();
	p.targetHp = def->GetCurrHitPoints();
	if (dmg >= left) p.flags |= GC_COMBAT_F_KILLS;
	return true;
}

// A melee unit attacking the plot from `from` (the unit's own plot, or the last plot of its path): CvUnitCombat::GenerateMeleeCombatInfo without the roll.
bool PreviewMelee(CvUnit* u, CvPlot* plot, const CvPlot* from, Preview& p)
{
	const int maxHP = GC.getMAX_HIT_POINTS();
	CvCity* tcity = plot->isEnemyCity(*u) ? plot->getPlotCity() : NULL;
	if (tcity)
	{
		const int aStr = u->GetMaxAttackStrength(from, plot, NULL);
		const int dStr = tcity->getStrengthValue();
		if (aStr <= 0) return false;
		const int dmgT = u->getCombatDamage(aStr, dStr, u->getDamage(), false, false, true);
		const int dmgA = u->getCombatDamage(dStr, aStr, tcity->getDamage(), false, true, false);
		const int cityMax = tcity->GetMaxHitPoints();
		p.attackerStrength = aStr;
		p.defenderStrength = dStr;
		p.targetMaxHp = cityMax;
		p.targetHp = cityMax - tcity->getDamage();
		p.damageToTarget = std::min(dmgT, p.targetHp);
		p.damageToAttacker = std::min(dmgA, RemainingHp(u));
		p.flags |= GC_COMBAT_F_CITY;
		// The city falls when its damage reaches its maximum; if the unit dies in the same fight the unit still wins (GenerateMeleeCombatInfo).
		if (dmgT + tcity->getDamage() >= cityMax && !u->isNoCapture()) p.flags |= GC_COMBAT_F_CAPTURES;
		return true;
	}
	const UnitHandle defender = plot->getBestDefender(NO_PLAYER, u->getOwner(), u, true);
	CvUnit* def = defender.pointer();
	if (!def)
	{
		// A civilian alone on the plot (captured or killed by the move-in).
		const UnitHandle civilian = plot->getBestDefender(NO_PLAYER, u->getOwner(), u, true, false, false, true);
		def = civilian.pointer();
		if (!def) return false;
	}
	p.targetMaxHp = def->GetMaxHitPoints();
	p.targetHp = def->GetCurrHitPoints();
	if (!def->IsCanDefend())
	{
		p.damageToTarget = RemainingHp(def);
		p.flags |= GC_COMBAT_F_KILLS;
		if (!u->isNoCapture() && (!def->isEmbarked() || def->getUnitInfo().IsCaptureWhileEmbarked()) &&
			def->getCaptureUnitType(GET_PLAYER(def->getOwner()).getCivilizationType()) != NO_UNIT)
			p.flags |= GC_COMBAT_F_CAPTURES;
		return true;
	}
	const int aStr = u->GetMaxAttackStrength(from, plot, def);
	if (aStr <= 0) return false;
	const int dStr = def->GetMaxDefenseStrength(plot, u, false);
	p.attackerStrength = aStr;
	p.defenderStrength = dStr;
	const int dmgT = u->getCombatDamage(aStr, dStr, u->getDamage(), false, false, false);
	const int dmgA = def->getCombatDamage(dStr, aStr, def->getDamage(), false, false, false);
	p.damageToTarget = std::min(std::min(dmgT, maxHP), RemainingHp(def));
	p.damageToAttacker = std::min(std::min(dmgA, maxHP), RemainingHp(u));
	if (def->getDamage() + dmgT >= maxHP) p.flags |= GC_COMBAT_F_KILLS;
	return true;
}

bool PreviewUnit(CvUnit* u, CvPlot* plot, Preview& p)
{
	if (u->isDelayedDeath() || u->IsBusy() || !u->IsCombatUnit()) return false;
	if (u->IsCanAttackRanged())
	{
		std::string why;
		if (!CheckUnitCommand(u, GC_CMD_UNIT_RANGE_ATTACK, true, plot->getX(), plot->getY(), why)) return false;
		return PreviewRanged(u, plot, p);
	}
	if (!u->IsCanAttack() || (!plot->isVisibleEnemyUnit(u) && !plot->isEnemyCity(*u))) return false;
	const CvPlot* from = u->plot();
	if (plot->isAdjacent(from))
	{
		if (!u->canMoveInto(*plot, CvUnit::MOVEFLAG_ATTACK | CvUnit::MOVEFLAG_DECLARE_WAR)) return false;
	}
	else
	{
		// Not next to the target: the attack the unit would make at the end of its path to it (the step before the last is where it fights from).
		if (u->IsImmobile()) return false;
		std::vector<PathStep> steps;
		if (!FindUnitPath(u, plot->getX(), plot->getY(), steps) || steps.size() < 2 || steps.back().x != plot->getX() || steps.back().y != plot->getY()) return false;
		from = GC.getMap().plot(steps[steps.size() - 2].x, steps[steps.size() - 2].y);
		if (!from) return false;
	}
	return PreviewMelee(u, plot, from, p);
}

// A city shooting at a unit (CvUnitCombat::GenerateRangedCityCombatInfo without the roll); cities shoot at units only.
bool PreviewCity(CvCity* c, CvPlot* plot, Preview& p)
{
	std::string why;
	if (!CheckCityRangeAttack(c, plot->getX(), plot->getY(), why)) return false;
	const UnitHandle defender = plot->getBestDefender(NO_PLAYER, c->getOwner(), NULL, true);
	CvUnit* def = defender.pointer();
	if (!def) return false;
	int dmg = c->rangeCombatDamage(def, NULL, false);
	const int left = RemainingHp(def);
	if (dmg > left) dmg = left;
	p.damageToTarget = dmg;
	p.attackerStrength = c->getStrengthValue();
	p.defenderStrength = def->GetMaxDefenseStrength(plot, NULL, true);
	p.targetMaxHp = def->GetMaxHitPoints();
	p.targetHp = def->GetCurrHitPoints();
	if (dmg >= left) p.flags |= GC_COMBAT_F_KILLS;
	return true;
}

}  // namespace

int32_t QueryReachableTree(const int32_t* args, uint32_t argc, gc_buf* out, std::string& msg)
{
	if (argc < 2 || argc > 3) return Invalid(msg, "Q_REACHABLE_TREE takes two or three arguments: player, unit, [horizon 1..3]");
	const int32_t horizon = argc == 3 ? args[2] : 1;
	if (horizon < 1 || horizon > 3) return Invalid(msg, "Q_REACHABLE_TREE: horizon must be 1..3");
	if (args[0] < 0 || args[0] >= MAX_PLAYERS) return Invalid(msg, "Q_REACHABLE_TREE: player out of range");
	CvUnit* u = GET_PLAYER((PlayerTypes)args[0]).getUnit(args[1]);
	if (!u) return Invalid(msg, "Q_REACHABLE_TREE: the player has no such unit");
	CvMap& map = GC.getMap();
	const TeamTypes team = GET_PLAYER(u->getOwner()).getTeam();
	GcqBuf w(GC_Q_REACHABLE_TREE, sizeof(gcq_tree_node));
	// A unit with no moves left reaches something only from the next turn on.
	const bool canGo = !u->isDelayedDeath() && !u->IsBusy() && !u->IsImmobile() && (horizon > 1 || u->canMove());
	if (canGo)
	{
		const int radius = TreeRadius(u, horizon);
		std::vector<PathStep> steps;
		const int x0 = u->getX(), y0 = u->getY();
		const int yLast = std::min((int)map.getGridHeight() - 1, y0 + radius);
		for (int y = std::max(0, y0 - radius); y <= yLast; ++y)
		{
			for (int x = 0; x < map.getGridWidth(); ++x)
			{
				if ((x == x0 && y == y0) || plotDistance(x0, y0, x, y) > radius) continue;
				const CvPlot* plot = map.plot(x, y);
				if (!plot || !plot->isRevealed(team, false)) continue;
				if (!FindUnitPath(u, x, y, steps) || steps.size() < 2 || steps.back().turn >= horizon) continue;
				const PathStep& last = steps.back();
				const PathStep& parent = steps[steps.size() - 2];
				gcq_tree_node r;
				memset(&r, 0, sizeof(r));
				r.x = (int16_t)last.x;
				r.y = (int16_t)last.y;
				r.parent_x = (int16_t)parent.x;
				r.parent_y = (int16_t)parent.y;
				r.moves_left = Clamp16(last.moves);
				r.turn = (uint8_t)last.turn;
				r.flags = (uint16_t)PathStepFlags(u, map.plot(parent.x, parent.y), plot, last.moves, true);
				w.Add(&r);
			}
		}
	}
	if (!w.Finish(out))
	{
		msg = "out of memory";
		return GC_ERR_INTERNAL;
	}
	return GC_OK;
}

int32_t QueryCombatPreview(const int32_t* args, uint32_t argc, gc_buf* out, std::string& msg)
{
	if (argc != 4) return Invalid(msg, "Q_COMBAT_PREVIEW takes four arguments: player, attacker (unit id, or -city id), x, y");
	if (args[0] < 0 || args[0] >= MAX_PLAYERS) return Invalid(msg, "Q_COMBAT_PREVIEW: player out of range");
	CvMap& map = GC.getMap();
	if (args[2] < 0 || args[3] < 0 || args[2] >= map.getGridWidth() || args[3] >= map.getGridHeight()) return Invalid(msg, "Q_COMBAT_PREVIEW: the target plot is outside the map");
	CvPlayer& pl = GET_PLAYER((PlayerTypes)args[0]);
	CvUnit* u = NULL;
	CvCity* c = NULL;
	if (args[1] >= 0)
	{
		u = pl.getUnit(args[1]);
		if (!u) return Invalid(msg, "Q_COMBAT_PREVIEW: the player has no such unit");
	}
	else
	{
		if (args[1] < -0x7FFFFFFF) return Invalid(msg, "Q_COMBAT_PREVIEW: the player has no such city");
		c = pl.getCity(-args[1]);
		if (!c) return Invalid(msg, "Q_COMBAT_PREVIEW: the player has no such city");
	}
	CvPlot* plot = map.plot(args[2], args[3]);
	GcqBuf w(GC_Q_COMBAT_PREVIEW, sizeof(gcq_combat_preview));
	Preview p;
	// Only a plot the player's team sees now (no fog leak through the numbers).
	if (plot && plot->isVisible(pl.getTeam(), false) && (u ? PreviewUnit(u, plot, p) : PreviewCity(c, plot, p)))
	{
		gcq_combat_preview r;
		memset(&r, 0, sizeof(r));
		r.damage_to_target = Clamp16(p.damageToTarget);
		r.damage_to_attacker = Clamp16(p.damageToAttacker);
		r.attacker_strength = p.attackerStrength;
		r.defender_strength = p.defenderStrength;
		r.target_hp = Clamp16(p.targetHp);
		r.target_max_hp = Clamp16(p.targetMaxHp);
		r.flags = (uint16_t)p.flags;
		w.Add(&r);
	}
	if (!w.Finish(out))
	{
		msg = "out of memory";
		return GC_ERR_INTERNAL;
	}
	return GC_OK;
}

}  // namespace lekmod_portable
