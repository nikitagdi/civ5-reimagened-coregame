//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstEngineUtility.h
//!  \brief     ICvEngineUtility4 for the DLL: hand-written overrides of the sim-class methods.
//!
//!		Everything else (sink/query) comes from gen/HstEngineUtility4Gen (engine-api.csv). Overrides count
//!		their calls with HST_API(id) and cite the exe function when the behaviour comes from reverse engineering.
//!		The turn handshake and the send* -> ICvNetMessageHandler3::Response* loopback live in net/HstLoopback.cpp.
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#pragma once
#ifndef HSTENGINEUTILITY_H
#define HSTENGINEUTILITY_H

#include "engine/gen/HstEngineUtility4Gen.h"

//! One queued loopback command (design 2.2 "commands, looped back into ICvNetMessageHandler3::Response*").
//! eId is the engine API id of the send* method that queued it; arguments are stored as ints in call order.
struct HstNetMessage
{
	HstApiId eId;
	PlayerTypes ePlayer;	// sender: the active player unless the send* method names a player
	int aiArg[9];
	std::string strText;
	// Peer mode (lockstep): exe message header fields (NetMsg +0x1c sender, +0x20 comm turn, +0x24 seq; 02 §2.1).
	int iPeer;				// sending peer (civhost process); 0 in single mode
	int iTurn;				// game turn when sent (host-side view; the coordinator orders by (turn, seq, peer))
	unsigned int uiSeq;		// per-peer send counter
};

//! Lockstep peer mode (control command "peer"). One civhost process = one peer with its own
//! active player. Outgoing send* go to an outbox (the coordinator delivers them to every peer, the sender included, at
//! the next frame, as the exe's NetProxy_Broadcast + loopback do); incoming messages run through the same Deliver path
//! as the single-mode loopback.
struct HstPeerConfig
{
	bool bEnabled;
	int iPeerId;
	int iNumPeers;
	int iHostPeer;				// IsHost() is true only on this peer
	bool bAiSendsOwner;			// false ("all", default): every peer forwards every send* the DLL issues (exe NetProxy_SendGameCommand
								// 0x45e30 has no sender filter). true ("owner"): a send* that names a player other than the local
								// active player is forwarded only by that player's peer, or by the host when no peer owns the player.
	bool bTurnBarrier;			// CanAdvanceTurn waits for the coordinator's grant (all peers asked for the same turn); false: true
	bool bExeTurns;				// turn_model "exe": the exe's MP turn handshake (NetTurnComplete / NetTurnAllComplete /
								// NetAICivsProcessed per human, NetLoopBarrier, CanAdvanceTurn 0x36ef20); see net/HstLoopback.cpp
	bool bTurnSlices;			// with bExeTurns and >= 2 peers: the exe's send queue + NetTurnSlice token (commands leave a peer
								// only in its own turn slice; FNetSession_Broadcast 0x3fc4b0 queues non-immediate messages,
								// NetTurnSlice Execute 0x129600 flushes them); see "turn slices" in net/HstLoopback.cpp
	std::vector<int> aiPeerPlayers;	// active player of every peer (owner rule)
	HstPeerConfig() : bEnabled(false), iPeerId(0), iNumPeers(1), iHostPeer(0), bAiSendsOwner(false), bTurnBarrier(false), bExeTurns(false),
		bTurnSlices(false) {}
};

//! Exe-only net messages (no ICvEngineUtility method; ids above the engine API table). Wire names in HstEngineUtility::MethodName.
enum HstExeNetMessage
{
	kHstNet_TurnAllComplete = NUM_HST_API + 1,	// NetTurnAllComplete (135): exe 0x1adeb0 sends, Execute 0x1c7d60 -> 0x35cba0
	kHstNet_LoopBarrier,						// NetLoopBarrier (112): enter 0x31b5f0, Execute 0x88070
	kHstNet_LoopBarrierComplete,				// NetLoopBarrierComplete (113): Execute 0x7f0d0
	kHstNet_IdeologyChoice,						// Lua Network.SendIdeologyChoice (exe 0x1de7d0 -> exe-only vfunc +0x414)
	kHstNet_ArchaeologyChoice,					// Lua Network.SendArchaeologyChoice (exe 0x227c00 -> exe-only vfunc +0x410)
	kHstNet_TurnSlice,							// NetTurnSlice (115, immediate): exe 0x307d0 sends, Execute 0x129600 (turn slices)
	kHstNet_End
};

