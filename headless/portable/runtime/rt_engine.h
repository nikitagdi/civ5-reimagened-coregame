// rt_engine: the EngineUtility the runtime hands to the gamecore (gDLL). Overrides the generated stubs that
// would stall or trap a real game: script system, virtual step time, CanAdvanceTurn, turns-per-minute, cache folder,
// and (rt_net.cpp) the single-player loopback: every send* command is queued and delivered to ICvNetMessageHandler3
// before the next Update().
// Ported from headless/host/src/engine/HstEngineUtility.cpp and net/HstLoopback.cpp (single-player subset).
#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "../host/PortableHost.h"
#include "rt_net.h"

namespace lekmod_portable {

class RtDiplo;
class RtEventQueue;
class RtUiReplica;

class RtEngine : public EngineUtility
{
public:
	// One Update() slice of virtual time. The exe value is not known; CvGame::getMinutesPlayed
	// (turnSlice / getTurnsPerMinute) and the combat mission timer only need it constant and non-zero.
	static const int STEP_MS = 250;

	RtEngine();

	// The Lua host (rt_lua) installs itself here; NULL until then.
	void SetScriptSystem(ICvEngineScriptSystem1* p) { m_scriptSystem = p; }
	virtual ICvEngineScriptSystem1* GetScriptSystem();

	// Net handler and pregame of the loaded gamecore (after ctx->Init()); needed by the loopback.
	void SetContext(ICvGameContext1* ctx);

	// Virtual clock: moves only when the runtime calls AdvanceStep(), never with wall time.
	void AdvanceStep() { m_virtualMs += STEP_MS; }
	unsigned int VirtualTimeMs() const { return m_virtualMs; }
	void SetVirtualTimeMs(unsigned int ms) { m_virtualMs = ms; }

	virtual int getMillisecsPerTurn();
	virtual float getSecsPerTurn();
	virtual int getTurnsPerSecond();
	virtual int getTurnsPerMinute();

	// CvMap::init / reset notify the renderer; nothing to do headless (HstEngineUtility::DoMapSetup).
	virtual void DoMapSetup(int iWorldNumPlots);

	// The AI's leader messages to the local human are queued there and answered by the host (F6, SIMP-003); NULL = dropped, as the stub did.
	void SetDiplo(RtDiplo* p) { m_diplo = p; }
	virtual void GameplayDiplomacyAILeaderMessage(PlayerTypes ePlayer, DiploUIStateTypes eDiploUIState, const char* szLeaderMessage, LeaderheadAnimationTypes eAction, int iData1);

	// GC_EVENT_WAR_STATE (F7): the queue gets one event per change of a team pair's war state (CvTeam::setAtWar also reports unchanged values).
	void SetEvents(RtEventQueue* p) { m_events = p; }
	virtual void GameplayWarStateChanged(TeamTypes eTeam1, TeamTypes eTeam2, bool bWar);

	// Single player has no turn barrier: CvGame::update may always run doTurn. Network (spike S1): the exe's rule (rt_net.cpp).
	virtual bool CanAdvanceTurn();

