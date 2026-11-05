// rt_ui_replica: the interface half of a replica (B4 M2 WO-1; abi/gamecore_abi.h "UI bridge").
//
// A replica serves the original Lua UI. Two things of the exe live here:
//  - the event sink: what the DLL tells the exe's UI (gDLL->Gameplay*, DLLUI->*) becomes gc_ui_event, packed into integers (no pointer leaves);
//  - the interface state the DLL reads by pointer: the selection (units, cities), the interface mode and its value, the city screen, the cycle counter.
// Both exist only in GC_ROLE_REPLICA: a server (PLAIN / SERVER) has no UI and behaves as before. C++03.
//
// ---- ICvUserInterface2: interface or rules (WO-1 item 4) --------------------------------------------------------------------------------
// "I" = interface: answered from the state here / sent to the sink in the replica role (RtUserInterface delegates). "R" = rules: the DLL's rules read it, so it answers
// as the server does (the RtUserInterface answer or the generated stub), otherwise the journal's PUMP slice would take another branch and the replica would diverge.
// The call sites are those of LEKMOD_DLL/CvGameCoreDLL_Expansion2 (DLLUI-> / GC.GetEngineUserInterface()->).
//
//  R  canEndTurn / setCanEndTurn / isHasMovedUnit / setHasMovedUnit   rt_ui.cpp keeps the DLL's flags (CvGame, CvUnitMission); per human, as on the server.
//  R  IsSPAutoEndTurnEnabled / IsMPAutoEndTurnEnabled (+ Set*)         false: CvGame::updateTestEndTurn must not end the turn by itself.
//  R  isDiploActive / isDiploOrPopupWaiting                            RtDiplo queue (CvUnit.cpp:1727, CvDiplomacyRequests, CvGame::update).
//  R  SetScratchDeal / GetScratchDeal                                  RtDiplo (the AI's offer to the human).
//  R  IsPolicyNotificationSeen / SetPolicyNotificationSeen             rt_ui.cpp (CvNotifications::Dismiss, CvPlayer.cpp:5359).
//  R  IsOptionNoRewardPopups                                           stub, false (CvTeam.cpp:6369 changes what a tech reward does).
//  R  isPopupUp / isPopupQueued / IsPopupQueueEmpty / IsModalStackEmpty / IsDontShowPopups / SetDontShowPopups / IsDealInTransit / SetDealInTransit /
//     IsAIRequestingConcessions / IsHumanMakingDemand / Get/Set/ChangeStartDiploRepeatCount / Get/Set/ChangeOfferTradeRepeatCount /
//     IsForceDiscussionModeQuitOnBack / SetForceDiscussionModeQuitOnBack / IsSPQuickCombatEnabled / IsMPQuickCombatEnabled / isForcePopup / setForcePopup /
//     isFocused / isFocusedWidget / isCombatFocus / setCombatFocus / isDiplomacyLocked / setDiplomacyLocked / isInAdvancedStart / IsLoadedGame /
//     waitingForRemotePlayers / getEndTurnCounter / Set/ChangeEndTurnCounter / updateEndTurnTimer      stub answers of the server (the rules read none of them to a
//                                                                     branch that changes saved state; combat focus only gates CvUnit's visualisation flag).
//  I  Selection: IsUnitSelected (CvUnit::IsSelected: 20 rules-side readers, all of them UI effects: dirty bits, messages, the camera, CvPlot::updateCenterUnit),
//     selectUnit (CvUnit.cpp:11593, CvCity.cpp:6961), selectGroup, selectAll, RemoveFromSelectionList / VisuallyDeselectUnit (CvUnit::kill), makeSelectionListDirty,
//     CanSelectionListFound, getLastSelectedUnit / setLastSelectedUnit, ClearSelectionList, InsertIntoSelectionList, GetLengthSelectionList (CvUnit.cpp:1725),
//     GetHeadSelectedUnit (CvUnitCycler.cpp:40, CvAStar.cpp:5505), GetSelectedUnits (CvLuaPlayer GetRecommended*: the selection, rt_ui.cpp), DoAutoUnitCycle (exe frame logic: false),
//     getSelectionPlot (CvUnit.cpp, the plot of the head unit), getOriginalPlotIndex / changePlotListColumn / getPlotListColumn / getPlotListOffset /
//     verifyPlotListColumn / getOriginalPlotCount (plot list of the exe's bottom bar: 0).
//  I  Cities: selectCity, selectLookAtCity, addSelectedCity, clearSelectedCities (CvCity.cpp:1565), isCitySelected (CvCity::isCitySelected: UI effects only),
//     isCitySelection, getHeadSelectedCity (CvPlot.cpp:5718), nextSelectedCitiesNode / headSelectedCitiesNode, updateCityScreen, isCityScreenUp / setCityScreenUp.
//  I  Mode: setInterfaceMode, getInterfaceMode, GetInterfaceModeValue (CvUnit.cpp:23921), CanDoInterfaceMode, getCycleSelectionCounter / setCycleSelectionCounter /
//     changeCycleSelectionCounter (CvUnit.cpp:1734, 2544, CvPlayer.cpp:21049), IsSelectedUnitRevealingNewPlots / Set*.
//  I  Sink only (no state): AddMessage / AddUnitMessage / AddPlotMessage / AddCityMessage (CvUnit, CvCity, CvPlayer: 41 sites), clearEventMessages, AddPopup (both forms),
//     AddPopupText (12 sites), PublishActivePlayerTurnStart / End, PublishRemotePlayerTurnStart / End, PublishPlayerTurnStatus (CvPlayer.cpp:20981),
//     AddNotification / RemoveNotification (found by comparison after each batch, SyncReplicaNotifications), ActivateNotification (CvNotifications::Activate),
//     setDirty (83 sites), makeInterfaceDirty, lookAt (CvNotifications, CvPlayer), lookAtSelectionPlot, lookAtCityOffset, centerCamera, releaseLockedCamera,
//     SetSpecificCityInfoDirty (10 sites), UpdateEndTurn, SetEndTurnBlockingChanged, PublishEndTurnDirty, UpdateCountryBorder / UpdateAllCountryBorders,
//     AddHexToUIRange (CvAStar.cpp:5481), OpenEndGameMenu, OpenTechTree, OpenPlayerDealScreen, AddDeferredWonderCommand (CvCity.cpp:17102, CvBuildingClasses.cpp:3513),
//     ToggleStrategicView, SetTempString, setFlashing / isFlashing, refreshYieldVisibleMode and the toggle*VisibleMode family (exe display modes: not in the core),
//     updateCursorType, setNoSelectionListCycle, setBusy, DirectionAction, DebugCityRoutePlots, exitingToMainMenu, doTurn (exe frame: nothing).
//
// ---- IsGraphicsInitialized (WO-1 item 4): 13 sites, none writes saved state when it is true ------------------------------------------------------
//  CvMap::setupGraphical, CvPlot::setupGraphical, CvUnit::setupGraphical, CvCity::setupGraphical   announce the live objects (the events of gc_ui_setup_graphical).
//  CvUnit::~CvUnit (380)                       GameplayUnitDestroyed.
//  CvUnit::setXY (16511)                       QueueMoveForVisualization (m_unitMoveLocs, a non-saved vector) or SetPosition (GameplayUnitTeleported); the move itself is the same.
//  CvPlot::updateFog (590) / updateVisibility (616) / updateCenterUnit (681, setCenterUnit: a non-saved handle) / setLayoutDirty (6131, 6396)   display only.
//  CvTeam::changeBridgeBuildingCount (3375)    GameplayBridgeChanged.
//  CvGame::setActivePlayer (6475)              not reached: a replica never changes its active player through the game.
// The test that settles it: tests/real_replica_ui.rs and rt_selftest_replica_ui.cpp (replica digest equals the server's after every batch with the UI reading in between).
//
// ---- every method of ICvUserInterface2 (150), in declaration order: R = rules (answers as the server), I = interface (state here or an event to the sink) ----
//  I    QueryInterface                     exe frame or display only: nothing to keep, nothing to send
//  I    Destroy                            exe frame or display only: nothing to keep, nothing to send
//  I    lookAtSelectionPlot                sent to the sink as an event
//  I    CanDoInterfaceMode                 state in RtUiReplica (selection, mode, city screen, cycle counter)
//  I    getSelectionPlot                   state in RtUiReplica (selection, mode, city screen, cycle counter)
//  I    GetHeadSelectedUnit                state in RtUiReplica (selection, mode, city screen, cycle counter)
//  I    DoAutoUnitCycle                    exe frame or display only: nothing to keep, nothing to send
//  R    GetScratchDeal                     RtDiplo: the AI's offer to the human, read by CvGame::update, CvDiplomacyRequests, CvUnit
//  R    SetScratchDeal                     RtDiplo: the AI's offer to the human, read by CvGame::update, CvDiplomacyRequests, CvUnit
//  R    isPopupUp                          the server's answer (generated stub / UserInterface default): nothing of the replica's UI changes a branch of the rules
//  R    isPopupQueued                      the server's answer (generated stub / UserInterface default): nothing of the replica's UI changes a branch of the rules
//  R    isDiploOrPopupWaiting              RtDiplo: the AI's offer to the human, read by CvGame::update, CvDiplomacyRequests, CvUnit
//  R    isDiploActive                      RtDiplo: the AI's offer to the human, read by CvGame::update, CvDiplomacyRequests, CvUnit
//  I    IsUnitSelected                     state in RtUiReplica (selection, mode, city screen, cycle counter)
//  I    selectUnit                         state in RtUiReplica (selection, mode, city screen, cycle counter)
//  I    selectGroup                        state in RtUiReplica (selection, mode, city screen, cycle counter)
//  I    selectAll                          state in RtUiReplica (selection, mode, city screen, cycle counter)
//  I    RemoveFromSelectionList            state in RtUiReplica (selection, mode, city screen, cycle counter)
//  I    makeSelectionListDirty             exe frame or display only: nothing to keep, nothing to send
//  I    CanSelectionListFound              state in RtUiReplica (selection, mode, city screen, cycle counter)
//  I    getLastSelectedUnit                state in RtUiReplica (selection, mode, city screen, cycle counter)
//  I    setLastSelectedUnit                state in RtUiReplica (selection, mode, city screen, cycle counter)
//  I    changePlotListColumn               exe frame or display only: nothing to keep, nothing to send
//  I    getOriginalPlotIndex               exe frame or display only: nothing to keep, nothing to send
//  I    ClearSelectionList                 state in RtUiReplica (selection, mode, city screen, cycle counter)
//  I    InsertIntoSelectionList            state in RtUiReplica (selection, mode, city screen, cycle counter)
//  I    GetLengthSelectionList             state in RtUiReplica (selection, mode, city screen, cycle counter)
//  I    VisuallyDeselectUnit               state in RtUiReplica (selection, mode, city screen, cycle counter)
//  I    GetSelectedUnits                   state in RtUiReplica (selection; the recommendations of CvLuaPlayer read it)
//  I    selectCity                         state in RtUiReplica (selection, mode, city screen, cycle counter)
//  I    selectLookAtCity                   state in RtUiReplica (selection, mode, city screen, cycle counter)
//  I    addSelectedCity                    state in RtUiReplica (selection, mode, city screen, cycle counter)
//  I    clearSelectedCities                state in RtUiReplica (selection, mode, city screen, cycle counter)
//  I    isCitySelected                     state in RtUiReplica (selection, mode, city screen, cycle counter)
//  I    getHeadSelectedCity                state in RtUiReplica (selection, mode, city screen, cycle counter)
//  I    isCitySelection                    state in RtUiReplica (selection, mode, city screen, cycle counter)
//  I    nextSelectedCitiesNode             state in RtUiReplica (selection, mode, city screen, cycle counter)
//  I    headSelectedCitiesNode             state in RtUiReplica (selection, mode, city screen, cycle counter)
//  I    updateCityScreen                   sent to the sink as an event
//  I    AddMessage                         sent to the sink as an event
//  I    AddUnitMessage                     sent to the sink as an event
//  I    AddPlotMessage                     sent to the sink as an event
//  I    AddCityMessage                     sent to the sink as an event
//  I    clearEventMessages                 sent to the sink as an event
//  I    AddPopup                           sent to the sink as an event
//  I    AddPopupText                       sent to the sink as an event
//  I    PublishActivePlayerTurnStart       sent to the sink as an event
//  I    PublishActivePlayerTurnEnd         sent to the sink as an event
//  I    PublishRemotePlayerTurnEnd         sent to the sink as an event
//  I    PublishPlayerTurnStatus            sent to the sink as an event
//  R+I  AddNotification                    RtUserInterface keeps the registry as on a server; the replica role sends ADDED / REMOVED by comparing the seat's list after each batch (SyncReplicaNotifications)
//  R+I  RemoveNotification                 RtUserInterface keeps the registry as on a server; the replica role sends ADDED / REMOVED by comparing the seat's list after each batch (SyncReplicaNotifications)
//  I    ActivateNotification               sent to the sink as an event
//  I    getCycleSelectionCounter           state in RtUiReplica (selection, mode, city screen, cycle counter)
//  I    setCycleSelectionCounter           state in RtUiReplica (selection, mode, city screen, cycle counter)
//  I    changeCycleSelectionCounter        state in RtUiReplica (selection, mode, city screen, cycle counter)
//  I    IsSelectedUnitRevealingNewPlots    state in RtUiReplica (selection, mode, city screen, cycle counter)
//  I    SetSelectedUnitRevealingNewPlots   state in RtUiReplica (selection, mode, city screen, cycle counter)
//  R    getEndTurnCounter                  the server's answer (generated stub / UserInterface default): nothing of the replica's UI changes a branch of the rules
//  R    setEndTurnCounter                  the server's answer (generated stub / UserInterface default): nothing of the replica's UI changes a branch of the rules
//  R    changeEndTurnCounter               the server's answer (generated stub / UserInterface default): nothing of the replica's UI changes a branch of the rules
//  R    isCombatFocus                      the server's answer (generated stub / UserInterface default): nothing of the replica's UI changes a branch of the rules
//  R    setCombatFocus                     the server's answer (generated stub / UserInterface default): nothing of the replica's UI changes a branch of the rules
//  I    isDirty                            exe frame or display only: nothing to keep, nothing to send
//  I    setDirty                           sent to the sink as an event
//  I    makeInterfaceDirty                 sent to the sink as an event
//  I    updateCursorType                   exe frame or display only: nothing to keep, nothing to send
//  I    lookAt                             sent to the sink as an event
//  I    centerCamera                       sent to the sink as an event
//  I    releaseLockedCamera                sent to the sink as an event
//  R    isFocusedWidget                    the server's answer (generated stub / UserInterface default): nothing of the replica's UI changes a branch of the rules
//  R    isFocused                          the server's answer (generated stub / UserInterface default): nothing of the replica's UI changes a branch of the rules
//  I    refreshYieldVisibleMode            exe frame or display only: nothing to keep, nothing to send
//  I    toggleYieldVisibleMode             exe frame or display only: nothing to keep, nothing to send
//  I    toggleResourceVisibleMode          exe frame or display only: nothing to keep, nothing to send
//  I    toggleGridVisibleMode              exe frame or display only: nothing to keep, nothing to send
//  I    getOriginalPlotCount               exe frame or display only: nothing to keep, nothing to send
//  I    isCityScreenUp                     state in RtUiReplica (selection, mode, city screen, cycle counter)
//  I    setCityScreenUp                    state in RtUiReplica (selection, mode, city screen, cycle counter)
//  I    setNoSelectionListCycle            exe frame or display only: nothing to keep, nothing to send
//  R    canEndTurn                         the host's flags (rt_ui.cpp), per human; the rules read them (CvGame, CvUnitMission, CvNotifications)
//  I    UpdateEndTurn                      sent to the sink as an event
//  I    SetEndTurnBlockingChanged          sent to the sink as an event
//  I    setInterfaceMode                   state in RtUiReplica (selection, mode, city screen, cycle counter)
//  I    getInterfaceMode                   state in RtUiReplica (selection, mode, city screen, cycle counter)
//  I    GetInterfaceModeValue              state in RtUiReplica (selection, mode, city screen, cycle counter)
//  I    setFlashing                        sent to the sink as an event
//  I    isFlashing                         exe frame or display only: nothing to keep, nothing to send
//  R    setDiplomacyLocked                 the server's answer (generated stub / UserInterface default): nothing of the replica's UI changes a branch of the rules
//  R    isDiplomacyLocked                  the server's answer (generated stub / UserInterface default): nothing of the replica's UI changes a branch of the rules
//  R    setCanEndTurn                      the host's flags (rt_ui.cpp), per human; the rules read them (CvGame, CvUnitMission, CvNotifications)
//  R    updateEndTurnTimer                 the server's answer (generated stub / UserInterface default): nothing of the replica's UI changes a branch of the rules
//  R    waitingForRemotePlayers            the server's answer (generated stub / UserInterface default): nothing of the replica's UI changes a branch of the rules
//  R    isHasMovedUnit                     the host's flags (rt_ui.cpp), per human; the rules read them (CvGame, CvUnitMission, CvNotifications)
//  R    setHasMovedUnit                    the host's flags (rt_ui.cpp), per human; the rules read them (CvGame, CvUnitMission, CvNotifications)
//  R    isForcePopup                       the server's answer (generated stub / UserInterface default): nothing of the replica's UI changes a branch of the rules
//  R    setForcePopup                      the server's answer (generated stub / UserInterface default): nothing of the replica's UI changes a branch of the rules
//  I    lookAtCityOffset                   sent to the sink as an event
//  I    getPlotListColumn                  exe frame or display only: nothing to keep, nothing to send
//  I    verifyPlotListColumn               exe frame or display only: nothing to keep, nothing to send
//  I    getPlotListOffset                  exe frame or display only: nothing to keep, nothing to send
//  I    exitingToMainMenu                  exe frame or display only: nothing to keep, nothing to send
//  R    isInAdvancedStart                  the server's answer (generated stub / UserInterface default): nothing of the replica's UI changes a branch of the rules
//  R    setInAdvancedStart                 the server's answer (generated stub / UserInterface default): nothing of the replica's UI changes a branch of the rules
//  I    setBusy                            exe frame or display only: nothing to keep, nothing to send
//  I    UpdateCountryBorder                sent to the sink as an event
//  I    UpdateAllCountryBorders            sent to the sink as an event
//  I    DebugCityRoutePlots                exe frame or display only: nothing to keep, nothing to send
//  I    DirectionAction                    exe frame or display only: nothing to keep, nothing to send
//  I    SetSpecificCityInfoDirty           sent to the sink as an event
//  I    AddHexToUIRange                    sent to the sink as an event
//  I    OpenEndGameMenu                    sent to the sink as an event
//  I    doTurn                             exe frame or display only: nothing to keep, nothing to send
//  R    IsOptionNoRewardPopups             the server's answer (generated stub / UserInterface default): nothing of the replica's UI changes a branch of the rules
//  R    IsAIRequestingConcessions          the server's answer (generated stub / UserInterface default): nothing of the replica's UI changes a branch of the rules
//  R    SetAIRequestingConcessions         the server's answer (generated stub / UserInterface default): nothing of the replica's UI changes a branch of the rules
//  R    IsHumanMakingDemand                the server's answer (generated stub / UserInterface default): nothing of the replica's UI changes a branch of the rules
//  R    SetHumanMakingDemand               the server's answer (generated stub / UserInterface default): nothing of the replica's UI changes a branch of the rules
//  R    GetStartDiploRepeatCount           the server's answer (generated stub / UserInterface default): nothing of the replica's UI changes a branch of the rules
//  R    SetStartDiploRepeatCount           the server's answer (generated stub / UserInterface default): nothing of the replica's UI changes a branch of the rules
//  R    ChangeStartDiploRepeatCount        the server's answer (generated stub / UserInterface default): nothing of the replica's UI changes a branch of the rules
//  R    GetOfferTradeRepeatCount           the server's answer (generated stub / UserInterface default): nothing of the replica's UI changes a branch of the rules
//  R    SetOfferTradeRepeatCount           the server's answer (generated stub / UserInterface default): nothing of the replica's UI changes a branch of the rules
//  R    ChangeOfferTradeRepeatCount        the server's answer (generated stub / UserInterface default): nothing of the replica's UI changes a branch of the rules
//  I    OpenTechTree                       sent to the sink as an event
//  I    OpenPlayerDealScreen               sent to the sink as an event
//  R    IsPolicyNotificationSeen           the host's flags (rt_ui.cpp), per human; the rules read them (CvGame, CvUnitMission, CvNotifications)
//  R    SetPolicyNotificationSeen          the host's flags (rt_ui.cpp), per human; the rules read them (CvGame, CvUnitMission, CvNotifications)
//  R    IsForceDiscussionModeQuitOnBack    the server's answer (generated stub / UserInterface default): nothing of the replica's UI changes a branch of the rules
//  R    SetForceDiscussionModeQuitOnBack   the server's answer (generated stub / UserInterface default): nothing of the replica's UI changes a branch of the rules
//  I    SetTempString                      sent to the sink as an event
//  I    PublishEndTurnDirty                sent to the sink as an event
//  I    AddDeferredWonderCommand           sent to the sink as an event
//  R    IsLoadedGame                       the server's answer (generated stub / UserInterface default): nothing of the replica's UI changes a branch of the rules
//  I    ToggleStrategicView                sent to the sink as an event
//  R    IsPopupQueueEmpty                  the server's answer (generated stub / UserInterface default): nothing of the replica's UI changes a branch of the rules
//  R    IsModalStackEmpty                  the server's answer (generated stub / UserInterface default): nothing of the replica's UI changes a branch of the rules
//  R    IsDontShowPopups                   the server's answer (generated stub / UserInterface default): nothing of the replica's UI changes a branch of the rules
//  R    SetDontShowPopups                  the server's answer (generated stub / UserInterface default): nothing of the replica's UI changes a branch of the rules
//  R    IsDealInTransit                    the server's answer (generated stub / UserInterface default): nothing of the replica's UI changes a branch of the rules
//  R    SetDealInTransit                   the server's answer (generated stub / UserInterface default): nothing of the replica's UI changes a branch of the rules
//  R    IsSPAutoEndTurnEnabled             false: CvGame::updateTestEndTurn must not end the turn by itself
//  R    SetSPAutoEndTurnEnabled            false: CvGame::updateTestEndTurn must not end the turn by itself
//  R    IsMPAutoEndTurnEnabled             false: CvGame::updateTestEndTurn must not end the turn by itself
//  R    SetMPAutoEndTurnEnabled            false: CvGame::updateTestEndTurn must not end the turn by itself
//  R    IsSPQuickCombatEnabled             the server's answer (generated stub / UserInterface default): nothing of the replica's UI changes a branch of the rules
//  R    SetSPQuickCombatEnabled            the server's answer (generated stub / UserInterface default): nothing of the replica's UI changes a branch of the rules
//  R    IsMPQuickCombatEnabled             the server's answer (generated stub / UserInterface default): nothing of the replica's UI changes a branch of the rules
//  R    SetMPQuickCombatEnabled            the server's answer (generated stub / UserInterface default): nothing of the replica's UI changes a branch of the rules
//  I    PublishRemotePlayerTurnStart       sent to the sink as an event
//
#pragma once