class HstEngineUtility : public HstEngineUtility4Gen
{
public:
	HstEngineUtility();

	void SetCacheFolder(const std::string& strUtf8Folder);	// must end with a backslash (I1)
	void SetScriptSystem(ICvEngineScriptSystem1* pkScriptSystem);
	//! Context for the loopback (active player, net message handler, deal AI). Call after ctx->Init().
	void SetContext(ICvGameContext1* pkCtx);

	//! Deliver the queued send* commands to ICvNetMessageHandler3 (host calls it before every ICvGame1::Update).
	//! Returns the number of messages delivered.
	int DispatchNetMessages();
	int GetNumNetMessagesDelivered() const { return m_iNetMessagesDelivered; }
	//! Checkpoint: handshake flags + undelivered commands (net/HstLoopback.cpp).
	void WriteHostState(FDataStream& kStream) const;
	bool ReadHostState(FDataStream& kStream);
	//! New game: clear the handshake flags and the queue.
	void ResetHostState();

	// ---- lockstep peer mode (net/HstLoopback.cpp) ----
	void SetPeerConfig(const HstPeerConfig& kConfig) { m_kPeer = kConfig; }
	const HstPeerConfig& GetPeerConfig() const { return m_kPeer; }
	bool IsPeerMode() const { return m_kPeer.bEnabled; }
	//! The host tells the engine the current game turn before every slice (CanAdvanceTurn barrier, message headers).
	void SetHostTurn(int iTurn) { m_iHostTurn = iTurn; }
	//! Move the messages sent since the last call into akOut (peer mode).
	void TakeOutbox(std::vector<HstNetMessage>& akOut);
	//! Execute one message received from the network (coordinator order); same path as the single-mode loopback.
	void DeliverRemote(const HstNetMessage& kMsg);
	//! Turn for which CanAdvanceTurn was refused since the last grant (-1: none); the coordinator grants it to all peers.
	int GetAdvanceWanted() const { return m_iAdvanceWanted; }
	void GrantAdvance(int iTurn) { m_iAdvanceGranted = iTurn; if(iTurn == m_iAdvanceWanted) m_iAdvanceWanted = -1; }
	//! Queue a command by engine method name as if the DLL had called it on this peer (scripted bot, "inject").
	bool InjectByName(const std::string& strMethod, const std::vector<int>& aiArgs, const std::string& strText, PlayerTypes eFrom, std::string& strError);
	//! Engine API id of an EngineUtility method name or an exe-only message name (NUM_HST_API when unknown).
	static HstApiId MethodId(const std::string& strMethod);
	//! Wire name of a message id (engine method name, or the exe message name for HstExeNetMessage ids).
	static const char* MethodName(HstApiId eId);
	//! Exe-only Lua sends (Network.SendIdeologyChoice / SendArchaeologyChoice): the exe calls its own vfuncs +0x414 / +0x410.
	void SendExeMessage(HstExeNetMessage eMsg, PlayerTypes ePlayer, int iArg0, int iArg1);
	//! turn_model exe: the exe's per-frame turn logic (GameViewState frame 0x3253e0) after an Update slice.
	void ExeFrame();
	bool IsExeTurns() const { return m_kPeer.bEnabled && m_kPeer.bExeTurns; }
	//! Turn slices (exe 0x307d0 Net_TurnSlice_CheckAndSend): after an Update slice, broadcast NetTurnSlice once per slice when no
	//! player is busy. No-op unless bTurnSlices is active.
	void TurnSliceFrame();
	bool IsTurnSlicing() const { return m_bTurnSlicingActive; }
	int GetTurnSliceNumber() const { return m_iTurnSliceNumber; }
	int GetSendQueueSize() const { return (int)m_akSendQueue.size(); }
	int GetTurnSliceFlushes() const { return m_iTurnSliceFlushes; }
	int GetTurnSliceFlushedMessages() const { return m_iTurnSliceFlushedMsgs; }
	int GetBarrierTurn() const { return m_iBarrierTurn; }
	int GetAllAIProcessedTurn() const { return m_iAllAIProcessedTurn; }
	bool SentTurnAllComplete() const { return m_bSentTurnAllComplete; }
	//! Human slot (SS_TAKEN) or an observer slot that is some peer's active player: the exe's "connected human" (0x558ed0).
	bool IsNetHuman(PlayerTypes ePlayer) const;
	bool AllHumansTurnComplete() const;		// exe 0x2de690
	int GetPeerSent() const { return m_iPeerSent; }
	int GetPeerDropped() const { return m_iPeerDropped; }
	int GetPeerReceived() const { return m_iPeerReceived; }
	int GetNumAICivsProcessedPeers() const;
	bool HasTurnCompleteReceived(PlayerTypes ePlayer) const { return ePlayer >= 0 && ePlayer < MAX_PLAYERS && m_abTurnCompleteReceived[ePlayer]; }
	bool AICivsProcessedReceived() const { return m_bAICivsProcessedReceived; }
	bool TurnCompleteSentLocal() const { const PlayerTypes e = GetActivePlayer(); return e >= 0 && e < MAX_PLAYERS && m_abTurnCompleteSent[e]; }
	PlayerTypes GetActivePlayerForHost() const { return GetActivePlayer(); }

