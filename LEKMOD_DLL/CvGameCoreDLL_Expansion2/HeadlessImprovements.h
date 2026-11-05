#pragma once
// Headless platform: flags of dll-improvements entries.
// Every OPT/FIX/HYG change in the DLL is fenced by its own flag. Flags are NEVER defined here by hand:
// headless/tools/build-dll.ps1 -FlagSet opt|fix|release passes them as /D from headless/improvements/flagsets.json.
// With no flag defined the DLL is byte-for-byte golden.
//
// Registry: headless/improvements/{opts,fixes,hyg}.json. One line per flag: id and a one-line description.
//
// --- OPT (bit-for-bit neutral) -------------------------------------------------------------------------------
// LEKMOD_HEADLESS_OPT_ASTAR_REMOVE_MALLOC       OPT-0001 CvAStar node arrays via FNEW/delete[] (AUI_ASTAR_REMOVE_MALLOC)
// LEKMOD_HEADLESS_OPT_ASTAR_PRECALC_NEIGHBORS   OPT-0002 CvAStar neighbour table built once at Initialize (AUI_ASTAR_PRECALCULATE_NEIGHBORS_ON_INITIALIZE + partial-node fix)
// LEKMOD_HEADLESS_OPT_ASTAR_CACHE_PLOTS         OPT-0003 CvAStarNode::m_pPlot instead of GC.getMap().plotUnchecked(x, y) (AUI_ASTAR_CACHE_PLOTS_AT_NODES)
// LEKMOD_HEADLESS_OPT_TRADE_PARTNERS_ARRAY      OPT-0004 GetNumDifferentTradingPartners: bool array instead of std::vector<bool> (AUI_TRADE_FIX_GET_NUM_DIFFERENT_TRADING_PARTNERS_USES_ARRAY)
// LEKMOD_HEADLESS_OPT_TRADE_PLOTLIST_POD        OPT-0005 TradeConnectionPlotList copied with memcpy (AUI_TRADE_FIX_FSTATICVECTOR_CONTENTS_ARE_POD)
// LEKMOD_HEADLESS_OPT_FAST_MINMAX               OPT-0006 MAX/MIN macros return by value (NQM_FAST_COMP)
// LEKMOD_HEADLESS_OPT_HEX_SIGN_TEST             OPT-0007 xor sign test in hexDistance/plotDistance/plotXYWithRangeCheck (NQM_GAME_CORE_UTILS_OPTIMIZATIONS)
// LEKMOD_HEADLESS_OPT_ECONAI_INPLACE_FILTER     OPT-0008 CvEconomicAI explorer/hut lists filtered in place instead of copy + rebuild
// LEKMOD_HEADLESS_OPT_TRADEAI_SCOREPROD_BYREF   OPT-0009 CvTradeAI::ScoreProductionTR takes the target city list by const reference
// LEKMOD_HEADLESS_OPT_GAMEOPTION_CACHE          OPT-0010 CvPreGame::GetGameOption(const char*) memoised (options scan + SQL default), exact invalidation
// LEKMOD_HEADLESS_OPT_BULLY_ICON_BATCH          OPT-0011 setXY/setDamage bully-icon block: one local-power scan per minor for all majors
// LEKMOD_HEADLESS_OPT_ASTAR_MOVE_MEMO           OPT-0012 A* PathCost/PathAdd: per-search memo of ZOC neighbour scan and movement predicates (pure, same results)
// LEKMOD_HEADLESS_OPT_ASTAR_NODE_CACHE_MEMO     OPT-0013 A* PathValid/TacticalAnalysisMapPathValid: node cost cache filled once per search instead of per edge
// LEKMOD_HEADLESS_OPT_SITE_PLOT_MEMO            OPT-0014 AI_updateFoundValues: per-pass memo of PlotFoundValue's per-plot Compute* terms + hoisted improvement lookups
// LEKMOD_HEADLESS_OPT_ASTAR_WALK_MEMO           OPT-0015 A* PathValid/TacticalAnalysisMapPathValid: parent-chain walk memoised per node (needs OPT-0013)
// LEKMOD_HEADLESS_OPT_ASTAR_FROM_PLOT_MEMO      OPT-0017 A* PathValid/TacticalAnalysisMapPathValid: canMoveThrough(from) memoised per search, adjacency test after the cheap tests
// LEKMOD_HEADLESS_OPT_DIPLO_RESPONSE_MEMO       OPT-0016 CvGame::GetDiploResponse: memo of the (Tag, Bias) query rows per (leader, response), same async pick
// LEKMOD_HEADLESS_OPT_TOURISM_BUILDING_FIRST   OPT-0018 CvCityCulture::GetBaseTourism: building count tested before the O(policies) building-class tourism modifier
// LEKMOD_HEADLESS_OPT_INFOTYPE_STRING_MEMO     OPT-0019 CvGlobals::getInfoTypeForString: memo by name contents, invalidated whenever the infos map is written
// LEKMOD_HEADLESS_OPT_BELIEF_PLOT_HOIST        OPT-0020 CvReligionAI::ScoreBeliefAtPlot: canHaveImprovement evaluated once per improvement, not once per yield
// LEKMOD_HEADLESS_OPT_UNIT_REPATH_DEDUP        OPT-0021 (rejected, code removed) CvUnit::UpdatePathCache/UnitAttack: skip the repeated identical GeneratePath for an adjacent target
// LEKMOD_HEADLESS_OPT_TRADE_PATH_MEMO          OPT-0022 CvGameTrade::IsValidTradeRoutePath/GetAvailableTR: memo of the trade-route A* result and path, invalidated per map block read by the search
// LEKMOD_HEADLESS_OPT_BELIEF_OWNED_LOOP        OPT-0040 CvReligionBeliefs sums: visit the religion's own belief ids ascending instead of every belief x HasBelief
// LEKMOD_HEADLESS_OPT_DB_SPARSE_LISTS          OPT-0041 policy yield/tourism sums and calculateYield's landmass building loop: only DB entries that can contribute
// LEKMOD_HEADLESS_OPT_YIELD_DEFICIENT_MEMO     OPT-0042 CvCityStrategyAI::IsYieldDeficient memo inside GetBuyPlotScore / GetDeficientYield (Lua listeners clear it)
// LEKMOD_HEADLESS_OPT_TRADE_NEED_SHARE         OPT-0043 CityStrategy NO_NEED_INTERNATIONAL_*_TRADE_ROUTE reuses the directly preceding NEED test (= !NEED)
// LEKMOD_HEADLESS_OPT_NATURE_PREREQ_SKIP       OPT-0044 CvPlot::canHaveImprovement: calculateNatureYield only for a positive nature-yield prerequisite
// LEKMOD_HEADLESS_OPT_AI_TEAMMATE_ORDER        OPT-0045 CvPlayer::IsAITeammateOfHuman: cheap pure tests (alive, team) before CvPreGame::isHuman
// LEKMOD_HEADLESS_OPT_OPTION_SITE_CACHE        OPT-0046 isOption("...") at 8 hot call sites cached per site, valid while OPT-0010's memo generation holds (needs OPT-0010)
// LEKMOD_HEADLESS_OPT_CITIZEN_FOOD_MEMO        OPT-0047 CvCityCitizens::GetBestCityPlotWithValue: the city's surplus-food term of GetPlotValue computed once per scan
// LEKMOD_HEADLESS_OPT_BULLY_DISK_LISTS         OPT-0048 OPT-0011's bully local-power scan walks a stored per-minor plot-index list (golden's dx/dy order) (needs OPT-0011)
// LEKMOD_HEADLESS_OPT_TOURISM_SPARSE_CLASSES   OPT-0049 CvCityCulture tourism: the 4 building-class loops visit only DB-relevant classes per civilization (needs OPT-0041)
// LEKMOD_HEADLESS_OPT_GOODY_TEST_ORDER         OPT-0050 CvPlot::isRevealedGoody: plot reads before CvTeam::isBarbarian/isMinorCiv (pure conjunction)
// LEKMOD_HEADLESS_OPT_SITE_RING_TABLE          OPT-0051 CvCitySiteEvaluator::PlotFoundValue: fixed radius-7 offset/distance table for anchors away from edges and seams
// LEKMOD_HEADLESS_OPT_POLICY_NUMERIC_SPARSE    OPT-0052 CvPlayerPolicies::GetNumericModifier: per type only the policies with a nonzero switch term (needs OPT-0041)
// LEKMOD_HEADLESS_OPT_TEAM_MINOR_SCAN          OPT-0060 CvTeam::isMinorCiv: minor slots matched on CvPreGame's dense team array (SSE2, 4 slots per compare) before CvPlayer::isAlive
// LEKMOD_HEADLESS_OPT_INLINE_ACCESSORS         OPT-0061 hot trivial accessors (CvGlobals info getters, entry tables, GetNumBuilding, plot/team/civ getters) inline in HeadlessInlineAccessors.h
// LEKMOD_HEADLESS_OPT_MAP_PLOT_FASTPATH        OPT-0062 CvMap::plot: coordinates already on the map return the plot directly (two unsigned compares), others take the golden wrap path
// LEKMOD_HEADLESS_OPT_ASTAR_OPEN_BUCKETS     OPT-0086 CvAStar open list: an index of cost runs finds the insertion place by binary search (the list and its order stay golden's; portable build)
// LEKMOD_HEADLESS_OPT_ASTAR_UNREACHABLE_STOP  OPT-0090 TurnsToReachTarget: a tactical search inside a pure-query scope stops once its target can no longer be popped (or only above the caller's turn bound); the rest runs before a reuse and at the scope's end
// LEKMOD_HEADLESS_OPT_PAR_CLOSEST_OP_UNIT      OPT-0080 (rejected, #error unless LEKMOD_HEADLESS_PAR_EXPERIMENT) FindClosestOperationUnit's TurnsToReachTarget searches on a thread pool: the A* call tree fires the Lua GameEvent CanDeclareWar
//
// OPT-0001..0007 switch on dormant community code: Delnar's AUI_*/NQM_* switches sit inside the /* ... */ comment of
// _Defines.h, so golden never compiles them. They are defined only here, from the OPT flags. Nothing in _Defines.h
// tests them before this header is included at its end, and CvGameCoreDLLPCH.h tests NQM_FAST_COMP only after
// including _Defines.h.
#if defined(LEKMOD_HEADLESS_OPT_ASTAR_REMOVE_MALLOC) && !defined(AUI_ASTAR_REMOVE_MALLOC)
#define AUI_ASTAR_REMOVE_MALLOC
#endif
#if defined(LEKMOD_HEADLESS_OPT_ASTAR_PRECALC_NEIGHBORS) && !defined(AUI_ASTAR_PRECALCULATE_NEIGHBORS_ON_INITIALIZE)
#define AUI_ASTAR_PRECALCULATE_NEIGHBORS_ON_INITIALIZE
#endif
#if defined(LEKMOD_HEADLESS_OPT_ASTAR_CACHE_PLOTS) && !defined(AUI_ASTAR_CACHE_PLOTS_AT_NODES)
#define AUI_ASTAR_CACHE_PLOTS_AT_NODES
#endif
#if defined(LEKMOD_HEADLESS_OPT_TRADE_PARTNERS_ARRAY) && !defined(AUI_TRADE_FIX_GET_NUM_DIFFERENT_TRADING_PARTNERS_USES_ARRAY)
#define AUI_TRADE_FIX_GET_NUM_DIFFERENT_TRADING_PARTNERS_USES_ARRAY
#endif
#if defined(LEKMOD_HEADLESS_OPT_TRADE_PLOTLIST_POD) && !defined(AUI_TRADE_FIX_FSTATICVECTOR_CONTENTS_ARE_POD)
#define AUI_TRADE_FIX_FSTATICVECTOR_CONTENTS_ARE_POD
#endif
#if defined(LEKMOD_HEADLESS_OPT_FAST_MINMAX) && !defined(NQM_FAST_COMP)
#define NQM_FAST_COMP
#endif
#if defined(LEKMOD_HEADLESS_OPT_HEX_SIGN_TEST) && !defined(NQM_GAME_CORE_UTILS_OPTIMIZATIONS)
#define NQM_GAME_CORE_UTILS_OPTIMIZATIONS
#endif
#if defined(LEKMOD_HEADLESS_OPT_TRADE_PATH_MEMO)
// OPT-0022 (CvTradeClasses.cpp): invalidation of the trade-route path memo. Rare writers of a trade-path plot input (CvPlot setters,
// CvMap::setup/updateAdjacency, city add/delete, player team change) void the whole domain right after the write; the frequent ones
// (route presence, feature, water plot owner) stamp only the map block of their plot.
void HeadlessTradePathBumpLand();
void HeadlessTradePathBumpWater();
void HeadlessTradePathBumpLandPlot(int iX, int iY);
void HeadlessTradePathBumpWaterPlot(int iX, int iY);
// While a memoised trade-route search runs, the trade-route finder callbacks (CvAStar.cpp) record the map block of every plot they
// read; NULL otherwise.
extern unsigned int* g_puiHeadlessTradePathRecord;
extern const unsigned char* g_pucHeadlessTradePathBlock;
extern int g_iHeadlessTradePathGridW;
inline void HeadlessTradePathMark(int iX, int iY)
{
	unsigned int* puiRecord = g_puiHeadlessTradePathRecord;
	if (puiRecord != NULL)
	{
		const unsigned int uiBlock = g_pucHeadlessTradePathBlock[iY * g_iHeadlessTradePathGridW + iX];
		puiRecord[uiBlock >> 5] |= (1u << (uiBlock & 31));
	}
}
#endif
//
// --- FIX (documented behaviour change) --------------------------------
// LEKMOD_HEADLESS_FIX_0001   FIX-0001/DS-0001 CvUnit: recompute m_iCombatBonusVsDifferentIdeologyModifier from promotions after load (no save format change)
// LEKMOD_HEADLESS_FIX_0002   FIX-0002/DS-0002 CvPlayer: m_bCanChooseReformationBelief = false in ctor and reset (was uninitialised)
// LEKMOD_HEADLESS_FIX_0009   FIX-0009/DS-0019 CvEspionageAI read: saved arrays over the reset() defaults, not appended (no save format change)
// LEKMOD_HEADLESS_FIX_0010   FIX-0010/DS-0020 CvGoodyHuts::Read: recent goodies then once-per-game flags, in Write's order (no save format change)
// LEKMOD_HEADLESS_FIX_0011   FIX-0011/DS-0021 CvMap: post-load landmass recalculation reproduces the saved landmass IDs (no save format change)
// LEKMOD_HEADLESS_FIX_0012   FIX-0012/DS-0022 CvGame: post-load initScoreCalculation keeps the saved m_iMaxPopulation/m_iInit* (no save format change)
// LEKMOD_HEADLESS_FIX_0013   FIX-0013/DS-0010 CvBuilderTaskingAI: after a load no forced worker re-plan (shared road list kept), city best yields computed
// LEKMOD_HEADLESS_FIX_0014   FIX-0014/DS-0010 CvBuilderTaskingAI::EvaluateBuilder: city best yields recomputed before scoring (sim change: fresher data)
// LEKMOD_HEADLESS_FIX_0015   FIX-0015/DS-0027 CvPlayerAI::AI_unitUpdate: tactical analysis map rebuilt for the moving player (sim change; not serialized)
// LEKMOD_HEADLESS_FIX_0016   FIX-0016/DS-0029 CvWorldInfo::readFrom: EstimatedNumCities re-read from the Worlds row (not saved; warmonger 0 after load)
// LEKMOD_HEADLESS_FIX_0017   FIX-0017/DS-0028 FPU_PRECISION_GUARD: x87 precision/rounding pinned to 53 bit / nearest for every exe entry into the DLL (HeadlessFpuGuard.h; no-op at 0x027F)
// LEKMOD_HEADLESS_FIX_0018   FIX-0018/DS-0030 CvEspionageSpy read: keep the saved m_bEvaluateReassignment (golden forced true: AI spies reshuffled after load)
// LEKMOD_HEADLESS_FIX_0019   FIX-0019/DS-0032 CvTacticalAI postures saved in an optional trailer after the supporting data (announced in the phony int; both-way save compatible)
// LEKMOD_HEADLESS_FIX_0020   FIX-0020/DS-0035 CvUnit::CanSwapWithUnitHere (UI-only swap check of every human move order): private IgnoreUnits finder, the global one's reuse tree untouched
// LEKMOD_HEADLESS_FIX_0021   FIX-0021 CvPlayer::TestMidTurnPopGrowth: leave the loop after capping an Avoid Growth city's food (golden looped forever: MP freeze)
// LEKMOD_HEADLESS_FIX_0022   FIX-0022/DS-0034 Lua Player:GetMilitaryMight/GetPower, Team:GetPower: read the might cache without writing its FAutoVariables (UI-only shadow)
// LEKMOD_HEADLESS_FIX_0023   FIX-0023/DS-0037 Lua gold/science/turns-left/trade getters run in CvHeadlessUiTreasuryScope: treasury unit costs into locals, saved fields untouched
// LEKMOD_HEADLESS_FIX_0025   FIX-0025/DS-0035 CvPlayerAI::AI_unitUpdate busy-unit early return restores the six finders' MP cache flags (golden left them on for good: cross-turn/UI reuse)
// LEKMOD_HEADLESS_FIX_0026   FIX-0026/DS-0007 CvDealAI: the human's peace-value cache is subtracted on the sender before sendNetDealAccepted, DoAcceptedDeal (every peer) no longer reads it
// LEKMOD_HEADLESS_FIX_0027   FIX-0027/DS-0008 CvDealAI::DoTradeScreenClosed: ClearDealToRenew / RA cancel go through sendFromUIDiploEvent (new event code) in network games, applied on every peer; SP/hot-seat unchanged
// LEKMOD_HEADLESS_FIX_0028   FIX-0028/DS-0006 DoFromUIDiploEvent stop-converting / stop-digging: request and answer recorded on every peer (golden: asker's machine only)
// LEKMOD_HEADLESS_FIX_0029   FIX-0029 CvDealAI::DoEqualizeDealWithHuman: acceptability tested against the deal's human (eOtherPlayer), not the per-machine active player
// LEKMOD_HEADLESS_FIX_0030   FIX-0030/DS-0037 trade screen deal valuation (DoHumanOfferDealToThisAI, DoHumanDemand, DoEqualizeDealWithHuman, DoTradeScreenOpened, Lua deal values, Deal:Add/ChangeGoldPerTurnTrade) in CvHeadlessUiTreasuryScope
//
// --- FIX-VALIDATION (Response* checks; honest commands bit-for-bit, invalid rejected before any mutation) ------
// LEKMOD_HEADLESS_FIX_VAL_RESEARCH            FIX-0101 ResponseResearch: iDiscover <= GetNumFreeTechs(), espionage target/tech in range
// LEKMOD_HEADLESS_FIX_VAL_CHANGE_WAR          FIX-0102 ResponseChangeWar: player/team id in range, no war/peace with own team
// LEKMOD_HEADLESS_FIX_VAL_FOUND_RELIGION      FIX-0104 ResponseFoundReligion: plot must exist (NULL deref), holy city owned by the founder
// LEKMOD_HEADLESS_FIX_VAL_ENHANCE_RELIGION    FIX-0105 ResponseEnhanceReligion: player id in range, NULL-safe plot in the failure branch (chat -1 untouched)
// LEKMOD_HEADLESS_FIX_VAL_DIPLOMACY_FROM_UI   FIX-0106 ResponseDiplomacyFromUI: player ids in range
// LEKMOD_HEADLESS_FIX_VAL_MINOR_CIV           FIX-0107 ResponseMinorCiv*/PledgeMinorProtection/MinorNoUnitSpawning: major/minor id ranges
// LEKMOD_HEADLESS_FIX_VAL_GOODY_CHOICE        FIX-0108 ResponseGoodyChoice: plot/unit must exist, unit carries the goody-hut picker promotion
// LEKMOD_HEADLESS_FIX_VAL_LEAGUE              FIX-0109 ResponseLeague*: NULL league, CanVote/CanProposeEnact/CanProposeRepeal as real checks
// LEKMOD_HEADLESS_FIX_VAL_RESEARCH_TECH       FIX-0111 ResponseResearch: tech id NO_TECH or in range in every branch (normal research indexed the table unchecked)
// LEKMOD_HEADLESS_FIX_VAL_PUSH_MISSION        FIX-0112 ResponsePushMission: MOVE_TO/ROUTE_TO to a plot that does not exist rejected (crash in setExperience later)
// LEKMOD_HEADLESS_FIX_VAL_LUA_FOUNDER         FIX-0113 Lua Game.GetFounder: NO_PLAYER for an unfounded religion (golden dereferenced NULL)
//
// --- HYG (hygiene: leaks, crashes on paths unreachable in the corpus; digest unchanged) -----------------------
// LEKMOD_HEADLESS_HYG_TRAIT_DTOR_LEAK   HYG-0001 CvTraitEntry: free the 1D arrays the destructor leaked (AUI_WARNING_FIXES is off), no double 2D alloc
// LEKMOD_HEADLESS_HYG_REPLAY_MSG_INIT   HYG-0002 CvReplayMessage(turn, type, player): m_iExtraData1/2 = 0 (stack residue was saved; digest noise only)
// LEKMOD_HEADLESS_HYG_NOTIFICATION_INIT HYG-0003 CvNotifications::Init: empty slots get m_iExtraGameData -1 / m_ePlayerID NO_PLAYER (stack residue was saved)
// LEKMOD_HEADLESS_HYG_PANTHEON_HOLYCITY_INIT HYG-0004 CvReligion(.., pHolyCity NULL, ..): holy-city coordinates -1 for pantheons (stack residue was saved)
// LEKMOD_HEADLESS_HYG_MILITARY_AI_LASTTURN_INIT HYG-0005/DS-0023 CvMilitaryAI::Reset: the four per-player last-turn arrays cleared over MAX_CIV_PLAYERS (was: number of AI strategies; heap garbage was saved)
// LEKMOD_HEADLESS_HYG_MINOR_BULLY_TOTALS_INIT HYG-0006/DS-0024 CvMinorCivAI::Reset: bully gold/workers totals cleared over MAX_CIV_PLAYERS (was: majors only; heap garbage was saved)
// LEKMOD_HEADLESS_HYG_TRAIT_COMBAT_IMPROVEMENT_INIT HYG-0007/DS-0025 CvTraitEntry ctor: m_eCombatBonusImprovement = NO_IMPROVEMENT (its initialiser sat in the dead AUI_WARNING_FIXES block; heap garbage was saved)
// LEKMOD_HEADLESS_HYG_CULTURE_OPINION_INIT HYG-0008/DS-0026 CvPlayerCulture::Init: public-opinion fields start at their DoPublicOpinion defaults (slots that never play saved heap garbage)
// LEKMOD_HEADLESS_DIAG_TURN_LOG HYG-0010 CvGame::doTurn: per-turn checksum lines (RNG seeds/counts, map, teams, players) in Logs\LekmodNG-turns.log when LoggingEnabled (read only; desync forensics)
// LEKMOD_HEADLESS_HYG_AIOP_REPLACE_LOSSES_INIT HYG-0009CvAIOperation::Reset: m_bShouldReplaceLossesWithReinforcements = false (the derived Init() overrides skipped the base Init; heap garbage was saved, flag never read)
// LEKMOD_HEADLESS_HYG_MULTIMAP_FIRST_EQUAL HYG-0011 CvTraitEntry/CvPolicyEntry::IsFreePromotionUnitCombat: multimap lower_bound instead of find (same element as MSVC's find; libc++ 22 find returns any equal one)
// LEKMOD_HEADLESS_HYG_COMBAT_MESSAGE_PLOT HYG-0012 CvUnitCombat: combat lines sent as MESSAGE_TYPE_COMBAT_MESSAGE with the fight's plot (portable build only; UI only)
// LEKMOD_HEADLESS_HYG_MESSAGES_TO_HUMANS HYG-0013 UI message branches guarded by "owner is the active player" also run for every human owner (CvUnitCombat, CvPlayer, CvUnit, CvTeam, CvCity, CvPlot; macros in HeadlessMsgGuard.h; portable build only; UI only)

#include "HeadlessDiag.h"
#include "HeadlessFpuGuard.h"	// FIX-0017: needs HEADLESS_FIX_TRIGGER from HeadlessDiag.h