#include <string.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "../host/PortableHost.h"
#include "gamecore_abi.h"
#include "rt_ui.h"

namespace lekmod_portable {

class RtScriptArgs;

// One event under construction (stack object). A(...) appends an integer argument (up to GC_UI_MAX_ARGS), T / T2 / D set the strings and the data blob.
struct RtUiEv
{
	gc_ui_event e;
	explicit RtUiEv(int kind)
	{
		memset(&e, 0, sizeof(e));
		e.kind = kind;
	}
	RtUiEv& A(int v)
	{
		if (e.n_args < GC_UI_MAX_ARGS) e.args[e.n_args++] = v;
		return *this;
	}
	RtUiEv& T(const char* t) { e.text = t; return *this; }
	RtUiEv& T2(const char* t) { e.text2 = t; return *this; }
	RtUiEv& D(const uint8_t* p, size_t n) { e.data = p; e.data_len = (uint32_t)n; return *this; }
};

class RtUiReplica
{
public:
	RtUiReplica();

	// ---- configuration ----
	void SetSink(const gc_ui_sink* sink);        // NULL clears
	// gc_set_role: the replica role (events and interface state work only in it) and the seat whose UI it serves (NO_PLAYER = a spectator).
	void SetRole(bool replica, PlayerTypes seat);
	bool IsReplica() const { return m_replica; }
	PlayerTypes Seat() const { return m_seat; }
	// Events go out: replica role and a sink.
	bool Active() const { return m_replica && !m_muted && m_sink.event != NULL; }
	// Off during gc_resync: the old game being torn down and the new one loaded are not news for the UI (gc_ui_setup_graphical announces the new one).
	void SetMuted(bool on) { m_muted = on; }
	// Events produced while on carry GC_UI_F_REBUILD (gc_ui_setup_graphical).
	void SetRebuild(bool on) { m_rebuild = on; }
	void Subscribe(const char* name) { m_subscribed.insert(name); }
	bool Subscribed(const char* name) const { return m_subscribed.find(name) != m_subscribed.end(); }