	// ---- network game type (spike S1; rt_net.cpp) ----
	// The exe turn model of civhost (headless/host/src/net/HstLoopback.cpp) folded into one
	// machine: the server is the peer of every human, so NetTurnAllComplete and NetAICivsProcessed "arrive from every human" at once and the
	// NetLoopBarrier closes as soon as it is entered. Off (single player) the handshake is the B1 one, unchanged.
	void SetNetworkGame(bool on) { m_network = on; }
	bool NetworkGame() const { return m_network; }
	// The exe's GameViewState frame (0x3253e0), run by RtSession::Pump after every Update (the gamecore is not executing then).
	void ExeFrame();
	// Per player "turn complete sent" (HasSentTurnComplete is for the active player only).
	bool TurnCompleteSent(PlayerTypes p) const { return p >= 0 && p < MAX_PLAYERS && m_turnCompleteSent[p]; }
	// The connection state of a human slot (Q_TURN CONNECTED, IsPlayerConnected), set by gc_slot_control (rt_slots.cpp, package P8): on line = a seated human
	// whose connection flag is set. A new game starts with every seated human on line; the connection is not saved: after a load nobody is,
	// and the host's CONNECT brings each human back.
	bool SlotConnected(PlayerTypes p) const;
	void SetSlotConnected(PlayerTypes p, bool on) { if (p >= 0 && p < MAX_PLAYERS) { m_connected[p] = on; m_offlineByLoad[p] = false; } }
	// The slot is off line only because the game was loaded (set by ClearSlotConnections, cleared by any SetSlotConnected): the game state still is the saved one, in which
	// the human was on line, so its first CONNECT must not run CvPlayer::reconnected (it would add PLAYER_CONNECTING notifications to the server seat).
	bool SlotOfflineByLoad(PlayerTypes p) const { return p >= 0 && p < MAX_PLAYERS && m_offlineByLoad[p]; }
	// B4 gc-replica: a replica takes both flags of a slot from the server's snapshot (gc_abi.cpp ApplySavedConnections).
	void SetSlotState(PlayerTypes p, bool connected, bool offlineByLoad) { if (p >= 0 && p < MAX_PLAYERS) { m_connected[p] = connected; m_offlineByLoad[p] = offlineByLoad; } }
	void InitSlotConnections();   // every seated human (SS_TAKEN) on line, nobody kicked: a new game
	void ClearSlotConnections();  // nobody on line (kicks stay): after a load
	// The room host kicked the slot (IsPlayerKicked answers it to CvPlayer::disconnected); saved. A CONNECT onto the slot clears it.
	bool SlotKicked(PlayerTypes p) const { return p >= 0 && p < MAX_PLAYERS && m_kicked[p]; }
	void SetSlotKicked(PlayerTypes p, bool on) { if (p >= 0 && p < MAX_PLAYERS) m_kicked[p] = on; }
	// The end of turn of a human whose turn is not active (a slot taken over after its AI moved): marked as sent and received without a message, so the turn does
	// not wait for it (GC_EVENT_PLAYER_TURN_ENDED is pushed).
	void MarkTurnEnded(PlayerTypes p);
	int BarrierTurn() const { return m_barrierTurn; }
	int AllAIProcessedTurn() const { return m_allAIProcessedTurn; }
	// Calls of exe methods the gamecore made that the runtime only answers with a fixed value or drops (S1 report), by method name; and
	// queued messages nobody delivers (no ICvNetMessageHandler3 response), by method name.
	const std::map<std::string, int>& ExeCalls() const { return m_exeCalls; }
	const std::map<std::string, int>& Dropped() const { return m_dropped; }

	virtual bool IsHost() const;
	virtual bool IsPlayerConnected(PlayerTypes playerId);
	virtual bool IsPlayerKicked(int iPlayerID);
	virtual bool IsPlayerHotJoining(int iPlayerID);
	virtual bool IsPitbossHost() const;
	virtual int getAssignedNetworkID(int iPlayerID);
	virtual void sendPlayerInfo(PlayerTypes eActivePlayer);
	virtual void sendPlayerOption(PlayerOptionTypes eOption, bool bValue);
	virtual void sendExtendedGame();
	virtual void sendPause(int iPauseID);
	virtual void sendTurnReminder(PlayerTypes toPlayer);
	virtual void FlushTurnReminders();
	virtual void sendPbemTurn(PlayerTypes ePlayer);
	virtual void sendNetDealAccepted(PlayerTypes fromPlayer, PlayerTypes toPlayer, ICvDeal1* pDeal, int iDealValueToMe, int iValueImOffering, int iValueTheyreOffering) const;
	virtual void sendNetDemandAccepted(PlayerTypes fromPlayer, PlayerTypes toPlayer, ICvDeal1* pDeal) const;
	virtual void hotJoinComplete() const;
	virtual void netDisconnect();
	virtual void NotifySpecificAILeaderInGame(PlayerTypes ePlayer);
	virtual void ReseatPlayer(PlayerTypes p, bool bClosePlayersOldSlot) const;
	virtual bool ReseatConnectedPlayer(PlayerTypes p) const;
	virtual bool IsNetPlayer(PlayerTypes p) const;
	virtual void SendGameDoTurnProcessed() const;
	virtual void VerifyPlayerSlot(PlayerTypes ePlayer) const;
	virtual void openSlot(PlayerTypes eID);
	virtual void closeSlot(PlayerTypes eID);
	virtual void sendUnitSyncCheck(PlayerTypes playerId, int unitId, FMemoryStream& syncArchiveData, const std::vector<std::pair<std::string, std::string> >& p3) const;
	virtual void sendPlotSyncCheck(PlayerTypes playerId, short plotX, short plotY, FMemoryStream& syncArchiveData, const std::vector<std::pair<std::string, std::string> >& p4) const;
	virtual void sendCitySyncCheck(PlayerTypes playerId, int cityId, FMemoryStream& syncArchiveData, const std::vector<std::pair<std::string, std::string> >& p3) const;
	virtual void sendRandomNumberGeneratorSyncCheck(PlayerTypes playerId, ICvRandom1* pRandomNumberGenerator) const;
	virtual void sendPlayerSyncCheck(PlayerTypes playerId, FMemoryStream& syncData, const std::vector<std::pair<std::string, std::string> >& p2) const;