	// ---- sim overrides ----
	virtual ICvEngineScriptSystem1* GetScriptSystem();
	virtual void DoMapSetup(int iWorldNumPlots);
	virtual bool IsPlayerConnected(PlayerTypes playerId);
	virtual int getMillisecsPerTurn();
	virtual float getSecsPerTurn();
	virtual int getTurnsPerSecond();
	virtual int getTurnsPerMinute();
	virtual void InitGlobals();
	virtual void UninitGlobals();
	virtual void SaveGame(SaveGameTypes eSaveGame);
	virtual void AutoSave(bool bInitial, bool bPostTurn);
	virtual const char* GetCacheFolderPath();
	virtual bool IsHost() const;

	// ---- turn handshake (net/HstLoopback.cpp) ----
	virtual void DoTurn();
	virtual bool CanAdvanceTurn();
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

	// ---- commands looped back into ICvNetMessageHandler3::Response* (net/HstLoopback.cpp) ----
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
	virtual void sendNetDealAccepted(PlayerTypes fromPlayer, PlayerTypes toPlayer, ICvDeal1* pDeal, int iDealValueToMe, int iValueImOffering, int iValueTheyreOffering) const;
	//! Leader messages go to the bot (HstUserInterface::HostLeaderMessage) in bot mode; the generated sink otherwise.
	virtual void GameplayDiplomacyAILeaderMessage(PlayerTypes ePlayer, DiploUIStateTypes eDiploUIState, const char* szLeaderMessage, LeaderheadAnimationTypes eAction, int iData1);
	virtual void sendNetDemandAccepted(PlayerTypes fromPlayer, PlayerTypes toPlayer, ICvDeal1* pDeal) const;
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

protected:
	PlayerTypes GetActivePlayer() const;
	//! Append a command; ePlayer = sender. Arguments are filled in by the caller.
	HstNetMessage& Queue(HstApiId eId, PlayerTypes ePlayer);
	void Deliver(const HstNetMessage& kMsg);
	//! NetDealAccepted / NetDemandAccepted in peer mode: the deal travels serialized (ICvDeal1::Write, hex in strText) and is
	//! read back into the game's temp deal on every peer before DoAcceptedDeal / DoAcceptedDemand.
	void DeliverDealMessage(const HstNetMessage& kMsg);
	//! Log a send* the exe has no Response* for (once per method).
	void LogNoResponse(HstApiId eId);

