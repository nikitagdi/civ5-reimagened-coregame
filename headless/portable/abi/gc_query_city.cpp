// Q_CITIES, Q_TOP_BAR, Q_CITY_BUILDS (record layouts in gamecore_abi.h). C++03.
// Everything here only reads the game (the top panel's gold and science go through the same treasury scope as the Lua getters,
// so a query never stores anything into the saved treasury fields).
#include "CvGameCoreDLLPCH.h"

#include "CvTreasury.h"
#include "gc_command.h"
#include "gc_gcq_writer.h"
#include "gc_query_city.h"

#include <algorithm>
#include <string.h>
#include <vector>

#ifndef HEADLESS_UI_TREASURY_SCOPE
#define HEADLESS_UI_TREASURY_SCOPE() do { } while (0)
#endif

namespace lekmod_portable {

namespace {

#define GCQ_ASSERT_SIZE(type, size) typedef char gcq_size_check_##type[(sizeof(type) == (size)) ? 1 : -1]
GCQ_ASSERT_SIZE(gcq_city, 56);  // v2 (P13): + founder
GCQ_ASSERT_SIZE(gcq_city_art, 12);
GCQ_ASSERT_SIZE(gcq_trade_route, 16);
GCQ_ASSERT_SIZE(gcq_top_bar, 60);
GCQ_ASSERT_SIZE(gcq_city_build, 24);

int32_t Invalid(std::string& msg, const std::string& text)
{
	msg = text;
	return GC_ERR_INVALID_ARG;
}

int32_t Oom(std::string& msg)
{
	msg = "out of memory";
	return GC_ERR_INTERNAL;
}

int16_t Clamp16(int v) { return (int16_t)(v < -32768 ? -32768 : (v > 32767 ? 32767 : v)); }

// Turns to finish: -1 = never (nothing produced, or the value is the core's "no end in sight").
int16_t TurnsOrNever(int turns, bool generating)
{
	if (!generating || turns < 0 || turns > 999) return -1;
	return (int16_t)turns;
}

// ---- Q_CITIES -------------------------------------------------------------------------------------------------

bool CityBefore(const CvCity* a, const CvCity* b)
{
	if (a->getOwner() != b->getOwner()) return a->getOwner() < b->getOwner();
	return a->GetID() < b->GetID();
}

int32_t QueryCities(int32_t viewer, gc_buf* out, std::string& msg)
{
	const bool all = viewer < 0;
	const TeamTypes team = all ? NO_TEAM : (TeamTypes)viewer;
	std::vector<CvCity*> cities;
	for (int i = 0; i < MAX_PLAYERS; ++i)
	{
		CvPlayer& pl = GET_PLAYER((PlayerTypes)i);
		if (!pl.isEverAlive()) continue;
		const bool own = all || (int)pl.getTeam() == viewer;
		int loop;
		for (CvCity* c = pl.firstCity(&loop); c; c = pl.nextCity(&loop))
		{
			if (!own)
			{
				const CvPlot* plot = c->plot();
				if (!plot || !plot->isRevealed(team, false)) continue;
				// Same rule as the Q_PLOTS city flag: seen now, or the owner we last saw on the plot is the city's.
				if (!plot->isVisible(team, false) && (int)plot->getRevealedOwner(team, false) != (int)c->getOwner()) continue;
			}
			cities.push_back(c);
		}
	}
	std::sort(cities.begin(), cities.end(), CityBefore);

	GcqBuf w(GC_Q_CITIES, sizeof(gcq_city));
	w.Reserve((uint32_t)cities.size());
	for (size_t k = 0; k < cities.size(); ++k)
	{
		CvCity* c = cities[k];
		const bool own = all || (int)c->getTeam() == viewer;
		const bool visible = own || c->plot()->isVisible(team, false);
		gcq_city r;
		memset(&r, 0, sizeof(r));
		r.id = c->GetID();
		r.owner = (int8_t)c->getOwner();
		r.production_order = -1;
		r.production_item = -1;
		r.production_turns = -1;
		r.growth_turns = -1;
		r.x = (int16_t)c->getX();
		r.y = (int16_t)c->getY();
		r.founder = -1;
		const CvString name = c->getName();
		r.name = w.Str(name.c_str());
		if (visible)
		{
			unsigned flags = GC_CITY_F_VISIBLE;
			r.founder = (int8_t)c->getOriginalOwner();
			if (c->isCapital()) flags |= GC_CITY_F_CAPITAL;
			if (c->IsRazing()) flags |= GC_CITY_F_RAZING;
			r.population = Clamp16(c->getPopulation());
			r.max_hp = Clamp16(c->GetMaxHitPoints());
			r.hp = Clamp16(c->GetMaxHitPoints() - c->getDamage());
			r.defense = Clamp16(c->getStrengthValue() / 100);
			if (own)
			{
				if (c->IsPuppet()) flags |= GC_CITY_F_PUPPET;
				if (c->IsOccupied()) flags |= GC_CITY_F_OCCUPIED;
				if (c->IsResistance()) flags |= GC_CITY_F_RESISTANCE;
				if (c->canRangeStrike() && c->CanRangeStrikeNow()) flags |= GC_CITY_F_CAN_RANGE_ATTACK;
				int order, item;
				if (CityHeadOrder(c, &order, &item))
				{
					r.production_order = (int8_t)order;
					r.production_item = (int16_t)item;
					if (order != GC_ORDER_MAINTAIN)
					{
						r.production_stored = c->getProduction();
						r.production_needed = c->getProductionNeeded();
						r.production_turns = TurnsOrNever(c->getProductionTurnsLeft(), c->getCurrentProductionDifferenceTimes100(false, false) > 0);
					}
				}
				r.growth_turns = TurnsOrNever(c->getFoodTurnsLeft(), c->foodDifference() > 0);
				r.yields[GC_YIELD_FOOD] = Clamp16(c->getYieldRate(YIELD_FOOD, false));
				r.yields[GC_YIELD_PRODUCTION] = Clamp16(c->getYieldRate(YIELD_PRODUCTION, false));
				r.yields[GC_YIELD_GOLD] = Clamp16(c->getYieldRate(YIELD_GOLD, false));
				r.yields[GC_YIELD_SCIENCE] = Clamp16(c->getYieldRate(YIELD_SCIENCE, false));
				r.yields[GC_YIELD_CULTURE] = Clamp16(c->getJONSCulturePerTurn());
				r.yields[GC_YIELD_FAITH] = Clamp16(c->GetFaithPerTurn());
			}
			r.flags = (uint16_t)flags;
		}
		w.Add(&r);
	}
	return w.Finish(out) ? GC_OK : Oom(msg);
}

// ---- Q_TOP_BAR ------------------------------------------------------------------------------------------------

}  // namespace

int32_t BuildTopBar(int player, gc_buf* out, std::string& msg)
{
	CvPlayerAI& pl = GET_PLAYER((PlayerTypes)player);
	gcq_top_bar r;
	memset(&r, 0, sizeof(r));
	{
		HEADLESS_UI_TREASURY_SCOPE();
		r.gold_per_turn = pl.calculateGoldRate();
		r.science_per_turn = pl.GetScience();
		r.research_tech = (int16_t)NO_TECH;
		r.research_turns = -1;
		const TechTypes tech = pl.GetPlayerTechs()->GetCurrentResearch();
		if (tech != NO_TECH)
		{
			r.research_tech = (int16_t)tech;
			r.research_turns = Clamp16(pl.GetPlayerTechs()->GetResearchTurnsLeft(tech, true));
			r.research_progress = pl.GetPlayerTechs()->GetResearchProgress(tech);
			r.research_cost = pl.GetPlayerTechs()->GetResearchCost(tech);
		}
	}
	r.gold = pl.GetTreasury()->GetGold();
	r.culture = pl.getJONSCulture();
	r.culture_per_turn = pl.GetTotalJONSCulturePerTurn();
	r.next_policy_cost = pl.getNextPolicyCost();
	r.happiness = Clamp16(pl.GetExcessHappiness());
	r.golden_age_turns = Clamp16(pl.getGoldenAgeTurns());
	r.faith = pl.GetFaith();
	r.faith_per_turn = pl.GetTotalFaithPerTurn();
	r.era = (int16_t)pl.GetCurrentEra();
	unsigned flags = 0;
	if (pl.IsEmpireUnhappy()) flags |= GC_TOPBAR_F_UNHAPPY;
	if (pl.IsEmpireVeryUnhappy()) flags |= GC_TOPBAR_F_VERY_UNHAPPY;
	if (pl.getGoldenAgeTurns() > 0) flags |= GC_TOPBAR_F_GOLDEN_AGE;
	r.flags = (uint16_t)flags;
	r.golden_age_progress = pl.GetGoldenAgeProgressMeter();
	r.golden_age_threshold = pl.GetGoldenAgeProgressThreshold();
	GcqBuf w(GC_Q_TOP_BAR, sizeof(r));
	w.Add(&r);
	return w.Finish(out) ? GC_OK : Oom(msg);
}

namespace {

// ---- Q_CITY_BUILDS --------------------------------------------------------------------------------------------

struct BuildCtx
{
	CvCity* city;
	bool generating;  // the city makes production at all (turns are meaningful)
	bool hasHead;
	int headOrder, headItem;
};

void AddBuild(GcqBuf& w, const BuildCtx& x, int order, int item, bool enabled, const CvString& tip, int turns, int productionCost, bool wonder)
{
	gcq_city_build r;
	memset(&r, 0, sizeof(r));
	r.order_type = (int8_t)order;
	r.item = (int16_t)item;
	r.turns = TurnsOrNever(turns, x.generating && order != GC_ORDER_MAINTAIN);
	r.production_cost = productionCost;
	unsigned flags = 0;
	if (wonder) flags |= GC_BUILD_F_WONDER;
	if (x.hasHead && x.headOrder == order && x.headItem == item) flags |= GC_BUILD_F_CURRENT;
	if (enabled) flags |= GC_BUILD_F_ENABLED;
	r.gold_cost = CityPurchaseCost(x.city, order, item, GC_YIELD_GOLD);
	r.faith_cost = CityPurchaseCost(x.city, order, item, GC_YIELD_FAITH);
	if (r.gold_cost >= 0)
	{
		std::string why;
		if (CheckCityPurchase(x.city, order, item, GC_YIELD_GOLD, why)) flags |= GC_BUILD_F_CAN_PURCHASE;
	}
	r.flags = (uint16_t)flags;
	if (enabled) r.reason = GCQ_NO_STRING;
	else
	{
		const std::string reason = PlainReason(tip.c_str());
		r.reason = w.Str(reason.empty() ? "cannot be started now" : reason.c_str());
	}
	w.Add(&r);
}

bool IsLimitedBuilding(int building)
{
	const CvBuildingEntry* e = GC.getBuildingInfo((BuildingTypes)building);
	if (!e) return false;
	const CvBuildingClassInfo* bc = GC.getBuildingClassInfo((BuildingClassTypes)e->GetBuildingClassType());
	return bc && (bc->getMaxGlobalInstances() > 0 || bc->getMaxPlayerInstances() == 1 || bc->getMaxTeamInstances() > 0);
}

int32_t QueryCityBuilds(int32_t player, int32_t cityId, gc_buf* out, std::string& msg)
{
	// v2: the own city of any living major civ; gc_query makes a seated human the active player for the call.
	if (player < 0 || player >= MAX_MAJOR_CIVS || !GET_PLAYER((PlayerTypes)player).isAlive()) return Invalid(msg, "Q_CITY_BUILDS: only the cities of a living major civilization can be asked");
	CvCity* city = GET_PLAYER((PlayerTypes)player).getCity(cityId);
	if (!city) return Invalid(msg, "Q_CITY_BUILDS: the player has no such city");

	BuildCtx x;
	x.city = city;
	x.generating = city->getCurrentProductionDifferenceTimes100(false, false) > 0;
	x.hasHead = CityHeadOrder(city, &x.headOrder, &x.headItem);
	GcqBuf w(GC_Q_CITY_BUILDS, sizeof(gcq_city_build));
	const CvString none;

	for (int i = 0; i < (int)GC.getNumUnitInfos(); ++i)
	{
		if (!city->canTrain((UnitTypes)i, false, true)) continue;  // "could train if ...": the popup's list
		const bool cur = x.hasHead && x.headOrder == GC_ORDER_TRAIN && x.headItem == i;
		CvString tip;
		// With a tooltip sink the core does not stop at a failed requirement (it only writes the text and carries on, e.g. the
		// settler size), so the sink call can say true for an item pushOrder refuses. The flag comes from the sink-less call (what
		// pushOrder asks); the sink call only fetches the text.
		const bool enabled = city->canTrain((UnitTypes)i, cur, false, false, false);
		if (!enabled) city->canTrain((UnitTypes)i, cur, false, false, false, &tip);
		AddBuild(w, x, GC_ORDER_TRAIN, i, enabled, tip, city->getProductionTurnsLeft((UnitTypes)i, 0), city->getProductionNeeded((UnitTypes)i), false);
	}
	for (int i = 0; i < (int)GC.getNumBuildingInfos(); ++i)
	{
		if (!city->canConstruct((BuildingTypes)i, false, true)) continue;
		const bool cur = x.hasHead && x.headOrder == GC_ORDER_CONSTRUCT && x.headItem == i;
		CvString tip;
		const bool enabled = city->canConstruct((BuildingTypes)i, cur, false, false);  // sink-less, see the unit loop
		if (!enabled) city->canConstruct((BuildingTypes)i, cur, false, false, &tip);
		AddBuild(w, x, GC_ORDER_CONSTRUCT, i, enabled, tip, city->getProductionTurnsLeft((BuildingTypes)i, 0), city->getProductionNeeded((BuildingTypes)i), IsLimitedBuilding(i));
	}
	for (int i = 0; i < (int)GC.getNumProjectInfos(); ++i)
	{
		if (!city->canCreate((ProjectTypes)i, false, true)) continue;
		const bool cur = x.hasHead && x.headOrder == GC_ORDER_CREATE && x.headItem == i;
		const bool enabled = city->canCreate((ProjectTypes)i, cur, false);
		AddBuild(w, x, GC_ORDER_CREATE, i, enabled, none, city->getProductionTurnsLeft((ProjectTypes)i, 0), city->getProductionNeeded((ProjectTypes)i), true);
	}
	for (int i = 0; i < (int)GC.getNumProcessInfos(); ++i)
	{
		if (!city->canMaintain((ProcessTypes)i)) continue;
		AddBuild(w, x, GC_ORDER_MAINTAIN, i, true, none, -1, 0, false);
	}
	return w.Finish(out) ? GC_OK : Oom(msg);
}

// ---- Q_CITY_ART (P13) -----------------------------------------------------------------------------------------

// The cities a viewer gets live data for: its own team's and the ones on a plot it sees now (all of them for viewer -1), ordered by owner and id.
void LiveCities(int32_t viewer, std::vector<CvCity*>& cities)
{
	const bool all = viewer < 0;
	const TeamTypes team = all ? NO_TEAM : (TeamTypes)viewer;
	for (int i = 0; i < MAX_PLAYERS; ++i)
	{
		CvPlayer& pl = GET_PLAYER((PlayerTypes)i);
		if (!pl.isEverAlive()) continue;
		const bool own = all || (int)pl.getTeam() == viewer;
		int loop;
		for (CvCity* c = pl.firstCity(&loop); c; c = pl.nextCity(&loop))
		{
			if (!own)
			{
				const CvPlot* plot = c->plot();
				if (!plot || !plot->isVisible(team, false)) continue;
			}
			cities.push_back(c);
		}
	}
	std::sort(cities.begin(), cities.end(), CityBefore);
}

int32_t QueryCityArt(int32_t viewer, gc_buf* out, std::string& msg)
{
	std::vector<CvCity*> cities;
	LiveCities(viewer, cities);
	// The buildings with art in the world: Buildings.ArtDefineTag set.
	const int n = GC.getNumBuildingInfos();
	std::vector<char> hasArt((size_t)n, 0);
	for (int b = 0; b < n; ++b)
	{
		const CvBuildingEntry* info = GC.getBuildingInfo((BuildingTypes)b);
		const char* tag = info ? info->GetArtDefineTag() : NULL;
		hasArt[(size_t)b] = (tag && tag[0]) ? 1 : 0;
	}
	GcqBuf w(GC_Q_CITY_ART, sizeof(gcq_city_art));
	for (size_t k = 0; k < cities.size(); ++k)
	{
		const CvCity* c = cities[k];
		const CvCityBuildings* buildings = c->GetCityBuildings();
		if (!buildings) continue;
		for (int b = 0; b < n; ++b)
		{
			if (!hasArt[(size_t)b] || buildings->GetNumBuilding((BuildingTypes)b) <= 0) continue;
			gcq_city_art r;
			memset(&r, 0, sizeof(r));
			r.city_id = c->GetID();
			r.owner = (int8_t)c->getOwner();
			r.building = (int16_t)b;
			r.x = (int16_t)c->getX();
			r.y = (int16_t)c->getY();
			w.Add(&r);
		}
	}
	return w.Finish(out) ? GC_OK : Oom(msg);
}

// ---- Q_TRADE_ROUTES (P13) -------------------------------------------------------------------------------------

// Does the viewer (a team, -1 = everyone) see the route: its team owns an end, or it sees a plot of the path now.
bool TradeRouteShown(const TradeConnection& route, int32_t viewer)
{
	if (viewer < 0) return true;
	const TeamTypes team = (TeamTypes)viewer;
	if (route.m_eOriginOwner >= 0 && (int)GET_PLAYER(route.m_eOriginOwner).getTeam() == viewer) return true;
	if (route.m_eDestOwner >= 0 && (int)GET_PLAYER(route.m_eDestOwner).getTeam() == viewer) return true;
	for (uint j = 0; j < route.m_aPlotList.size(); ++j)
	{
		const CvPlot* plot = GC.getMap().plot(route.m_aPlotList[j].m_iX, route.m_aPlotList[j].m_iY);
		if (plot && plot->isVisible(team, false)) return true;
	}
	return false;
}

// Q_TRADE_PATHS (B4 fix-path): the plot lists of the routes Q_TRADE_ROUTES lists, same filter and order.
int32_t QueryTradePaths(int32_t viewer, gc_buf* out, std::string& msg)
{
	CvGameTrade* trade = GC.getGame().GetGameTrade();
	GcqBuf w(GC_Q_TRADE_PATHS, sizeof(gcq_trade_plot));
	if (trade)
	{
		for (uint i = 0; i < trade->m_aTradeConnections.size(); ++i)
		{
			if (trade->IsTradeRouteIndexEmpty((int)i)) continue;
			const TradeConnection& route = trade->m_aTradeConnections[i];
			if (!TradeRouteShown(route, viewer)) continue;
			for (uint j = 0; j < route.m_aPlotList.size(); ++j)
			{
				gcq_trade_plot r;
				memset(&r, 0, sizeof(r));
				r.route = route.m_iID;
				r.x = (int16_t)route.m_aPlotList[j].m_iX;
				r.y = (int16_t)route.m_aPlotList[j].m_iY;
				w.Add(&r);
			}
		}
	}
	return w.Finish(out) ? GC_OK : Oom(msg);
}

int32_t QueryTradeRoutes(int32_t viewer, gc_buf* out, std::string& msg)
{
	CvGameTrade* trade = GC.getGame().GetGameTrade();
	GcqBuf w(GC_Q_TRADE_ROUTES, sizeof(gcq_trade_route));
	if (trade)
	{
		for (uint i = 0; i < trade->m_aTradeConnections.size(); ++i)
		{
			if (trade->IsTradeRouteIndexEmpty((int)i)) continue;
			const TradeConnection& route = trade->m_aTradeConnections[i];
			if (!TradeRouteShown(route, viewer)) continue;
			gcq_trade_route r;
			memset(&r, 0, sizeof(r));
			r.id = route.m_iID;
			r.owner = (int8_t)route.m_eOriginOwner;
			r.dest_owner = (int8_t)route.m_eDestOwner;
			r.domain = (int8_t)route.m_eDomain;
			r.from_x = (int16_t)route.m_iOriginX;
			r.from_y = (int16_t)route.m_iOriginY;
			r.to_x = (int16_t)route.m_iDestX;
			r.to_y = (int16_t)route.m_iDestY;
			w.Add(&r);
		}
	}
	return w.Finish(out) ? GC_OK : Oom(msg);
}

}  // namespace

int32_t RunCityQuery(int32_t kind, const int32_t* args, uint32_t argc, gc_buf* out, std::string& msg)
{
	switch (kind)
	{
	case GC_Q_CITIES:
		if (argc != 1) return Invalid(msg, "Q_CITIES takes one argument: viewer_team");
		if (args[0] < -1 || args[0] >= MAX_TEAMS) return Invalid(msg, "Q_CITIES: viewer_team out of range");
		return QueryCities(args[0], out, msg);
	case GC_Q_TOP_BAR:
	{
		if (argc != 1) return Invalid(msg, "Q_TOP_BAR takes one argument: player");
		// v2: any living major civ (v1: the human only). Not the server seat: it has no empire.
		if (args[0] < 0 || args[0] >= MAX_MAJOR_CIVS || !GET_PLAYER((PlayerTypes)args[0]).isAlive() || GET_PLAYER((PlayerTypes)args[0]).isObserver())
			return Invalid(msg, "Q_TOP_BAR: only a living major civilization has a top bar to read");
		return BuildTopBar(args[0], out, msg);
	}
	case GC_Q_CITY_BUILDS:
		if (argc != 2) return Invalid(msg, "Q_CITY_BUILDS takes two arguments: player, city");
		return QueryCityBuilds(args[0], args[1], out, msg);
	case GC_Q_CITY_ART:
		if (argc != 1) return Invalid(msg, "Q_CITY_ART takes one argument: viewer_team");
		if (args[0] < -1 || args[0] >= MAX_TEAMS) return Invalid(msg, "Q_CITY_ART: viewer_team out of range");
		return QueryCityArt(args[0], out, msg);
	case GC_Q_TRADE_ROUTES:
		if (argc != 1) return Invalid(msg, "Q_TRADE_ROUTES takes one argument: viewer_team");
		if (args[0] < -1 || args[0] >= MAX_TEAMS) return Invalid(msg, "Q_TRADE_ROUTES: viewer_team out of range");
		return QueryTradeRoutes(args[0], out, msg);
	case GC_Q_TRADE_PATHS:
		if (argc != 1) return Invalid(msg, "Q_TRADE_PATHS takes one argument: viewer_team");
		if (args[0] < -1 || args[0] >= MAX_TEAMS) return Invalid(msg, "Q_TRADE_PATHS: viewer_team out of range");
		return QueryTradePaths(args[0], out, msg);
	}
	return Invalid(msg, "not a city query");
}

}  // namespace lekmod_portable