	// Folder of ./Civ5SavedGameDatabase.db; rt_fs will serve it from memory.
	virtual const char* GetCacheFolderPath();

	// ---- loopback (rt_net.cpp) ----
	PlayerTypes GetActivePlayer() const;
	// Queues a command as if the DLL had sent it (host-side injection: human orders, UI choices).
	RtNetMessage& Queue(int id, PlayerTypes player);
	// The archaeology popup's command: Lua Network.SendArchaeologyChoice(player, unit index, choice), an exe-only message (no gDLL sender); delivered as
	// ResponseArchaeologyChoice(player, choice) (the unit index is not used: the gamecore takes the next dig-complete archaeologist).
	void SendArchaeologyChoice(PlayerTypes ePlayer, int iUnitIndex, ArchaeologyChoiceType eChoice);
	// Delivers the queued commands to ICvNetMessageHandler3 (commands sent by the responses wait for the next call); returns how many.
	int DispatchNetMessages();
	int NetQueueSize() const { return (int)m_netQueue.size(); }
	// B4 M4 WO-D: an accepted human offer / demand arrives (kRtNet_sendNetDealAccepted / sendNetDemandAccepted).
	void DeliverDeal(const RtNetMessage& m);
	// B4 fix-mp-trade: a deal between two humans. The exe sends NetPlayerDealProposed when the UI proposes a deal to a human (UI.DoProposeDeal) and
	// NetPlayerDealFinalized when a side accepts, refuses or withdraws it (UI.DoFinalizePlayerDeal); every machine stores the proposal
	// (CvGameDeals::AddProposedDeal, the PLAYER_DEAL / PLAYER_DEAL_RECEIVED notifications) and resolves it (ResponsePlayerDealFinalized).
	void SendNetDealProposed(PlayerTypes from, PlayerTypes to, ICvDeal1* pDeal);
	void SendNetPlayerDealFinalized(PlayerTypes from, PlayerTypes to, bool bAccepted);
	void DeliverPlayerDeal(const RtNetMessage& m);
	// True while the core works for itself (Pump, replay): the leader messages then mirror the server's; outside it a replica's UI called the DLL.
	bool InCore() const { return m_coreDepth > 0; }
	bool IsReplicaUiCall() const { return m_outboxOn && m_coreDepth == 0; }
	// B4 gc-replica: a replica's UI must not change its copy of the game. With the outbox on, a message queued outside the
	// core's own work (RtSession::Pump, a replay of the server's inputs) is what the UI sent: it goes to the outbox for the server, not to the loopback.
	void SetOutbox(bool on) { m_outboxOn = on; }
	void EnterCore() { ++m_coreDepth; }
	void LeaveCore() { --m_coreDepth; }
	void TakeOutbox(std::vector<RtNetMessage>& out) { out.swap(m_outbox); m_outbox.clear(); }
	// A message of another machine (a human's UI through the server, or the server's journal on a replica): delivered by the next slice like the local ones.
	void Inject(const RtNetMessage& m) { m_netQueue.push_back(m); }
	// gc_resync: the loopback and outbox start empty; the saved runtime state is read after this.
	void ClearNetQueues() { m_netQueue.clear(); m_netDelivering.clear(); m_outbox.clear(); }
	int NetMessagesDelivered() const { return m_netDelivered; }

