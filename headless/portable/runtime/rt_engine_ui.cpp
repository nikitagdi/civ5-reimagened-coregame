// B4 M2 WO-1: the Gameplay* / Publish* callbacks of ICvEngineUtility4 that a replica's UI is told. Each one packs what the exe reads at the moment
// of the callback into a gc_ui_event (rt_ui_replica.h); without the replica role m_uiRep is idle and every callback is what the generated stub was (nothing).
#include "CvGameCoreDLLPCH.h"

#include "CvDllCity.h"
#include "CvDllPlot.h"
#include "CvDllUnit.h"

#include "rt_active.h"
#include "rt_engine.h"
#include "rt_ui_replica.h"

namespace lekmod_portable {

namespace {

inline bool On(RtUiReplica* r) { return r && r->Active(); }

void Plain(RtUiReplica* r, int kind)
{
	if (!On(r)) return;
	RtUiEv ev(kind);
	r->Send(ev);
}

// kind, unit [, a [, b [, c [, d]]]]
void UnitEv(RtUiReplica* r, int kind, ICvUnit1* unit, int a = 0, int na = 0, int b = 0, int nb = 0)
{
	if (!On(r)) return;
	RtUiEv ev(kind);
	r->PackUnit(ev, unit);
	if (na >= 1) ev.A(a);
	if (nb >= 1) ev.A(b);
	r->Send(ev);
}

void CityEv(RtUiReplica* r, int kind, ICvCity1* city, int a = 0, int na = 0, int b = 0, int nb = 0)
{
	if (!On(r)) return;
	RtUiEv ev(kind);
	r->PackCity(ev, city);
	if (na >= 1) ev.A(a);
	if (nb >= 1) ev.A(b);
	r->Send(ev);
}

void PlotEv(RtUiReplica* r, int kind, const ICvPlot1* plot)
{
	if (!On(r)) return;
	RtUiEv ev(kind);
	int x, y;
	RtUiReplica::PlotXY(plot, &x, &y);
	ev.A(x).A(y);
	r->Send(ev);
}

}  // namespace

void RtEngine::GameplayEraChanged(PlayerTypes playerID, EraTypes newEra)
{
	if (!On(m_uiRep)) return;
	RtUiEv ev(GC_UI_EV_ERA_CHANGED);
	ev.A((int)playerID).A((int)newEra);
	m_uiRep->Send(ev);
}

void RtEngine::GameplayBridgeChanged(bool bBridge, unsigned char newBridgeStyle)
{
	if (!On(m_uiRep)) return;
	RtUiEv ev(GC_UI_EV_BRIDGE_CHANGED);
	ev.A(bBridge ? 1 : 0).A((int)newBridgeStyle);
	m_uiRep->Send(ev);
}

void RtEngine::GameplayUnitCreated(ICvUnit1* pThisUnit) { UnitEv(m_uiRep, GC_UI_EV_UNIT_CREATED, pThisUnit); }

void RtEngine::GameplayUnitMoved(ICvUnit1* pThisUnit, const CvPlotIndexVector& kPlots)
{
	if (!On(m_uiRep)) return;
	RtUiEv ev(GC_UI_EV_UNIT_MOVED);
	m_uiRep->PackUnit(ev, pThisUnit);
	// The path: u32 plot indices, little endian.
	std::vector<uint8_t> path;
	path.reserve(kPlots.size() * 4);
	for (size_t i = 0; i < kPlots.size(); ++i)
	{
		const uint32_t v = (uint32_t)kPlots[i];
		path.push_back((uint8_t)(v & 0xFF));
		path.push_back((uint8_t)((v >> 8) & 0xFF));
		path.push_back((uint8_t)((v >> 16) & 0xFF));
		path.push_back((uint8_t)((v >> 24) & 0xFF));
	}
	if (!path.empty()) ev.D(&path[0], path.size());
	m_uiRep->Send(ev);
}

void RtEngine::GameplayUnitTeleported(ICvUnit1* pThisUnit, ICvPlot1* pThisPlot)
{
	if (!On(m_uiRep)) return;
	RtUiEv ev(GC_UI_EV_UNIT_TELEPORTED);
	m_uiRep->PackUnit(ev, pThisUnit);
	int x, y;
	RtUiReplica::PlotXY(pThisPlot, &x, &y);
	ev.A(x).A(y);
	m_uiRep->Send(ev);
}

void RtEngine::GameplayUnitDestroyed(ICvUnit1* pThisUnit) { UnitEv(m_uiRep, GC_UI_EV_UNIT_DESTROYED, pThisUnit); }
void RtEngine::GameplayUnitDestroyedInCombat(ICvUnit1* pUnit) { UnitEv(m_uiRep, GC_UI_EV_UNIT_DESTROYED_IN_COMBAT, pUnit); }
void RtEngine::GameplayUnitSetDamage(ICvUnit1* pThisUnit, int iDamage, int iPreviousDamage) { UnitEv(m_uiRep, GC_UI_EV_UNIT_SET_DAMAGE, pThisUnit, iDamage, 1, iPreviousDamage, 1); }
void RtEngine::GameplayUnitFortify(ICvUnit1* pThisUnit, bool bFortify) { UnitEv(m_uiRep, GC_UI_EV_UNIT_FORTIFY, pThisUnit, bFortify ? 1 : 0, 1); }
void RtEngine::GameplayUnitWork(ICvUnit1* pThisUnit, int iWorkType) { UnitEv(m_uiRep, GC_UI_EV_UNIT_WORK, pThisUnit, iWorkType, 1); }
void RtEngine::GameplayUnitParadrop(ICvUnit1* pThisUnit) { UnitEv(m_uiRep, GC_UI_EV_UNIT_PARADROP, pThisUnit); }
void RtEngine::GameplayUnitActivate(ICvUnit1* pThisUnit) { UnitEv(m_uiRep, GC_UI_EV_UNIT_ACTIVATE, pThisUnit); }
void RtEngine::GameplayUnitEmbark(ICvUnit1* pThisUnit, bool bEmbark) { UnitEv(m_uiRep, GC_UI_EV_UNIT_EMBARK, pThisUnit, bEmbark ? 1 : 0, 1); }
void RtEngine::GameplayUnitGarrison(ICvUnit1* pThisUnit, bool bGarrison) { UnitEv(m_uiRep, GC_UI_EV_UNIT_GARRISON, pThisUnit, bGarrison ? 1 : 0, 1); }
void RtEngine::GameplayUnitShouldDimFlag(ICvUnit1* pThisUnit, bool bDim) { UnitEv(m_uiRep, GC_UI_EV_UNIT_SHOULD_DIM_FLAG, pThisUnit, bDim ? 1 : 0, 1); }
void RtEngine::GameplayUnitMarkThreatening(ICvUnit1* pThisUnit, bool bMark) { UnitEv(m_uiRep, GC_UI_EV_UNIT_MARK_THREATENING, pThisUnit, bMark ? 1 : 0, 1); }

void RtEngine::GameplayUnitVisibility(ICvUnit1* pThisUnit, bool bVisible, bool bChangeInvisibility, const float fBlendTime)
{
	if (!On(m_uiRep)) return;
	RtUiEv ev(GC_UI_EV_UNIT_VISIBILITY);
	m_uiRep->PackUnit(ev, pThisUnit);
	ev.A(bVisible ? 1 : 0).A(bChangeInvisibility ? 1 : 0).A((int)(fBlendTime * 1000.0f));
	m_uiRep->Send(ev);
}

uint RtEngine::GameplayUnitCombat(ICvCombatInfo1* pCombatInfo)
{
	if (On(m_uiRep))
	{
		RtUiEv ev(GC_UI_EV_UNIT_COMBAT);
		m_uiRep->PackCombat(ev, pCombatInfo);
		m_uiRep->Send(ev);
	}
	return 0;
}

void RtEngine::GameplayCityCreated(ICvCity1* pThisCity, EraTypes eCurrentEra) { CityEv(m_uiRep, GC_UI_EV_CITY_CREATED, pThisCity, (int)eCurrentEra, 1); }
void RtEngine::GameplayCityDestroyed(ICvCity1* pThisCity, PlayerTypes eNewOwner) { CityEv(m_uiRep, GC_UI_EV_CITY_DESTROYED, pThisCity, (int)eNewOwner, 1); }
void RtEngine::GameplayCityCaptured(ICvCity1* pThisCity, PlayerTypes eNewOwner) { CityEv(m_uiRep, GC_UI_EV_CITY_CAPTURED, pThisCity, (int)eNewOwner, 1); }
void RtEngine::GameplayCitySetDamage(ICvCity1* pThisCity, int iDamage, int iPreviousDamage) { CityEv(m_uiRep, GC_UI_EV_CITY_SET_DAMAGE, pThisCity, iDamage, 1, iPreviousDamage, 1); }
void RtEngine::GameplayCityPopulationChanged(ICvCity1* pThisCity, int newPopulation) { CityEv(m_uiRep, GC_UI_EV_CITY_POPULATION_CHANGED, pThisCity, newPopulation, 1); }

uint RtEngine::GameplayCityCombat(ICvCombatInfo1* pInfo)
{
	if (On(m_uiRep))
	{
		RtUiEv ev(GC_UI_EV_CITY_COMBAT);
		m_uiRep->PackCombat(ev, pInfo);
		m_uiRep->Send(ev);
	}
	return 0;
}

void RtEngine::GameplayUnitMissionEnd(ICvUnit1* pThisUnit) { UnitEv(m_uiRep, GC_UI_EV_UNIT_MISSION_END, pThisUnit); }

void RtEngine::GameplayUnitRebased(ICvUnit1* pThisUnit, ICvPlot1* pOldPlot, ICvPlot1* pThisPlot)
{
	if (!On(m_uiRep)) return;
	RtUiEv ev(GC_UI_EV_UNIT_REBASED);
	m_uiRep->PackUnit(ev, pThisUnit);
	int ox, oy, nx, ny;
	RtUiReplica::PlotXY(pOldPlot, &ox, &oy);
	RtUiReplica::PlotXY(pThisPlot, &nx, &ny);
	ev.A(ox).A(oy).A(nx).A(ny);
	m_uiRep->Send(ev);
}

void RtEngine::GameplayUnitResetAnimationState(ICvUnit1* pThisUnit) { UnitEv(m_uiRep, GC_UI_EV_UNIT_RESET_ANIMATION, pThisUnit); }

void RtEngine::GameplayTechAcquired(TeamTypes eTeam, TechTypes eTech)
{
	if (!On(m_uiRep)) return;
	RtUiEv ev(GC_UI_EV_TECH_ACQUIRED);
	ev.A((int)eTeam).A((int)eTech);
	m_uiRep->Send(ev);
}

void RtEngine::GameplayFeatureChanged(ICvPlot1* pThisPlot, FeatureTypes newFeature)
{
	if (!On(m_uiRep)) return;
	RtUiEv ev(GC_UI_EV_FEATURE_CHANGED);
	int x, y;
	RtUiReplica::PlotXY(pThisPlot, &x, &y);
	ev.A(x).A(y).A((int)newFeature);
	m_uiRep->Send(ev);
}

void RtEngine::GameplayPlotStateChange(const ICvPlot1* pThisPlot, const ResourceTypes eRevealedResource, const ImprovementTypes eRevealedImprovement, const int iRevealedImprovementState, const RouteTypes eRevealedRoute, const int iRouteState)
{
	if (!On(m_uiRep)) return;
	RtUiEv ev(GC_UI_EV_PLOT_STATE_CHANGE);
	int x, y;
	RtUiReplica::PlotXY(pThisPlot, &x, &y);
	ev.A(x).A(y).A((int)eRevealedResource).A((int)eRevealedImprovement).A(iRevealedImprovementState).A((int)eRevealedRoute).A(iRouteState);
	m_uiRep->Send(ev);
}

void RtEngine::GameplayPlotIconStateChange(const ICvPlot1* pThisPlot, const ResourceTypes eRevealedResource, const ImprovementTypes eRevealedImprovement, const int iRevealedImprovementState)
{
	if (!On(m_uiRep)) return;
	RtUiEv ev(GC_UI_EV_PLOT_ICON_STATE_CHANGE);
	int x, y;
	RtUiReplica::PlotXY(pThisPlot, &x, &y);
	ev.A(x).A(y).A((int)eRevealedResource).A((int)eRevealedImprovement).A(iRevealedImprovementState);
	m_uiRep->Send(ev);
}

void RtEngine::GameplayNaturalWonderRevealed(ICvPlot1* pThisPlot) { PlotEv(m_uiRep, GC_UI_EV_NATURAL_WONDER_REVEALED, pThisPlot); }
void RtEngine::GameplayWallCreated(ICvPlot1* pThisPlot) { PlotEv(m_uiRep, GC_UI_EV_WALL_CREATED, pThisPlot); }

void RtEngine::GameplayDoFX(ICvPlot1* pThisPlot, int iFXType)
{
	if (!On(m_uiRep)) return;
	RtUiEv ev(GC_UI_EV_DO_FX);
	int x, y;
	RtUiReplica::PlotXY(pThisPlot, &x, &y);
	ev.A(x).A(y).A(iFXType);
	m_uiRep->Send(ev);
}

// The DLL works the fog out for the active team, which during a replay is the server seat's: the UI's own team is compared after each batch instead (RtUiReplica::SyncFog).
void RtEngine::GameplayFOWChanged(int, int, int, bool) {}

void RtEngine::GameplayYieldMightHaveChanged(ICvPlot1* pThisPlot) { PlotEv(m_uiRep, GC_UI_EV_YIELD_MIGHT_HAVE_CHANGED, pThisPlot); }

void RtEngine::NotifyAILeadersInGame() { Plain(m_uiRep, GC_UI_EV_NOTIFY_AI_LEADERS_IN_GAME); }
void RtEngine::DoClearDiplomacyTradeTable() { Plain(m_uiRep, GC_UI_EV_CLEAR_DIPLO_TRADE_TABLE); }

void RtEngine::GameplayMetTeam(TeamTypes eMyTeam, TeamTypes eTheirTeam)
{
	if (!On(m_uiRep)) return;
	RtUiEv ev(GC_UI_EV_MET_TEAM);
	ev.A((int)eMyTeam).A((int)eTheirTeam);
	m_uiRep->Send(ev);
}

void RtEngine::GameplayWonderCreated(PlayerTypes ePlayerID, ICvPlot1* pThisPlot, BuildingTypes eBuildingIndex, int iState)
{
	if (!On(m_uiRep)) return;
	RtUiEv ev(GC_UI_EV_WONDER_CREATED);
	int x, y;
	RtUiReplica::PlotXY(pThisPlot, &x, &y);
	ev.A((int)ePlayerID).A(x).A(y).A((int)eBuildingIndex).A(iState);
	m_uiRep->Send(ev);
}

void RtEngine::GameplayWonderEdited(PlayerTypes ePlayerID, ICvPlot1* pThisPlot, BuildingTypes eBuildingIndex, int iState)
{
	if (!On(m_uiRep)) return;
	RtUiEv ev(GC_UI_EV_WONDER_EDITED);
	int x, y;
	RtUiReplica::PlotXY(pThisPlot, &x, &y);
	ev.A((int)ePlayerID).A(x).A(y).A((int)eBuildingIndex).A(iState);
	m_uiRep->Send(ev);
}

void RtEngine::GameplayWonderRemoved(PlayerTypes ePlayerID, ICvPlot1* pThisPlot, BuildingTypes eBuildingIndex)
{
	if (!On(m_uiRep)) return;
	RtUiEv ev(GC_UI_EV_WONDER_REMOVED);
	int x, y;
	RtUiReplica::PlotXY(pThisPlot, &x, &y);
	ev.A((int)ePlayerID).A(x).A(y).A((int)eBuildingIndex);
	m_uiRep->Send(ev);
}

void RtEngine::GameplaySpaceshipCreated(ICvPlot1* pThisPlot, int iState)
{
	if (!On(m_uiRep)) return;
	RtUiEv ev(GC_UI_EV_SPACESHIP_CREATED);
	int x, y;
	RtUiReplica::PlotXY(pThisPlot, &x, &y);
	ev.A(x).A(y).A(iState);
	m_uiRep->Send(ev);
}

void RtEngine::GameplaySpaceshipEdited(ICvPlot1* pThisPlot, int iState)
{
	if (!On(m_uiRep)) return;
	RtUiEv ev(GC_UI_EV_SPACESHIP_EDITED);
	int x, y;
	RtUiReplica::PlotXY(pThisPlot, &x, &y);
	ev.A(x).A(y).A(iState);
	m_uiRep->Send(ev);
}

void RtEngine::GameplaySpaceshipRemoved(ICvPlot1* pThisPlot) { PlotEv(m_uiRep, GC_UI_EV_SPACESHIP_REMOVED, pThisPlot); }
void RtEngine::GameplayOpenOptionsScreen() { Plain(m_uiRep, GC_UI_EV_OPEN_OPTIONS_SCREEN); }

void RtEngine::GameplaySearchForPediaEntry(const char* c)
{
	if (!On(m_uiRep)) return;
	RtUiEv ev(GC_UI_EV_SEARCH_FOR_PEDIA_ENTRY);
	ev.T(c);
	m_uiRep->Send(ev);
}

void RtEngine::GameplayOpenInfoCorner(int iInfoCornerID)
{
	if (!On(m_uiRep)) return;
	RtUiEv ev(GC_UI_EV_OPEN_INFO_CORNER);
	ev.A(iInfoCornerID);
	m_uiRep->Send(ev);
}

void RtEngine::GameplayWorldAnchor(GenericWorldAnchorTypes eAnchor, bool bAdd, int iX, int iY, int iData1)
{
	if (!On(m_uiRep)) return;
	RtUiEv ev(GC_UI_EV_WORLD_ANCHOR);
	ev.A((int)eAnchor).A(bAdd ? 1 : 0).A(iX).A(iY).A(iData1);
	m_uiRep->Send(ev);
}

void RtEngine::GameplayMinimapUnitSelect(int iX, int iY)
{
	if (!On(m_uiRep)) return;
	RtUiEv ev(GC_UI_EV_MINIMAP_UNIT_SELECT);
	ev.A(iX).A(iY);
	m_uiRep->Send(ev);
}

void RtEngine::GameplayMinimapNotification(int iX, int iY, int iID)
{
	if (!On(m_uiRep)) return;
	RtUiEv ev(GC_UI_EV_MINIMAP_NOTIFICATION);
	ev.A(iX).A(iY).A(iID);
	m_uiRep->Send(ev);
}

void RtEngine::GameplayGoldenAgeStarted() { Plain(m_uiRep, GC_UI_EV_GOLDEN_AGE_STARTED); }
void RtEngine::GameplayGoldenAgeEnded() { Plain(m_uiRep, GC_UI_EV_GOLDEN_AGE_ENDED); }

void RtEngine::GameplayTurnChanged(int iNewTurn)
{
	if (!On(m_uiRep)) return;
	RtUiEv ev(GC_UI_EV_TURN_CHANGED);
	ev.A(iNewTurn);
	m_uiRep->Send(ev);
}

void RtEngine::GameplayActivePlayerChanged(PlayerTypes eNewPlayer)
{
	if (!On(m_uiRep)) return;
	RtUiEv ev(GC_UI_EV_ACTIVE_PLAYER_CHANGED);
	ev.A((int)eNewPlayer);
	m_uiRep->Send(ev);
}

void RtEngine::PublishActivePlayer(PlayerTypes ePlayer, PlayerTypes eOldPlayer)
{
	if (!On(m_uiRep)) return;
	RtUiEv ev(GC_UI_EV_PUBLISH_ACTIVE_PLAYER);
	ev.A((int)ePlayer).A((int)eOldPlayer);
	m_uiRep->Send(ev);
}

void RtEngine::PublishNewGameTurn(int gameTurn)
{
	if (!On(m_uiRep)) return;
	RtUiEv ev(GC_UI_EV_PUBLISH_NEW_GAME_TURN);
	ev.A(gameTurn);
	m_uiRep->Send(ev);
}

void RtEngine::PublishEraChanges() { Plain(m_uiRep, GC_UI_EV_PUBLISH_ERA_CHANGES); }

void RtEngine::TradeVisuals_NewRoute(int iRoute, int iPlayer, TradeConnectionType type, int nPlots, int x[], int y[])
{
	if (!On(m_uiRep)) return;
	RtUiEv ev(GC_UI_EV_TRADE_NEW_ROUTE);
	// The args carry the first 9 plots (kept for old readers); the data blob carries ALL nPlots as int32 LE x, y pairs.
	const int total = (nPlots < 0 || !x || !y) ? 0 : nPlots;
	const int n = total > 9 ? 9 : total;
	ev.A(iRoute).A(iPlayer).A((int)type).A(n);
	for (int i = 0; i < n; ++i) ev.A(x[i]).A(y[i]);
	std::vector<int32_t> all;
	if (total > 9)
	{
		all.reserve((size_t)total * 2);
		for (int i = 0; i < total; ++i) { all.push_back(x[i]); all.push_back(y[i]); }
		ev.D((const uint8_t*)&all[0], all.size() * sizeof(int32_t));
	}
	m_uiRep->Send(ev);
}

void RtEngine::TradeVisuals_UpdateRouteDirection(int iRoute, bool bForwards)
{
	if (!On(m_uiRep)) return;
	RtUiEv ev(GC_UI_EV_TRADE_ROUTE_DIRECTION);
	ev.A(iRoute).A(bForwards ? 1 : 0);
	m_uiRep->Send(ev);
}

void RtEngine::TradeVisuals_DestroyRoute(int iRoute, int iPlayer)
{
	if (!On(m_uiRep)) return;
	RtUiEv ev(GC_UI_EV_TRADE_DESTROY_ROUTE);
	ev.A(iRoute).A(iPlayer);
	m_uiRep->Send(ev);
}

void RtEngine::TradeVisuals_ActivatePopupRoute(int iRoute)
{
	if (!On(m_uiRep)) return;
	RtUiEv ev(GC_UI_EV_TRADE_ACTIVATE_POPUP_ROUTE);
	ev.A(iRoute);
	m_uiRep->Send(ev);
}

void RtEngine::TradeVisuals_DeactivatePopupRoute() { Plain(m_uiRep, GC_UI_EV_TRADE_DEACTIVATE_POPUP_ROUTE); }

}  // namespace lekmod_portable