	void Send(RtUiEv& ev);

	// ---- packing (the integers the exe reads at the moment of the callback) ----
	// The reads are those of the seat's UI: the seat's team is the active one for the call.
	void PackUnit(RtUiEv& ev, ICvUnit1* unit);
	void PackCity(RtUiEv& ev, ICvCity1* city);   // sets ev's text to the city's name key (valid while the city is)
	static void PlotXY(const ICvPlot1* plot, int* x, int* y);
	void PackCombat(RtUiEv& ev, ICvCombatInfo1* info);

	// ---- Lua events ----
	// GameEvents.<name>(args) after the core's listeners (only for subscribed names).
	void EmitGameEvent(const char* name, RtScriptArgs* args);
	// Events.<name>(...) called from the core's Lua: the values at stack indices first..last of L.
	void EmitLuaEvent(const char* name, struct lua_State* L, int first, int last);
	// The same for GameEvents.<name>(...) called from the core's Lua (kind GC_UI_EV_GAME_EVENT; only for subscribed names).
	void EmitLuaValues(int kind, const char* name, struct lua_State* L, int first, int last);

	// ---- interface state (replica role) ----
	// Units and cities are (owner, id); the CvUnit / CvCity are looked up on use, so a stale entry is a missing one.
	bool IsUnitSelected(PlayerTypes owner, int id) const;
	void SelectUnit(CvUnit* unit, bool clear, bool toggle);
	void SelectGroup(CvUnit* unit, bool shift, bool ctrl, bool alt);
	void SelectAllAt(CvPlot* plot);
	bool RemoveFromSelection(PlayerTypes owner, int id);
	void ClearSelection();
	int SelectionLength() const { return (int)m_selected.size(); }
	CvUnit* HeadSelectedUnit();
	CvUnit* LastSelectedUnit();
	void SetLastSelected(PlayerTypes owner, int id) { m_last = IDInfo(owner, id); }
	const std::vector<IDInfo>& Selected() const { return m_selected; }
	bool CanSelectionListFound();
	bool CanSelectionListWork();