	virtual void DoTurn();
	virtual void sendTurnComplete();
	virtual bool HasSentTurnComplete();
	virtual bool HasReceivedTurnComplete(PlayerTypes ePlayer);
	virtual bool HasSentTurnAllComplete();
	virtual bool HasReceivedTurnAllComplete(PlayerTypes ePlayer);
	virtual bool HasReceivedTurnAllCompleteFromAllPlayers();
	virtual bool sendTurnUnready();
	virtual void sendAutoMoves();
	virtual void sendPlayerInitialAIProcessed() const;
	virtual bool allAIProcessedThisTurn() const;
	virtual bool allAICivsProcessedThisTurn() const;
	virtual void SendAICivsProcessed() const;
	virtual bool HasReceivedAICivsProcessed(PlayerTypes ePlayer) const;
	virtual int GetNumAICivsProcessed() const;
	virtual void SendFoundPantheon(PlayerTypes ePlayer, BeliefTypes eBelief);
	virtual void SendFoundReligion(PlayerTypes ePlayer, ReligionTypes eReligion, const char* szCustomName, BeliefTypes eBelief1, BeliefTypes eBelief2, BeliefTypes eBelief3, BeliefTypes eBelief4, int iCityX, int iCityY);
	virtual void SendEnhanceReligion(PlayerTypes ePlayer, ReligionTypes eReligion, const char* szCustomName, BeliefTypes eBelief1, BeliefTypes eBelief2, int iCityX, int iCityY);
	virtual void SendMoveSpy(PlayerTypes ePlayer, int iSpyIndex, int iTargetPlayer, int iTargetCity, bool bAsDiplomat);
	virtual void SendStageCoup(PlayerTypes eSpyPlayer, int iSpyIndex);
	virtual void SendFaithPurchase(PlayerTypes ePlayer, FaithPurchaseTypes eFaithPurchaseType, int iFaithPurchaseIndex);
	virtual void sendPushMission(int iUnitID, MissionTypes eMission, int iData1, int iData2, int iFlags, bool bShift);
	virtual void sendAutoMission(int iUnitID);
	virtual void sendDoCommand(int iUnitID, CommandTypes eCommand, int iData1, int iData2, bool bAlt);
	virtual void sendPushOrder(int iCityID, OrderTypes eOrder, int iData, bool bAlt, bool bShift, bool bCtrl);
	virtual void sendPopOrder(int iCity, int iNum);
	virtual void sendSwapOrder(int iCity, int iNum0);
	virtual void sendPurchase(int iCity, UnitTypes eUnitType, BuildingTypes eBuildingType, ProjectTypes eProjectType);
	virtual void sendPurchase(int iCity, UnitTypes eUnitType, BuildingTypes eBuildingType, ProjectTypes eProjectType, int ePurchaseYield);
	virtual void sendDoTask(int iCity, TaskTypes eTask, int iData1, int iData2, bool bOption, bool bAlt, bool bShift, bool bCtrl);
	virtual void sendResearch(TechTypes eTech, int iDiscover, PlayerTypes ePlayerToStealFrom, bool bShift);
	virtual void sendChangeWar(TeamTypes iRivalTeam, bool bWar);
	virtual void sendIgnoreWarning(TeamTypes iRivalTeam);
	virtual void SendPledgeMinorProtection(PlayerTypes eMinor, bool bValue);
	virtual void SendMinorNoUnitSpawning(PlayerTypes eMinor, bool bValue);
	virtual void SendLiberateMinor(PlayerTypes eMinor, int iCityID);
	virtual void sendUpdatePolicies(bool bNOTPolicyBranch, int iID, bool bValue);
	virtual void SendDiploVote(PlayerTypes eVotePlayer);
	virtual void sendLaunch(PlayerTypes ePlayer, VictoryTypes eVictory);
	virtual void sendAdvancedStartAction(AdvancedStartActionTypes eAction, PlayerTypes ePlayer, int iX, int iY, int iData, bool bAdd);
	virtual void sendMinorCivQuestNoInterest(PlayerTypes eMinor, bool bPeace);
	virtual void sendMinorCivQuestCompleted(PlayerTypes eMinor, bool bPeace);
	virtual void sendMinorCivIntrusion(PlayerTypes eMinor, int iOptionChosen);
	virtual void sendMinorCivEnterTerritory(PlayerTypes eMinor);
	virtual void sendBarbarianRansom(int iOptionChosen, int iUnitID);
	virtual void sendGiftUnit(PlayerTypes eMinor, int iUnitID);
	virtual void sendReturnCivilian(bool bReturn, PlayerTypes ePlayer, int iUnitID);
	virtual void SendUpdateCityCitizens(int iCityID);
	virtual void SendSellBuilding(int iCityID, BuildingTypes eBuilding);
	virtual void SendRenameCity(int iCityID, CvString szNewName);
	virtual void SendRenameUnit(int iUnitID, CvString szNewName);
	virtual void sendPlayerHurry(HurryTypes eHurry);
	virtual void sendSwapUnits(int iUnitID, MissionTypes eMission, int iData1, int iData2, int iFlags, bool bShift);
	virtual void sendCityBuyPlot(int iCityID, int iX, int iY);
	virtual void sendMinorPledgeProtection(PlayerTypes eMajor, PlayerTypes eMinor, bool bProtect, bool bPledgeNowBroken);
	virtual void sendMinorGiftGold(PlayerTypes eMinor, int iGold);
	virtual void sendMinorGiftTileImprovement(PlayerTypes eMajor, PlayerTypes eMinor, int iPlotX, int iPlotY);
	virtual void sendMinorBullyGold(PlayerTypes eBully, PlayerTypes eMinor, int iGold);
	virtual void sendMinorBullyUnit(PlayerTypes eBully, PlayerTypes eMinor, UnitTypes eUnitType);
	virtual void sendMinorBuyout(PlayerTypes eMajor, PlayerTypes eMinor);
	virtual void sendGreatPersonChoice(PlayerTypes ePlayer, UnitTypes eUnit);
	virtual void sendMayaBonusChoice(PlayerTypes ePlayer, UnitTypes eUnit);
	virtual void sendFaithGreatPersonChoice(PlayerTypes ePlayer, UnitTypes eUnit);
	virtual void sendSetCityAIFocus(int iCityID, CityAIFocusTypes eFocus);
	virtual void sendSetCityAvoidGrowth(int iCityID, bool bAvoidGrowth);
	virtual void sendFromUIDiploEvent(PlayerTypes eOtherPlayer, FromUIDiploEventTypes eEvent, int iArg1, int iArg2);
	virtual void sendGoodyChoice(PlayerTypes ePlayerID, int iPlotX, int iPlotY, GoodyTypes eGoody, int iUnitID);
	virtual void sendSetSwappableGreatWork(PlayerTypes ePlayer, int iWorkClass, int iWorkIndex);
	virtual void sendSwapGreatWorks(PlayerTypes ePlayer1, int iWork1, PlayerTypes ePlayer2, int iWork2);
	virtual void sendMoveGreatWorks(PlayerTypes ePlayer, int iCity1, int iBuildingClass1, int iWorkIndex1, int iCity2, int iBuildingClass2, int iWorkIndex2);
	virtual void SendLeagueVoteEnact(LeagueTypes eLeague, int iResolutionID, PlayerTypes eVoter, int iNumVotes, int iChoice);
	virtual void SendLeagueVoteRepeal(LeagueTypes eLeague, int iResolutionID, PlayerTypes eVoter, int iNumVotes, int iChoice);
	virtual void SendLeagueVoteAbstain(LeagueTypes eLeague, PlayerTypes eVoter, int iNumVotes);
	virtual void SendLeagueProposeEnact(LeagueTypes eLeague, ResolutionTypes eResolution, PlayerTypes eProposer, int iChoice);
	virtual void SendLeagueProposeRepeal(LeagueTypes eLeague, int iResolutionID, PlayerTypes eProposer);
	virtual void SendLeagueEditName(LeagueTypes eLeague, PlayerTypes ePlayer, const char* szCustomName);
	virtual void sendChangeIdeology();

