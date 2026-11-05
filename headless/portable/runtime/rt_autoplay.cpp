// rt_autoplay: the test bot in the human's seat (task F8; see rt_autoplay.h for the rules). New, after civhost's lockstep bot
// (headless/tools/civsim/lockstep/bot/bot.lua): it only plays through the ABI's human commands and queries. C++03.
#include "CvGameCoreDLLPCH.h"

#include "rt_autoplay.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "CvGameCoreUtils.h"
#include "../abi/gc_command.h"
#include "../abi/gc_query.h"
#include "../abi/gc_query_city.h"
#include "rt_active.h"
#include "rt_session.h"
#include "lekmod_portable/host_log.h"

namespace lekmod_portable {

namespace {

std::string Fmt(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string Fmt(const char* fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return buf;
}

// Records of one GCQ1 reply (a record may be longer than R: newer fields are skipped).
template <class R>
bool ReadReply(gc_buf& b, std::vector<R>& out)
{
	out.clear();
	if (!b.data || b.len < sizeof(gcq_header)) return false;
	gcq_header h;
	memcpy(&h, b.data, sizeof(h));
	if (memcmp(h.magic, "GCQ1", 4) != 0 || h.record_size == 0) return false;
	const size_t n = h.record_size < sizeof(R) ? h.record_size : sizeof(R);
	for (uint32_t i = 0; i < h.count; ++i)
	{
		R r;
		memset(&r, 0, sizeof(r));
		memcpy(&r, b.data + sizeof(h) + (size_t)i * h.record_size, n);
		out.push_back(r);
	}
	return true;
}

template <class R>
bool GameQuery(int32_t kind, const int32_t* args, uint32_t argc, std::vector<R>& out)
{
	gc_buf b = {0, 0, 0, 0};
	std::string msg;
	RtSession::BlockTarget none;
	if (RunGameQuery(kind, args, argc, -1, -1, none, &b, msg) != GC_OK) return false;
	const bool ok = ReadReply(b, out);
	gc_buf_release(&b);
	return ok;
}

// Q_UNIT_ACTIONS of one unit: bit k set = command kind k applies, enabled[k] = it can be given now.
struct Actions
{
	unsigned applies, enabled;
	Actions() : applies(0), enabled(0) {}
	bool Has(int kind) const { return (enabled >> kind) & 1u; }
	bool Applies(int kind) const { return (applies >> kind) & 1u; }
};

Actions UnitActions(int player, int unit)
{
	Actions a;
	std::vector<gcq_unit_action> recs;
	const int32_t args[2] = {player, unit};
	if (!GameQuery(GC_Q_UNIT_ACTIONS, args, 2, recs)) return a;
	for (size_t i = 0; i < recs.size(); ++i)
	{
		if (recs[i].action < 0 || recs[i].action >= 32) continue;
		a.applies |= 1u << recs[i].action;
		if (recs[i].enabled) a.enabled |= 1u << recs[i].action;
	}
	return a;
}

bool PathExists(int player, int unit, int x, int y)
{
	std::vector<gcq_path_node> nodes;
	const int32_t args[4] = {player, unit, x, y};
	// v2: a target without a path is one node with GC_REACH_F_UNREACHABLE, not an empty reply.
	return GameQuery(GC_Q_PATH, args, 4, nodes) && !nodes.empty() && !(nodes.back().flags & GC_REACH_F_UNREACHABLE);
}

const CvUnitEntry* UnitInfo(int type) { return (type >= 0 && type < GC.getNumUnitInfos()) ? GC.getUnitInfo((UnitTypes)type) : NULL; }

bool IsSettlerType(int type) { const CvUnitEntry* e = UnitInfo(type); return e && e->IsFound(); }
bool IsWorkerType(int type) { const CvUnitEntry* e = UnitInfo(type); return e && e->GetDefaultUnitAIType() == UNITAI_WORKER; }
bool IsLandMilitaryType(int type)
{
	const CvUnitEntry* e = UnitInfo(type);
	return e && e->GetDomainType() == DOMAIN_LAND && (e->GetCombat() > 0 || e->GetRangedCombat() > 0);
}

// An enemy (at war with the unit's team) the unit could attack this round: a ranged target in range, or (melee) an adjacent enemy unit
// it beats clearly. Read-only over the gamecore. kind = GC_CMD_UNIT_RANGE_ATTACK or GC_CMD_UNIT_MOVE_TO; false when none.
bool FindAttack(CvUnit* u, int* kind, int* tx, int* ty)
{
	if (!u || !u->IsCombatUnit() || u->isEmbarked() || u->getDomainType() != DOMAIN_LAND) return false;
	const TeamTypes team = u->getTeam();
	std::string why;
	if (u->IsCanAttackRanged())
	{
		const int r = u->GetRange();
		for (int dy = -r; dy <= r; ++dy)
			for (int dx = -r; dx <= r; ++dx)
			{
				CvPlot* p = plotXYWithRangeCheck(u->getX(), u->getY(), dx, dy, r);
				if (!p || !p->isVisible(team)) continue;
				const bool enemyCity = p->isCity() && GET_TEAM(team).isAtWar(p->getTeam());
				if (!enemyCity && !p->isVisibleEnemyUnit(u->getOwner())) continue;
				if (CheckUnitCommand(u, GC_CMD_UNIT_RANGE_ATTACK, true, p->getX(), p->getY(), why))
				{
					*kind = GC_CMD_UNIT_RANGE_ATTACK;
					*tx = p->getX();
					*ty = p->getY();
					return true;
				}
			}
		return false;
	}
	if (!u->IsCanAttack() || u->GetCurrHitPoints() * 10 < u->GetMaxHitPoints() * 6) return false;
	for (int d = 0; d < NUM_DIRECTION_TYPES; ++d)
	{
		CvPlot* p = plotDirection(u->getX(), u->getY(), (DirectionTypes)d);
		if (!p || p->isCity() || p->isWater() || !p->isVisible(team)) continue;
		CvUnit* def = p->getVisibleEnemyDefender(u->getOwner());
		if (!def) continue;
		const int att = u->GetMaxAttackStrength(u->plot(), p, def);
		const int dfn = def->GetMaxDefenseStrength(p, u);
		if (att * 10 < dfn * 12) continue;
		if (CheckUnitCommand(u, GC_CMD_UNIT_MOVE_TO, true, p->getX(), p->getY(), why))
		{
			*kind = GC_CMD_UNIT_MOVE_TO;
			*tx = p->getX();
			*ty = p->getY();
			return true;
		}
	}
	return false;
}

// Where a settler that cannot found here should go: the gamecore's own best settle plot when it has a path, else the nearest plot within
// kSettleRadius it can found on and reach (ties by y, then x). False when there is none.
bool FindSettlePlot(CvUnit* u, int* tx, int* ty)
{
	const PlayerTypes me = u->getOwner();
	// B4 gc-replica: GetBestSettlePlot fills the saved found-value cache of the plots it looks at (CvPlot::getFoundValue). The bot runs on the
	// server only, so it puts the cache back: what it leaves in the game are its commands, which the journal carries to the replicas.
	CvMap& map = GC.getMap();
	std::vector<int> found(map.numPlots());
	for (int i = 0; i < map.numPlots(); ++i) found[i] = map.plotByIndexUnchecked(i)->getFoundValueCached(me);
	CvPlot* best = GET_PLAYER(me).GetBestSettlePlot(u, false);
	for (int i = 0; i < map.numPlots(); ++i) map.plotByIndexUnchecked(i)->setFoundValue(me, found[i]);
	if (best && !(best->getX() == u->getX() && best->getY() == u->getY()) && PathExists((int)me, u->GetID(), best->getX(), best->getY()))
	{
		*tx = best->getX();
		*ty = best->getY();
		return true;
	}
	const TeamTypes team = u->getTeam();
	const int r = RtAutoplay::kSettleRadius;
	int tried = 0;
	for (int dist = 1; dist <= r && tried < 8; ++dist)
		for (int dy = -dist; dy <= dist && tried < 8; ++dy)
			for (int dx = -dist; dx <= dist && tried < 8; ++dx)
			{
				CvPlot* p = plotXYWithRangeCheck(u->getX(), u->getY(), dx, dy, dist);
				if (!p || plotDistance(u->getX(), u->getY(), p->getX(), p->getY()) != dist) continue;
				if (!p->isRevealed(team) || (p->getOwner() != NO_PLAYER && p->getOwner() != me) || !u->canFound(p)) continue;
				++tried;
				if (PathExists((int)me, u->GetID(), p->getX(), p->getY()))
				{
					*tx = p->getX();
					*ty = p->getY();
					return true;
				}
			}
	return false;
}

}  // namespace

RtAutoplayStats::RtAutoplayStats()
	: turns(0), rounds(0), commands(0), rejected(0), forced(0), founded(0), attacks(0), productions(0), maxRoundsPerTurn(0), retaken(0)
{
	memset(byKind, 0, sizeof(byKind));
}

struct RtAutoplay::Ctx
{
	int me;
	int team;
	int turn;
	std::vector<gcq_unit> units;    // own units (Q_UNITS of the human's team, owner = the human)
	std::vector<gcq_city> cities;   // own cities
	int settlers, workers, military;
};

RtAutoplay::RtAutoplay() : m_turn(-1), m_endedTurn(-1), m_roundsThisTurn(0), m_idleRounds(0), m_wokeThisTurn(false) {}

void RtAutoplay::NewTurn(int turn)
{
	m_turn = turn;
	m_roundsThisTurn = 0;
	m_idleRounds = 0;
	m_wokeThisTurn = false;
	m_ordersThisTurn.clear();
}

bool RtAutoplay::Command(RtSession& s, int kind, int a0, int a1, int a2, int argc, int a3)
{
	const int32_t args[4] = {a0, a1, a2, a3};
	gc_command cmd;
	memset(&cmd, 0, sizeof(cmd));
	cmd.kind = (uint16_t)kind;
	cmd.player = (uint8_t)s.Engine().GetActivePlayer();
	cmd.argc = (uint32_t)argc;
	cmd.args = argc > 0 ? args : NULL;
	std::string msg;
	const int32_t rc = ApplyHumanCommand(s, cmd, msg);
	if (rc != GC_OK)
	{
		++m_stats.rejected;
		if (m_stats.firstReject.empty()) m_stats.firstReject = Fmt("turn %d command %d: %s", m_turn, kind, msg.c_str());
		Log(LOG_DEBUG, "autoplay: turn %d: command %d refused: %s", m_turn, kind, msg.c_str());
		return false;
	}
	++m_stats.commands;
	if (kind >= 0 && kind < 48) ++m_stats.byKind[kind];
	return true;
}

int RtAutoplay::SetProductions(RtSession& s, Ctx& c)
{
	int issued = 0;
	for (size_t i = 0; i < c.cities.size(); ++i)
	{
		const gcq_city& city = c.cities[i];
		if (city.production_order >= 0 || (city.flags & GC_CITY_F_PUPPET)) continue;
		std::vector<gcq_city_build> builds;
		{
			gc_buf b = {0, 0, 0, 0};
			std::string msg;
			const int32_t args[2] = {c.me, city.id};
			if (RunCityQuery(GC_Q_CITY_BUILDS, args, 2, &b, msg) != GC_OK) continue;
			ReadReply(b, builds);
			gc_buf_release(&b);
		}
		// The rule of rt_autoplay.h step 1; -1 = not found yet.
		int pick = -1, pickMil = -1, pickBld = -1, pickWonder = -1, pickUnit = -1, pickProc = -1;
		const int cities = (int)c.cities.size();
		for (size_t k = 0; k < builds.size(); ++k)
		{
			const gcq_city_build& b = builds[k];
			if (!(b.flags & GC_BUILD_F_ENABLED)) continue;
			if (b.order_type == GC_ORDER_TRAIN)
			{
				if (IsSettlerType(b.item) && cities + c.settlers < kTargetCities && pick < 0) pick = (int)k;
				else if (IsWorkerType(b.item) && c.workers < cities && pick < 0) pick = (int)k;
				if (IsLandMilitaryType(b.item) && (pickMil < 0 || b.production_cost > builds[pickMil].production_cost)) pickMil = (int)k;
				if (pickUnit < 0 && !IsSettlerType(b.item)) pickUnit = (int)k;
			}
			else if (b.order_type == GC_ORDER_CONSTRUCT || b.order_type == GC_ORDER_CREATE)
			{
				int& slot = (b.flags & GC_BUILD_F_WONDER) ? pickWonder : pickBld;
				if (slot < 0 || b.production_cost < builds[slot].production_cost) slot = (int)k;
			}
			else if (b.order_type == GC_ORDER_MAINTAIN && pickProc < 0)
				pickProc = (int)k;
		}
		if (pick < 0 && pickMil >= 0 && c.military < 2 * cities) pick = pickMil;
		if (pick < 0) pick = pickBld;
		if (pick < 0) pick = pickWonder;
		if (pick < 0) pick = pickUnit;
		if (pick < 0) pick = pickProc;
		if (pick < 0) continue;
		const gcq_city_build& b = builds[pick];
		if (Command(s, GC_CMD_CITY_SET_PRODUCTION, city.id, b.order_type, b.item, 3))
		{
			++issued;
			++m_stats.productions;
			if (b.order_type == GC_ORDER_TRAIN)
			{
				if (IsSettlerType(b.item)) ++c.settlers;
				else if (IsWorkerType(b.item)) ++c.workers;
				else if (IsLandMilitaryType(b.item)) ++c.military;
			}
		}
	}
	return issued;
}

int RtAutoplay::WakeForAttacks(RtSession& s, Ctx& c)
{
	int issued = 0;
	CvPlayer& p = GET_PLAYER((PlayerTypes)c.me);
	for (size_t i = 0; i < c.units.size(); ++i)
	{
		const gcq_unit& r = c.units[i];
		if (!(r.flags & (GC_UNIT_F_FORTIFIED | GC_UNIT_F_SLEEPING)) || !(r.flags & GC_UNIT_F_CAN_MOVE) || (r.flags & GC_UNIT_F_CIVILIAN)) continue;
		CvUnit* u = p.getUnit(r.id);
		int kind, x, y;
		if (!u || !FindAttack(u, &kind, &x, &y)) continue;
		if (UnitActions(c.me, r.id).Has(GC_CMD_UNIT_WAKE) && Command(s, GC_CMD_UNIT_WAKE, r.id, 0, 0, 1)) ++issued;
	}
	return issued;
}

int RtAutoplay::OrderUnits(RtSession& s, Ctx& c)
{
	int issued = 0;
	CvPlayer& p = GET_PLAYER((PlayerTypes)c.me);
	for (size_t i = 0; i < c.units.size(); ++i)
	{
		const gcq_unit& r = c.units[i];
		if (!(r.flags & GC_UNIT_F_NEEDS_ORDERS) || !(r.flags & GC_UNIT_F_CAN_MOVE)) continue;
		int& orders = m_ordersThisTurn[r.id];
		if (orders >= 2 * kMaxOrdersPerUnit) continue;  // left to FORCE_END_TURN
		++orders;
		CvUnit* u = p.getUnit(r.id);
		if (!u) continue;
		const Actions a = UnitActions(c.me, r.id);
		const int id = r.id;
		bool done = false;
		if (orders > kMaxOrdersPerUnit)
		{
			done = a.Has(GC_CMD_UNIT_SKIP) && Command(s, GC_CMD_UNIT_SKIP, id, 0, 0, 1);
			if (done) ++issued;
			continue;
		}
		const bool settler = IsSettlerType(r.unit_type);
		int kind = 0, x = -1, y = -1;
		if (settler)
		{
			if (a.Has(GC_CMD_UNIT_FOUND_CITY) && Command(s, GC_CMD_UNIT_FOUND_CITY, id, 0, 0, 1)) { done = true; ++m_stats.founded; }
			else if (a.Has(GC_CMD_UNIT_MOVE_TO) && FindSettlePlot(u, &x, &y)) done = Command(s, GC_CMD_UNIT_MOVE_TO, id, x, y, 3);
		}
		else if (a.Applies(GC_CMD_UNIT_AUTOMATE_BUILD))
			done = a.Has(GC_CMD_UNIT_AUTOMATE_BUILD) && Command(s, GC_CMD_UNIT_AUTOMATE_BUILD, id, 0, 0, 1);
		else if (UnitInfo(r.unit_type) && UnitInfo(r.unit_type)->GetDefaultUnitAIType() == UNITAI_EXPLORE && a.Has(GC_CMD_UNIT_AUTOMATE_EXPLORE))
			done = Command(s, GC_CMD_UNIT_AUTOMATE_EXPLORE, id, 0, 0, 1);
		if (!done && !settler && !(r.flags & GC_UNIT_F_CIVILIAN))
		{
			if (FindAttack(u, &kind, &x, &y) && a.Has(kind) && Command(s, kind, id, x, y, 3)) { done = true; ++m_stats.attacks; }
			else if (r.hp * 2 < r.max_hp && a.Has(GC_CMD_UNIT_HEAL)) done = Command(s, GC_CMD_UNIT_HEAL, id, 0, 0, 1);
			if (!done && a.Has(GC_CMD_UNIT_FORTIFY)) done = Command(s, GC_CMD_UNIT_FORTIFY, id, 0, 0, 1);
		}
		if (!done && !settler && a.Has(GC_CMD_UNIT_SLEEP)) done = Command(s, GC_CMD_UNIT_SLEEP, id, 0, 0, 1);
		if (!done && a.Has(GC_CMD_UNIT_SKIP)) done = Command(s, GC_CMD_UNIT_SKIP, id, 0, 0, 1);
		if (!done && a.Has(GC_CMD_UNIT_MOVE_TO))
		{
			// STACKED_UNITS: nothing may stay here, so step aside.
			for (int d = 0; d < NUM_DIRECTION_TYPES && !done; ++d)
			{
				CvPlot* to = plotDirection(u->getX(), u->getY(), (DirectionTypes)d);
				std::string why;
				if (to && u->canMoveInto(*to) && CheckUnitCommand(u, GC_CMD_UNIT_MOVE_TO, true, to->getX(), to->getY(), why))
					done = Command(s, GC_CMD_UNIT_MOVE_TO, id, to->getX(), to->getY(), 3);
			}
		}
		if (!done && u->plot() && !u->canHold(u->plot()))
		{
			// STACKED_UNITS and no free neighbour to step aside to (N3.5, mp seed 1 with 3 humans): exploring takes the unit off the plot, else it is disbanded.
			if (a.Has(GC_CMD_UNIT_AUTOMATE_EXPLORE)) done = Command(s, GC_CMD_UNIT_AUTOMATE_EXPLORE, id, 0, 0, 1);
			if (!done && a.Has(GC_CMD_UNIT_DISBAND)) done = Command(s, GC_CMD_UNIT_DISBAND, id, 0, 0, 1);
		}
		if (done) ++issued;
	}
	return issued;
}

bool RtAutoplay::ActAs(RtSession& s, int player, std::string& why)
{
	ActiveAs as((PlayerTypes)player);
	return Act(s, why);
}

bool RtAutoplay::Act(RtSession& s, std::string& why)
{
	Ctx c;
	c.me = (int)s.Engine().GetActivePlayer();
	if (c.me < 0 || c.me >= MAX_PLAYERS || !GET_PLAYER((PlayerTypes)c.me).isHuman())
	{
		why = "autoplay: there is no human player to play";
		return false;
	}
	c.team = (int)GET_PLAYER((PlayerTypes)c.me).getTeam();
	c.turn = GC.getGame().getGameTurn();
	if (c.turn != m_turn) NewTurn(c.turn);
	++m_stats.rounds;
	if (++m_roundsThisTurn > m_stats.maxRoundsPerTurn) m_stats.maxRoundsPerTurn = m_roundsThisTurn;
	RtSession::BlockTarget target;
	const int blocking = s.CurrentBlocking(&target, (PlayerTypes)c.me);  // this human's blocker (N3.14: not the lead human's, which CurrentBlocking(&target) means in a network game)
	if (m_endedTurn == c.turn)
	{
		// The turn was ended and the gamecore took it back (sendTurnUnready from CvGame::UpdateMoves: a unit is ready again).
		m_endedTurn = -1;
		++m_stats.retaken;
		if (m_stats.firstRetaken.empty())
		{
			const CvUnit* u = GET_PLAYER((PlayerTypes)c.me).GetFirstReadyUnit();
			m_stats.firstRetaken = Fmt("turn %d: blocker %s, first ready unit %s", c.turn, RtSession::BlockingName(blocking),
				u ? Fmt("%d %s at (%d,%d) automated=%d", u->GetID(), u->getUnitInfo().GetType(), u->getX(), u->getY(), u->IsAutomated() ? 1 : 0).c_str() : "none");
		}
	}
	if (m_roundsThisTurn > kMaxRounds)
	{
		why = Fmt("autoplay: turn %d: the human's turn did not end after %d bot rounds (blocker %s)", c.turn, kMaxRounds, RtSession::BlockingName(blocking));
		return false;
	}

	{
		std::vector<gcq_unit> all;
		const int32_t viewer = c.team;
		GameQuery(GC_Q_UNITS, &viewer, 1, all);
		c.settlers = c.workers = c.military = 0;
		for (size_t i = 0; i < all.size(); ++i)
		{
			if (all[i].owner != c.me) continue;
			c.units.push_back(all[i]);
			if (IsSettlerType(all[i].unit_type)) ++c.settlers;
			else if (IsWorkerType(all[i].unit_type)) ++c.workers;
			else if (IsLandMilitaryType(all[i].unit_type)) ++c.military;
		}
		std::vector<gcq_city> cities;
		gc_buf b = {0, 0, 0, 0};
		std::string msg;
		if (RunCityQuery(GC_Q_CITIES, &viewer, 1, &b, msg) == GC_OK)
		{
			ReadReply(b, cities);
			gc_buf_release(&b);
		}
		for (size_t i = 0; i < cities.size(); ++i)
			if (cities[i].owner == c.me) c.cities.push_back(cities[i]);
	}

	int issued = SetProductions(s, c);
	if (!m_wokeThisTurn)
	{
		m_wokeThisTurn = true;
		issued += WakeForAttacks(s, c);
	}
	issued += OrderUnits(s, c);
	if (issued > 0)
	{
		m_idleRounds = 0;
		return true;
	}

	// Nothing to order: end the turn, or force it over units the rounds could not settle.
	if (blocking == NO_ENDTURN_BLOCKING_TYPE)
	{
		if (Command(s, GC_CMD_END_TURN, 0, 0, 0, 0)) { ++m_stats.turns; m_endedTurn = c.turn; return true; }
	}
	else if (blocking == ENDTURN_BLOCKING_UNITS || blocking == ENDTURN_BLOCKING_STACKED_UNITS)
	{
		if (Command(s, GC_CMD_FORCE_END_TURN, 0, 0, 0, 0)) { ++m_stats.turns; ++m_stats.forced; m_endedTurn = c.turn; return true; }
	}
	if (++m_idleRounds > kMaxIdleRounds)
	{
		std::string unitInfo;
		if (target.kind == RtSession::TARGET_UNIT)
		{
			const CvUnit* tu = GET_PLAYER((PlayerTypes)c.me).getUnit(target.id);
			if (tu)
			{
				const Actions a = UnitActions(c.me, target.id);
				unitInfo = Fmt("; unit %s: moves %d, canMove %d, mission queue %d, actions enabled 0x%x applies 0x%x, %d unit(s) on the plot, can hold %d", tu->getUnitInfo().GetType(), tu->movesLeft(),
					tu->canMove() ? 1 : 0, tu->GetLengthMissionQueue(), a.enabled, a.applies, tu->plot() ? tu->plot()->getNumUnits() : 0, tu->plot() && tu->canHold(tu->plot()) ? 1 : 0);
			}
		}
		why = Fmt("autoplay: turn %d: %d bot rounds in a row without a command; blocker %s (target %s %d at (%d,%d))%s", c.turn, kMaxIdleRounds,
			RtSession::BlockingName(blocking), RtSession::TargetKindName(target.kind), target.id, target.x, target.y, unitInfo.c_str());
		return false;
	}
	return true;
}

}  // namespace lekmod_portable