	std::string m_strCacheFolder;
	ICvEngineScriptSystem1* m_pkScriptSystem;
	ICvGameContext1* m_pkCtx;
	ICvPreGame1* m_pkPreGame;					// DLL wrapper, kept for the process lifetime
	ICvNetMessageHandler3* m_pkNetHandler;		// DLL singleton (QI returns this); never deleted

	// Loopback state (single mode; every message is delivered to the one local "peer").
	std::vector<HstNetMessage> m_akNetQueue;
	std::vector<HstNetMessage> m_akNetDelivering;
	int m_iNetMessagesDelivered;
	bool m_abTurnCompleteSent[MAX_PLAYERS];
	bool m_abTurnCompleteReceived[MAX_PLAYERS];
	mutable bool m_bAICivsProcessedSent;
	mutable bool m_bAICivsProcessedReceived;
	mutable bool m_bInitialAIProcessedSent;
	std::set<int> m_kNoResponseLogged;

	// Peer mode
	HstPeerConfig m_kPeer;
	std::vector<HstNetMessage> m_akOutbox;
	HstNetMessage m_kDropped;					// target of Queue() for a message the owner rule does not forward
	bool m_abAICivsProcessedFrom[MAX_PLAYERS];	// NetAICivsProcessed received from the peer whose active player is i
	int m_iHostTurn;
	int m_iAdvanceWanted;
	int m_iAdvanceGranted;
	unsigned int m_uiSeq;
	int m_iPeerSent;
	int m_iPeerDropped;
	int m_iPeerReceived;

	// turn_model exe (per-player net state at exe 0x2ddfe40 + p*0x18 and the globals after it)
	int m_aiTurnAllCompleteTurn[MAX_PLAYERS];	// +0x14: game turn of the last NetTurnAllComplete from p
	int m_aiAICivsProcessedTurn[MAX_PLAYERS];	// +0x04: turn carried by the last NetAICivsProcessed from p
	bool m_bSentTurnAllComplete;				// 0x2de0445
	bool m_bTurnUnreadySent;					// 0x2de0444
	int m_iBarrierTurn;							// 0x2de0440: turn whose NetLoopBarrier completed (CanAdvanceTurn needs == game turn)
	int m_iAllAIProcessedTurn;					// 0xc98de4: allAICivsProcessedThisTurn = (== game turn)
	int m_iLastTurnAllCompleteTurn;				// 0xc98ddc
	bool m_bInLoopBarrier;						// 0x2dd2f5e
	int m_iLoopBarrierCount;					// 0x2dd2f48
	int m_iLoopBarrierCompleteCount;			// 0x2dd2f60
	int GetGameTurnNow() const;

	// Turn slices (bTurnSlices; net/HstLoopback.cpp "turn slices")
	std::vector<HstNetMessage> m_akSendQueue;	// FNetSession queued outgoing messages (session+0x64.., count +0x80)
	bool m_bQueueOutgoing;						// session+0x84 (FNetSession vt+0x194 0x3fef90; on from game start with >= 2 players)
	bool m_bTurnSlicingActive;					// 0x2dd2f59 (Net_SetTurnSlicingActive 0x2fbe80)
	int m_iTurnSliceNumber;						// 0x2dd2f50
	int m_iTurnSliceReceived;					// 0x2dd2f54 (counts every NetTurnSlice, whatever its slice number)
	bool m_bTurnSliceSent;						// 0x2dd2f58
	int m_iLastTurnInitialAIProcessed;			// 0xc98de8 (0x4e5490)
	int m_aiInitialAIProcessedTurn[MAX_PLAYERS];	// per-player turn of the last NetInitialAIProcessed (0x79aeb0)
	int m_iSelfInFlight;						// harness: own flushed commands not yet delivered back (see TurnSliceFrame)
	int m_iTurnSliceFlushes;
	int m_iTurnSliceFlushedMsgs;
	static bool IsImmediateMessage(HstApiId eId);
	void DeliverTurnSlice();
};

#endif // HSTENGINEUTILITY_H