	// B4 M2 WO-1: the Gameplay* / Publish* callbacks a replica's UI is told (packed into gc_ui_event by RtUiReplica); every other role keeps the generated stub.
	void SetUiReplica(RtUiReplica* p) { m_uiRep = p; }
	virtual void GameplayEraChanged(PlayerTypes playerID, EraTypes newEra);
	virtual void GameplayBridgeChanged(bool bBridge, unsigned char newBridgeStyle);
	virtual void GameplayUnitCreated(ICvUnit1* pThisUnit);
	virtual void GameplayUnitMoved(ICvUnit1* pThisUnit, const CvPlotIndexVector& kPlots);
	virtual void GameplayUnitTeleported(ICvUnit1* pThisUnit, ICvPlot1* pThisPlot);
	virtual void GameplayUnitDestroyed(ICvUnit1* pThisUnit);
	virtual void GameplayUnitDestroyedInCombat(ICvUnit1* pUnit);
	virtual void GameplayUnitSetDamage(ICvUnit1* pThisUnit, int iDamage, int iPreviousDamage);
	virtual void GameplayUnitFortify(ICvUnit1* pThisUnit, bool bFortify);
	virtual void GameplayUnitWork(ICvUnit1* pThisUnit, int iWorkType);
	virtual void GameplayUnitParadrop(ICvUnit1* pThisUnit);
	virtual void GameplayUnitActivate(ICvUnit1* pThisUnit);
	virtual void GameplayUnitEmbark(ICvUnit1* pThisUnit, bool bEmbark);
	virtual void GameplayUnitGarrison(ICvUnit1* pThisUnit, bool bGarrison);
	virtual void GameplayUnitShouldDimFlag(ICvUnit1* pThisUnit, bool bDim);
	virtual void GameplayUnitMarkThreatening(ICvUnit1* pThisUnit, bool bMark);
	virtual void GameplayUnitVisibility(ICvUnit1* pThisUnit, bool bVisible, bool bChangeInvisibility, const float fBlendTime);
	virtual uint GameplayUnitCombat(ICvCombatInfo1* pCombatInfo);
	virtual void GameplayCityCreated(ICvCity1* pThisCity, EraTypes eCurrentEra);
	virtual void GameplayCityDestroyed(ICvCity1* pThisCity, PlayerTypes eNewOwner);
	virtual void GameplayCityCaptured(ICvCity1* pThisCity, PlayerTypes eNewOwner);
	virtual void GameplayCitySetDamage(ICvCity1* pThisCity, int iDamage, int iPreviousDamage);
	virtual void GameplayCityPopulationChanged(ICvCity1* pThisCity, int newPopulation);
	virtual uint GameplayCityCombat(ICvCombatInfo1* pInfo);
	virtual void GameplayUnitMissionEnd(ICvUnit1* pThisUnit);
	virtual void GameplayUnitRebased(ICvUnit1* pThisUnit, ICvPlot1* pOldPlot, ICvPlot1* pThisPlot);
	virtual void GameplayUnitResetAnimationState(ICvUnit1* pThisUnit);
	virtual void GameplayTechAcquired(TeamTypes eTeam, TechTypes eTech);
	virtual void GameplayFeatureChanged(ICvPlot1* pThisPlot, FeatureTypes newFeature);
	virtual void GameplayPlotStateChange(const ICvPlot1* pThisPlot, const ResourceTypes eRevealedResource, const ImprovementTypes eRevealedImprovement, const int iRevealedImprovementState, const RouteTypes eRevealedRoute, const int iRouteState);
	virtual void GameplayPlotIconStateChange(const ICvPlot1* pThisPlot, const ResourceTypes eRevealedResource, const ImprovementTypes eRevealedImprovement, const int iRevealedImprovementState);
	virtual void GameplayNaturalWonderRevealed(ICvPlot1* pThisPlot);
	virtual void GameplayWallCreated(ICvPlot1* pThisPlot);
	virtual void GameplayDoFX(ICvPlot1* pThisPlot, int iFXType);
	virtual void GameplayFOWChanged(int x, int y, int eFOWType, bool bSetAll);
	virtual void GameplayYieldMightHaveChanged(ICvPlot1* pThisPlot);
	virtual void NotifyAILeadersInGame();
	virtual void DoClearDiplomacyTradeTable();
	virtual void GameplayMetTeam(TeamTypes eMyTeam, TeamTypes eTheirTeam);
	virtual void GameplayWonderCreated(PlayerTypes ePlayerID, ICvPlot1* pThisPlot, BuildingTypes eBuildingIndex, int iState);
	virtual void GameplayWonderEdited(PlayerTypes ePlayerID, ICvPlot1* pThisPlot, BuildingTypes eBuildingIndex, int iState);
	virtual void GameplayWonderRemoved(PlayerTypes ePlayerID, ICvPlot1* pThisPlot, BuildingTypes eBuildingIndex);
	virtual void GameplaySpaceshipCreated(ICvPlot1* pThisPlot, int iState);
	virtual void GameplaySpaceshipEdited(ICvPlot1* pThisPlot, int iState);
	virtual void GameplaySpaceshipRemoved(ICvPlot1* pThisPlot);
	virtual void GameplayOpenOptionsScreen();
	virtual void GameplaySearchForPediaEntry(const char * c);
	virtual void GameplayOpenInfoCorner(int iInfoCornerID);
	virtual void GameplayWorldAnchor(GenericWorldAnchorTypes eAnchor, bool bAdd, int iX, int iY, int iData1);
	virtual void GameplayMinimapUnitSelect(int iX, int iY);
	virtual void GameplayMinimapNotification(int iX, int iY, int iID);
	virtual void GameplayGoldenAgeStarted();
	virtual void GameplayGoldenAgeEnded();
	virtual void GameplayTurnChanged(int iNewTurn);
	virtual void GameplayActivePlayerChanged(PlayerTypes eNewPlayer);
	virtual void PublishActivePlayer(PlayerTypes ePlayer, PlayerTypes eOldPlayer);
	virtual void PublishNewGameTurn(int gameTurn);
	virtual void PublishEraChanges();
	virtual void TradeVisuals_NewRoute(int iRoute, int iPlayer, TradeConnectionType type, int nPlots, int x[], int y[]);
	virtual void TradeVisuals_UpdateRouteDirection(int iRoute, bool bForwards);
	virtual void TradeVisuals_DestroyRoute(int iRoute, int iPlayer);
	virtual void TradeVisuals_ActivatePopupRoute(int iRoute);
	virtual void TradeVisuals_DeactivatePopupRoute();
private:
	friend struct RtSaveAccess;  // rt_save.cpp (G4a): the runtime state a save carries
	void Deliver(const RtNetMessage& m);
	void LogNoResponse(int id);
	void CountExe(const char* name) const { ++m_exeCalls[name]; }
	void CountDropped(const char* name) const { ++m_dropped[name]; }
	int TurnNow() const;
	bool AllHumansTurnComplete() const;