	void SelectCity(CvCity* city);
	void AddSelectedCity(CvCity* city, bool toggle);
	void ClearSelectedCities();
	bool IsCitySelected(PlayerTypes owner, int id) const;
	CvCity* HeadSelectedCity();
	std::vector<IDInfo>& SelectedCities() { return m_cities; }
	void SetCityScreenUp(bool up);
	bool CityScreenUp() const { return m_cityScreenUp; }

	void SetInterfaceMode(InterfaceModeTypes mode);
	InterfaceModeTypes InterfaceMode() const { return m_mode; }
	void SetInterfaceModeValue(int value) { m_modeValue = value; }
	int InterfaceModeValue() const { return m_modeValue; }
	bool CanDoInterfaceMode(InterfaceModeTypes mode, bool testVisible);

	int CycleCounter() const { return m_cycle; }
	void SetCycleCounter(int v) { m_cycle = v; }
	bool SelectedUnitRevealingNewPlots() const { return m_revealing; }
	void SetSelectedUnitRevealingNewPlots(bool v) { m_revealing = v; }

	// After a load or resync: drops what no longer exists (units and cities were recreated), announces the change.
	void Revalidate();
	// A new game or a resync from nothing: nothing is selected, the mode is selection.
	void ResetState();

	// The notifications of the seat as the previous SyncReplicaNotifications saw them; the ones the UI removed itself (gc_ui_op REMOVE_NOTIFICATION) while the core
	// still has them (the dismissed flag is the server's) are not announced again.
	std::map<int, RtNotification>& SeenNotifications() { return m_seenNotes; }
	std::set<int>& HiddenNotifications() { return m_hiddenNotes; }

	// The fog of war of the seat's team, plot by plot, against the previous call: FOW_CHANGED for each plot whose mode changed (the first call announces every plot that is not
	// unexplored). The DLL's own GameplayFOWChanged works out the mode for the active team, which during a replay is the server seat's, so it is not used.
	void SyncFog();
	void ForgetFog() { m_fog.clear(); }

private:
	void SelectionChanged();
	void CitySelectionChanged();

	bool m_replica;
	PlayerTypes m_seat;
	gc_ui_sink m_sink;
	bool m_rebuild;
	bool m_muted;
	std::set<std::string> m_subscribed;
	std::vector<IDInfo> m_selected;
	IDInfo m_last;
	std::vector<IDInfo> m_cities;
	InterfaceModeTypes m_mode;
	int m_modeValue;
	bool m_cityScreenUp;
	int m_cycle;
	bool m_revealing;
	std::map<int, RtNotification> m_seenNotes;
	std::set<int> m_hiddenNotes;
	std::vector<unsigned char> m_fog;
};

}  // namespace lekmod_portable