	ICvEngineScriptSystem1* m_scriptSystem;
	unsigned int m_virtualMs;
	RtDiplo* m_diplo;
	RtEventQueue* m_events;
	RtUiReplica* m_uiRep;
	bool m_war[MAX_TEAMS][MAX_TEAMS];  // last reported war state per team pair (index a < b)

	ICvGameContext1* m_ctx;
	ICvPreGame1* m_preGame;                  // from ctx->GetPreGame(), never deleted (as civhost)
	ICvNetMessageHandler3* m_netHandler;     // DLL singleton
	std::vector<RtNetMessage> m_netQueue;
	std::vector<RtNetMessage> m_netDelivering;
	std::vector<RtNetMessage> m_outbox;  // replica: messages the UI sent, for gc_net_take
	bool m_outboxOn;
	int m_coreDepth;                      // > 0 while the core works for itself (Pump, journal replay): its sends stay local
	std::set<int> m_noResponseLogged;
	int m_netDelivered;
	bool m_turnCompleteSent[MAX_PLAYERS];
	bool m_turnCompleteReceived[MAX_PLAYERS];
	mutable bool m_aiCivsProcessedSent;
	mutable bool m_aiCivsProcessedReceived;

	// network (spike S1): the exe model's per-turn net state (civhost names in rt_net.cpp)
	bool m_network;
	int m_turnAllCompleteTurn[MAX_PLAYERS];  // +0x14: turn of the player's last NetTurnAllComplete
	int m_allAIProcessedTurn;                // 0xc98de4: turn for which every human's NetAICivsProcessed arrived (-1 = none)
	int m_barrierTurn;                       // 0x2de0440: turn whose NetLoopBarrier completed (CanAdvanceTurn needs == game turn)
	bool m_sentTurnAllComplete;              // 0x2de0445
	bool m_turnUnreadySent[MAX_PLAYERS];
	bool m_connected[MAX_PLAYERS];           // P8: the slot's human is on line (not saved)
	bool m_offlineByLoad[MAX_PLAYERS];       // see SlotOfflineByLoad (not saved)
	bool m_kicked[MAX_PLAYERS];            // P8: the slot was kicked (saved)
	mutable std::map<std::string, int> m_exeCalls;
	mutable std::map<std::string, int> m_dropped;
};

// The core works for itself while this lives (RtEngine::EnterCore): its net sends go to the loopback, not to a replica's outbox.
struct RtCoreScope
{
	RtEngine& engine;
	explicit RtCoreScope(RtEngine& e) : engine(e) { engine.EnterCore(); }
	~RtCoreScope() { engine.LeaveCore(); }
};

}  // namespace lekmod_portable
