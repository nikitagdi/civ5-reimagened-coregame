//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstLoopback.cpp
//!  \brief     Turn handshake and send* -> ICvNetMessageHandler3::Response* loopback (design 1.8, 2.2).
//!
//!		Single mode: one local peer. Every send* command the DLL issues is queued and delivered to the DLL's
//!		net message handler before the next ICvGame1::Update (the exe processes received net messages
//!		between game-core frames; GUESS: delivery at the next slice, not synchronously inside send*).
//!		Sender = the active player unless the send* method names a player (as the exe's net messages carry
//!		the sending peer's player id). Member functions of HstEngineUtility (declared in HstEngineUtility.h).
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#include "HstCommon.h"
#include "engine/HstEngineUtility.h"
#include "platform/HstPlatform.h"
#include "platform/HstDetHeap.h"
#include "engine/HstUserInterface.h"

extern HstUserInterface* g_pkHstLuaUI;	// HstExeLua.cpp (bot bridge)

// ---- helpers ---------------------------------------------------------------------------------------------------------

PlayerTypes HstEngineUtility::GetActivePlayer() const
{
	return m_pkPreGame ? m_pkPreGame->activePlayer() : NO_PLAYER;
}

HstNetMessage& HstEngineUtility::Queue(HstApiId eId, PlayerTypes ePlayer)
{
	std::vector<HstNetMessage>* pakTarget = &m_akNetQueue;
	if(m_kPeer.bEnabled)
	{
		pakTarget = &m_akOutbox;
		// Owner rule (--ai-sends owner): a message naming a player other than the local active player is forwarded only by
		// that player's peer, or by the host when no peer has it as active player. Default "all": no filter (exe).
		const PlayerTypes eActive = GetActivePlayer();
		if(m_kPeer.bAiSendsOwner && ePlayer != eActive && ePlayer != NO_PLAYER)
		{
			bool bOwned = false;
			for(size_t i = 0; i < m_kPeer.aiPeerPlayers.size(); ++i)
				bOwned = bOwned || m_kPeer.aiPeerPlayers[i] == (int)ePlayer;
			if(bOwned || m_kPeer.iPeerId != m_kPeer.iHostPeer)
			{
				++m_iPeerDropped;
				m_kDropped.eId = eId;
				m_kDropped.ePlayer = ePlayer;
				return m_kDropped;
			}
		}
		++m_iPeerSent;
		// Turn slices: FNetSession_Broadcast 0x3fc4b0 keeps a non-immediate message (every game command) in the session's send
		// queue while session+0x84 is set; it leaves this peer only when NetTurnSlice Execute flushes the queue (DeliverTurnSlice).
		if(m_bQueueOutgoing && !IsImmediateMessage(eId))
			pakTarget = &m_akSendQueue;
	}
	if(pakTarget == &m_akSendQueue)
	{
		HstDetHeapPauseScope kPause;	// the queue's size differs per peer; keep its storage out of the deterministic layout
		pakTarget->push_back(HstNetMessage());
	}
	else
		pakTarget->push_back(HstNetMessage());
	HstNetMessage& kMsg = pakTarget->back();
	kMsg.eId = eId;
	kMsg.ePlayer = ePlayer;
	for(int i = 0; i < (int)HST_ARRAY_SIZE(kMsg.aiArg); ++i)
		kMsg.aiArg[i] = 0;
	kMsg.iPeer = m_kPeer.bEnabled ? m_kPeer.iPeerId : 0;
	kMsg.iTurn = m_iHostTurn;
	kMsg.uiSeq = m_uiSeq++;
	return kMsg;
}

void HstEngineUtility::TakeOutbox(std::vector<HstNetMessage>& akOut)
{
	akOut.clear();
	akOut.swap(m_akOutbox);
}

void HstEngineUtility::DeliverRemote(const HstNetMessage& kMsg)
{
	++m_iPeerReceived;
	++m_iNetMessagesDelivered;
	if(m_iSelfInFlight > 0 && kMsg.iPeer == m_kPeer.iPeerId && !IsImmediateMessage(kMsg.eId))
		--m_iSelfInFlight;		// one of this peer's own flushed commands came back (see TurnSliceFrame)
	Deliver(kMsg);
}

int HstEngineUtility::GetNumAICivsProcessedPeers() const
{
	int iCount = 0;
	for(int i = 0; i < MAX_PLAYERS; ++i)
		iCount += m_abAICivsProcessedFrom[i] ? 1 : 0;
	return iCount;
}

static const char* s_aszHstExeNetNames[] = { "", "NetTurnAllComplete", "NetLoopBarrier", "NetLoopBarrierComplete", "NetIdeologyChoice", "NetArchaeologyChoice",
	"NetTurnSlice" };

HstApiId HstEngineUtility::MethodId(const std::string& strMethod)
{
	for(int i = 0; i < NUM_HST_API; ++i)
	{
		if(strcmp(g_akHstApiInfo[i].szFamily, "EngineUtility") == 0 && strMethod == g_akHstApiInfo[i].szMethod)
			return (HstApiId)i;
	}
	for(int i = kHstNet_TurnAllComplete; i < kHstNet_End; ++i)
		if(strMethod == s_aszHstExeNetNames[i - NUM_HST_API])
			return (HstApiId)i;
	return (HstApiId)NUM_HST_API;
}

const char* HstEngineUtility::MethodName(HstApiId eId)
{
	if((int)eId >= 0 && (int)eId < NUM_HST_API)
		return g_akHstApiInfo[eId].szMethod;
	if((int)eId >= kHstNet_TurnAllComplete && (int)eId < kHstNet_End)
		return s_aszHstExeNetNames[(int)eId - NUM_HST_API];
	return "?";
}

void HstEngineUtility::SendExeMessage(HstExeNetMessage eMsg, PlayerTypes ePlayer, int iArg0, int iArg1)
{
	HstNetMessage& k = Queue((HstApiId)eMsg, ePlayer);
	k.aiArg[0] = iArg0;
	k.aiArg[1] = iArg1;
}

// The live game turn (the exe's helpers call game->GetGameTurn() each time; m_iHostTurn is the turn at the start of the
// slice and is stale after CvGame::doTurn inside the same Update).
int HstEngineUtility::GetGameTurnNow() const
{
	if(!m_pkCtx)
		return m_iHostTurn;
	std::auto_ptr<ICvGame1> pkGame(m_pkCtx->GetGame());
	return pkGame->GetGameTurn();
}

bool HstEngineUtility::IsNetHuman(PlayerTypes ePlayer) const
{
	if(!m_pkPreGame || ePlayer < 0 || ePlayer >= MAX_PLAYERS)
		return false;
	const SlotStatus eStatus = m_pkPreGame->slotStatus(ePlayer);
	if(eStatus == SS_TAKEN)
		return true;
	if(eStatus != SS_OBSERVER)
		return false;
	for(size_t i = 0; i < m_kPeer.aiPeerPlayers.size(); ++i)
		if(m_kPeer.aiPeerPlayers[i] == (int)ePlayer)
			return true;
	return false;
}

// exe 0x2de690 (first test of CanAdvanceTurn and of the frame logic): every human (connected observers too) has its
// NetTurnComplete flag (+0xf) set.
bool HstEngineUtility::AllHumansTurnComplete() const
{
	for(int i = 0; i < MAX_PLAYERS; ++i)
		if(IsNetHuman((PlayerTypes)i) && !m_abTurnCompleteReceived[i])
			return false;
	return true;
}

// turn_model exe: the GameViewState frame (exe 0x3253e0) runs between game-core updates (only when !IsGameCoreExecuting):
//   if (barrierTurn != turn && allAICivsProcessedThisTurn && all humans sent NetTurnComplete) {
//     if (!sentTurnAllComplete) 0x1adeb0: broadcast NetTurnAllComplete(active player), sentTurnAllComplete = 1
//     else if (game->GetNumGameTurnActive() == 0 && HasReceivedTurnAllCompleteFromAllPlayers) enter NetLoopBarrier (0x31b5f0) }
// The NetLoopBarrier completes when every peer's NetLoopBarrierComplete arrived (0x7f0d0): barrierTurn = turn (0x1a5a10),
// allAICivsProcessed reset; CanAdvanceTurn (0x36ef20) then lets CvGame::update run doTurn.
// GUESS: the frame does not re-enter the barrier while it is open (civhost guards with bInLoopBarrier); the sync checks of
// the barrier (City/Unit/Player/RNG, 0x88070) are not simulated: LoopBarrier from N peers -> each sends LoopBarrierComplete.
void HstEngineUtility::ExeFrame()
{
	if(!IsExeTurns() || !m_pkCtx)
		return;
	std::auto_ptr<ICvGame1> pkGame(m_pkCtx->GetGame());
	const int iTurn = pkGame->GetGameTurn();
	m_iHostTurn = iTurn;
	if(m_iBarrierTurn == iTurn || m_iAllAIProcessedTurn != iTurn || !AllHumansTurnComplete())
		return;
	const PlayerTypes eActive = GetActivePlayer();
	if(!m_bSentTurnAllComplete)
	{
		if(eActive != NO_PLAYER)
		{
			Queue((HstApiId)kHstNet_TurnAllComplete, eActive);
			m_iLastTurnAllCompleteTurn = iTurn;		// NetTurnAllComplete ctor 0x10a9f0 writes Sync_g_lastTurnAllCompleteTurn
		}
		m_bSentTurnAllComplete = true;
		return;
	}
	if(m_bInLoopBarrier || pkGame->GetNumGameTurnActive() != 0 || !HasReceivedTurnAllCompleteFromAllPlayers())
		return;
	if(m_iLastTurnAllCompleteTurn != iTurn && eActive != NO_PLAYER)
	{
		Queue((HstApiId)kHstNet_TurnAllComplete, eActive);
		m_iLastTurnAllCompleteTurn = iTurn;
	}
	m_bInLoopBarrier = true;
	Queue((HstApiId)kHstNet_LoopBarrier, eActive);
}

// ---- turn slices (exe command order in an MP game) -------------------------------------------------------------------
// From game start (NetPlayerReady Execute 0x80c80: session vt+0x194(1), Net_SetTurnSlicingActive(1) when > 1 player) a peer
// does not send its game commands when the UI / DLL issues them: FNetSession_Broadcast 0x3fc4b0 keeps every non-immediate
// message in the session's send queue (session+0x84 set). Every frame Net_TurnSlice_CheckAndSend 0x307d0 broadcasts
// NetTurnSlice(slice#) (immediate) once per slice, when no player HasBusyUnitOrCity and no turn-active player
// HasUnitsThatNeedAIUpdate. NetTurnSlice Execute 0x129600 counts every NetTurnSlice; at the number of session players the slice
// closes, and only the peer whose local player has rank (slice# mod number of human players) among the humans flushes its
// queue (vt+0x194(0); FlushQueued 0x3fefc0 with the max set by NetTurnSliceAdjust, -1 = all; vt+0x194(1)), and only if
// every connected human sent NetInitialAIProcessed for this game turn (0xc98de8), it has not sent NetTurnAllComplete this
// turn (0xc98ddc) and its local player IsTurnActive. The flush goes to every player, the sender included: its own copy is
// executed through the transport's loopback deque (FNetTransport_Send 0x3f7970 -> transport+0x5c, drained at the end of
// FNetTransport_Update 0x3f7dd0), i.e. in the same network pump as the flush, before its next NetTurnSlice. Commands of
// different players therefore never overlap: one player's batch per slice, every peer quiescent in between.
// Harness: the flushed commands go to the outbox and come back to the sender at the next frame (like every message), so
// TurnSliceFrame holds this peer's next NetTurnSlice until its own flushed commands were executed (m_iSelfInFlight).

bool HstEngineUtility::IsImmediateMessage(HstApiId eId)
{
	switch((int)eId)
	{
	case kHstApi_EngineUtility_sendTurnComplete:			// NetTurnComplete 0x293c40: +4 = 1
	case kHstApi_EngineUtility_sendTurnUnready:				// NetTurnUnready
	case kHstApi_EngineUtility_SendAICivsProcessed:			// NetAICivsProcessed ctor 0x46b70: +4 = 1
	case kHstApi_EngineUtility_sendPlayerInitialAIProcessed:	// NetInitialAIProcessed ctor 0x92e00: +4 = 1
	case kHstNet_TurnAllComplete:							// 0x10a9f0: +4 = 1
	case kHstNet_LoopBarrier:								// 0x31b5f0: +4 = 1
	case kHstNet_LoopBarrierComplete:
	case kHstNet_TurnSlice:
		return true;
	default:
		return false;
	}
}

void HstEngineUtility::TurnSliceFrame()
{
	if(!m_bTurnSlicingActive || !m_pkCtx || m_kPeer.iNumPeers <= 1)
		return;
	HstDetHeapPauseScope kPause;	// host-side net logic; the player getters below are const
	std::auto_ptr<ICvGame1> pkGame(m_pkCtx->GetGame());
	for(int i = 0; i < MAX_PLAYERS; ++i)
	{
		std::auto_ptr<ICvPlayer1> pkPlayer(pkGame->GetPlayer((PlayerTypes)i));
		if(pkPlayer.get() && (pkPlayer->HasBusyUnitOrCity() || (pkPlayer->IsTurnActive() && pkPlayer->HasUnitsThatNeedAIUpdate())))
			return;		// "Turn slice stalled!" after 10 s
	}
	if(m_iSelfInFlight > 0)
		return;
	if(!m_bTurnSliceSent)
	{
		HstNetMessage& kMsg = Queue((HstApiId)kHstNet_TurnSlice, GetActivePlayer());
		kMsg.aiArg[0] = m_iTurnSliceNumber;
		m_bTurnSliceSent = true;
	}
}

void HstEngineUtility::DeliverTurnSlice()
{
	if(++m_iTurnSliceReceived < m_kPeer.iNumPeers)	// Sync_Net_GetNumActivePlayers(1): every player in the session
		return;
	if(m_pkCtx)
	{
		HstDetHeapPauseScope kPause;
		std::auto_ptr<ICvGame1> pkGame(m_pkCtx->GetGame());
		const PlayerTypes eLocal = GetActivePlayer();		// session vt+0x8c (0x3fd7a0): the local player
		int iHumans = 0;
		int iRank = -1;
		for(int i = 0; i < MAX_PLAYERS; ++i)
		{
			std::auto_ptr<ICvPlayer1> pkPlayer(pkGame->GetPlayer((PlayerTypes)i));
			if(pkPlayer.get() && pkPlayer->IsHuman())
			{
				if(i == (int)eLocal)
					iRank = iHumans;
				++iHumans;
			}
		}
		const bool bMySlice = iHumans < 1 || (int)((unsigned int)m_iTurnSliceNumber % (unsigned int)iHumans) == iRank;
		const int iTurn = pkGame->GetGameTurn();
		bool bFlush = false;
		if(bMySlice && m_iLastTurnInitialAIProcessed == iTurn && m_iLastTurnAllCompleteTurn != iTurn && eLocal != NO_PLAYER)
		{
			std::auto_ptr<ICvPlayer1> pkLocal(pkGame->GetPlayer(eLocal));
			bFlush = pkLocal.get() && pkLocal->IsTurnActive();
		}
		if(bFlush && m_bTurnSlicingActive && !m_akSendQueue.empty())
		{
			// "------> TURN SLICE <---------": FlushQueued sends the batch to every player, this peer included
			for(size_t i = 0; i < m_akSendQueue.size(); ++i)
				m_akOutbox.push_back(m_akSendQueue[i]);
			m_iSelfInFlight += (int)m_akSendQueue.size();
			m_iTurnSliceFlushedMsgs += (int)m_akSendQueue.size();
			++m_iTurnSliceFlushes;
			m_akSendQueue.clear();
		}
	}
	++m_iTurnSliceNumber;
	m_iTurnSliceReceived = 0;
	m_bTurnSliceSent = false;
}

bool HstEngineUtility::InjectByName(const std::string& strMethod, const std::vector<int>& aiArgs, const std::string& strText, PlayerTypes eFrom,
	std::string& strError)
{
	const HstApiId eId = MethodId(strMethod);
	if(eId == (HstApiId)NUM_HST_API)
	{
		strError = "unknown engine method " + strMethod;
		return false;
	}
	if(eId == kHstApi_EngineUtility_sendTurnComplete)
	{
		sendTurnComplete();
		return true;
	}
	if(aiArgs.size() > 9)
	{
		strError = "at most 9 integer arguments";
		return false;
	}
	HstNetMessage& kMsg = Queue(eId, eFrom == NO_PLAYER ? GetActivePlayer() : eFrom);
	for(size_t i = 0; i < aiArgs.size(); ++i)
		kMsg.aiArg[i] = aiArgs[i];
	kMsg.strText = strText;
	return true;
}

void HstEngineUtility::LogNoResponse(HstApiId eId)
{
	if(m_kNoResponseLogged.insert((int)eId).second)
		HstLog("net: %s has no ICvNetMessageHandler3 response; dropped (logged once)", g_akHstApiInfo[eId].szMethod);
}

int HstEngineUtility::DispatchNetMessages()
{
	if(m_akNetQueue.empty())
		return 0;
	// Responses may send again; those go to the next slice (swap out the queue first).
	m_akNetDelivering.clear();
	m_akNetDelivering.swap(m_akNetQueue);
	for(size_t i = 0; i < m_akNetDelivering.size(); ++i)
		Deliver(m_akNetDelivering[i]);
	const int iCount = (int)m_akNetDelivering.size();
	m_iNetMessagesDelivered += iCount;
	m_akNetDelivering.clear();
	return iCount;
}

#define A(i) (kMsg.aiArg[i])
#define B(i) (kMsg.aiArg[i] != 0)

void HstEngineUtility::Deliver(const HstNetMessage& kMsg)
{
	const PlayerTypes eP = kMsg.ePlayer;
	// Handshake messages first (no handler needed).
	switch(kMsg.eId)
	{
	case kHstApi_EngineUtility_sendTurnComplete:
		if(eP >= 0 && eP < MAX_PLAYERS)
			m_abTurnCompleteReceived[eP] = true;
		return;
	case kHstApi_EngineUtility_sendTurnUnready:
		// NetTurnUnready Execute 0x1af980: the player's turn-complete flag (+0xf) is cleared.
		if(eP >= 0 && eP < MAX_PLAYERS)
			m_abTurnCompleteReceived[eP] = false;
		return;
	case kHstNet_TurnAllComplete:
		// Execute 0x1c7d60 -> 0x35cba0: player->SetBusyUnitUpdatesRemaining(600); +0x14 = game turn.
		if(eP >= 0 && eP < MAX_PLAYERS)
		{
			m_aiTurnAllCompleteTurn[eP] = m_iHostTurn;
			if(m_pkCtx && IsNetHuman(eP))	// GUESS for spectator peers: an AI active player's message does not touch the AI player
			{
				std::auto_ptr<ICvGame1> pkGame(m_pkCtx->GetGame());
				std::auto_ptr<ICvPlayer1> pkPlayer(pkGame->GetPlayer(eP));
				if(pkPlayer.get())
					pkPlayer->SetBusyUnitUpdatesRemaining(600);
			}
		}
		return;
	case kHstNet_LoopBarrier:
		// Execute 0x88070: ++count; at N every peer sends its sync checks, then (0x1aae30, no OOS) NetLoopBarrierComplete.
		if(++m_iLoopBarrierCount >= m_kPeer.iNumPeers)
		{
			m_iLoopBarrierCount = 0;
			Queue((HstApiId)kHstNet_LoopBarrierComplete, GetActivePlayer());
		}
		return;
	case kHstNet_LoopBarrierComplete:
		// Execute 0x7f0d0: at N: allAICivsProcessed turn = -1, "NetLoopBarrier EXIT", barrier turn = game turn (0x1a5a10).
		if(++m_iLoopBarrierCompleteCount >= m_kPeer.iNumPeers)
		{
			m_iLoopBarrierCompleteCount = 0;
			m_iAllAIProcessedTurn = -1;
			m_bInLoopBarrier = false;
			m_iBarrierTurn = m_iHostTurn;
		}
		return;
	case kHstApi_EngineUtility_SendAICivsProcessed:
		if(IsExeTurns())
		{
			// Execute 0x975b0: +4 = message turn; 0x19a140: when every connected human (0x2636d0) has +4 == game turn,
			// allAICivsProcessedThisTurn becomes true (0xc98de4 = game turn).
			if(eP >= 0 && eP < MAX_PLAYERS)
			{
				m_aiAICivsProcessedTurn[eP] = kMsg.iTurn;
				m_abAICivsProcessedFrom[eP] = true;
			}
			bool bAll = true;
			for(int i = 0; i < MAX_PLAYERS && bAll; ++i)
				if(IsNetHuman((PlayerTypes)i) && m_aiAICivsProcessedTurn[i] != m_iHostTurn)
					bAll = false;
			if(bAll)
				m_iAllAIProcessedTurn = m_iHostTurn;
			m_bAICivsProcessedReceived = m_iAllAIProcessedTurn == m_iHostTurn;
			return;
		}
		if(!m_kPeer.bEnabled)
		{
			m_bAICivsProcessedReceived = true;
			return;
		}
		// Peer mode: "all AI civs processed" = NetAICivsProcessed (128) received from every peer (exe 0x975b0; GUESS: the
		// count is compared with the number of active network players, like NetTurnSlice / NetLoopBarrier).
		if(eP >= 0 && eP < MAX_PLAYERS)
			m_abAICivsProcessedFrom[eP] = true;
		m_bAICivsProcessedReceived = GetNumAICivsProcessedPeers() >= m_kPeer.iNumPeers;
		return;
	case kHstApi_EngineUtility_sendPlayerInitialAIProcessed:
		// NetInitialAIProcessed Execute 0xb6dc0: the player's stamp = the message turn (0x79aeb0), then 0x4e5490: once every connected
		// human's stamp is the game turn (0x75d540), s_lastTurnInitialAIProcessed (0xc98de8) = game turn. Only turn slices read it.
		if(IsExeTurns() && eP >= 0 && eP < MAX_PLAYERS && m_pkPreGame)
		{
			m_aiInitialAIProcessedTurn[eP] = A(0);
			const int iTurn = GetGameTurnNow();
			bool bAll = true;
			for(int i = 0; i < MAX_PLAYERS && bAll; ++i)
				if(m_pkPreGame->slotStatus((PlayerTypes)i) == SS_TAKEN && m_aiInitialAIProcessedTurn[i] != iTurn)
					bAll = false;
			if(bAll)
				m_iLastTurnInitialAIProcessed = iTurn;
		}
		return;
	case kHstNet_TurnSlice:
		DeliverTurnSlice();
		return;
	case kHstApi_EngineUtility_sendNetDealAccepted:
	case kHstApi_EngineUtility_sendNetDemandAccepted:
		DeliverDealMessage(kMsg);
		return;
	default:
		break;
	}
	ICvNetMessageHandler3* h = m_pkNetHandler;
	if(!h)
		return;
	if(kMsg.eId == (HstApiId)kHstNet_IdeologyChoice)
	{
		h->ResponseIdeologyChoice(eP, (PolicyBranchTypes)A(0));
		return;
	}
	if(kMsg.eId == (HstApiId)kHstNet_ArchaeologyChoice)
	{
		h->ResponseArchaeologyChoice(eP, (ArchaeologyChoiceType)A(1));	// Lua (player, unit index, choice)
		return;
	}
	if((int)kMsg.eId >= NUM_HST_API)
		return;
	switch(kMsg.eId)
	{
	case kHstApi_EngineUtility_SendFoundPantheon:
		h->ResponseFoundPantheon(eP, (BeliefTypes)A(0));
		break;
	case kHstApi_EngineUtility_SendFoundReligion:
		h->ResponseFoundReligion(eP, (ReligionTypes)A(0), kMsg.strText.c_str(), (BeliefTypes)A(1), (BeliefTypes)A(2), (BeliefTypes)A(3), (BeliefTypes)A(4), A(5), A(6));
		break;
	case kHstApi_EngineUtility_SendEnhanceReligion:
		h->ResponseEnhanceReligion(eP, (ReligionTypes)A(0), kMsg.strText.c_str(), (BeliefTypes)A(1), (BeliefTypes)A(2), A(3), A(4));
		break;
	case kHstApi_EngineUtility_SendMoveSpy:
		h->ResponseMoveSpy(eP, A(0), A(1), A(2), B(3));
		break;
	case kHstApi_EngineUtility_SendStageCoup:
		h->ResponseStageCoup(eP, A(0));
		break;
	case kHstApi_EngineUtility_SendFaithPurchase:
		h->ResponseFaithPurchase(eP, (FaithPurchaseTypes)A(0), A(1));
		break;
	case kHstApi_EngineUtility_sendPushMission:
		h->ResponsePushMission(eP, A(0), (MissionTypes)A(1), A(2), A(3), A(4), B(5));
		break;
	case kHstApi_EngineUtility_sendAutoMission:
		h->ResponseAutoMission(eP, A(0));
		break;
	case kHstApi_EngineUtility_sendDoCommand:
		h->ResponseDoCommand(eP, A(0), (CommandTypes)A(1), A(2), A(3), B(4));
		break;
	case kHstApi_EngineUtility_sendPushOrder:
		h->ResponseCityPushOrder(eP, A(0), (OrderTypes)A(1), A(2), B(3), B(4), B(5));
		break;
	case kHstApi_EngineUtility_sendPopOrder:
		h->ResponseCityPopOrder(eP, A(0), A(1));
		break;
	case kHstApi_EngineUtility_sendSwapOrder:
		h->ResponseCitySwapOrder(eP, A(0), A(1));
		break;
	case kHstApi_EngineUtility_sendPurchase_1:
		static_cast<ICvNetMessageHandler1*>(h)->ResponseCityPurchase(eP, A(0), (UnitTypes)A(1), (BuildingTypes)A(2), (ProjectTypes)A(3));	// v2 overload hides v1
		break;
	case kHstApi_EngineUtility_sendPurchase_2:
		h->ResponseCityPurchase(eP, A(0), (UnitTypes)A(1), (BuildingTypes)A(2), (ProjectTypes)A(3), A(4));
		break;
	case kHstApi_EngineUtility_sendDoTask:
		h->ResponseCityDoTask(eP, A(0), (TaskTypes)A(1), A(2), A(3), B(4), B(5), B(6), B(7));
		break;
	case kHstApi_EngineUtility_sendResearch:
		h->ResponseResearch(eP, (TechTypes)A(0), A(1), (PlayerTypes)A(2), B(3));
		break;
	case kHstApi_EngineUtility_sendChangeWar:
		h->ResponseChangeWar(eP, (TeamTypes)A(0), B(1));
		break;
	case kHstApi_EngineUtility_sendIgnoreWarning:
		h->ResponseIgnoreWarning(eP, (TeamTypes)A(0));
		break;
	case kHstApi_EngineUtility_SendPledgeMinorProtection:
		h->ResponsePledgeMinorProtection(eP, (PlayerTypes)A(0), B(1), false);	// GUESS: v1 message has no "pledge broken" flag
		break;
	case kHstApi_EngineUtility_SendMinorNoUnitSpawning:
		h->ResponseMinorNoUnitSpawning(eP, (PlayerTypes)A(0), B(1));
		break;
	case kHstApi_EngineUtility_SendLiberateMinor:
		h->ResponseLiberatePlayer(eP, (PlayerTypes)A(0), A(1));
		break;
	case kHstApi_EngineUtility_sendUpdatePolicies:
		h->ResponseUpdatePolicies(eP, B(0), A(1), B(2));
		break;
	case kHstApi_EngineUtility_SendDiploVote:
		h->ResponseDiploVote(eP, (PlayerTypes)A(0));
		break;
	case kHstApi_EngineUtility_sendLaunch:
		h->ResponseLaunchSpaceship(eP, (VictoryTypes)A(0));
		break;
	case kHstApi_EngineUtility_sendAdvancedStartAction:
		h->ResponseAdvancedStartAction(eP, (AdvancedStartActionTypes)A(0), A(1), A(2), A(3), B(4));
		break;
	case kHstApi_EngineUtility_sendBarbarianRansom:
		h->ResponseBarbarianRansom(eP, A(0), A(1));
		break;
	case kHstApi_EngineUtility_sendGiftUnit:
		h->ResponseGiftUnit(eP, (PlayerTypes)A(0), A(1));
		break;
	case kHstApi_EngineUtility_sendReturnCivilian:
		h->ResponseReturnCivilian(eP, (PlayerTypes)A(1), A(2), B(0));
		break;
	case kHstApi_EngineUtility_SendUpdateCityCitizens:
		h->ResponseUpdateCityCitizens(eP, A(0));
		break;
	case kHstApi_EngineUtility_SendSellBuilding:
		h->ResponseSellBuilding(eP, A(0), (BuildingTypes)A(1));
		break;
	case kHstApi_EngineUtility_SendRenameCity:
		h->ResponseRenameCity(eP, A(0), kMsg.strText.c_str());
		break;
	case kHstApi_EngineUtility_SendRenameUnit:
		h->ResponseRenameUnit(eP, A(0), kMsg.strText.c_str());
		break;
	case kHstApi_EngineUtility_sendSwapUnits:
		h->ResponseSwapUnits(eP, A(0), (MissionTypes)A(1), A(2), A(3), A(4), B(5));
		break;
	case kHstApi_EngineUtility_sendCityBuyPlot:
		h->ResponseCityBuyPlot(eP, A(0), A(1), A(2));
		break;
	case kHstApi_EngineUtility_sendMinorPledgeProtection:
		h->ResponsePledgeMinorProtection(eP, (PlayerTypes)A(0), B(1), B(2));
		break;
	case kHstApi_EngineUtility_sendMinorGiftGold:
		h->ResponseMinorCivGiftGold(eP, (PlayerTypes)A(0), A(1));
		break;
	case kHstApi_EngineUtility_sendMinorGiftTileImprovement:
		h->ResponseMinorCivGiftTileImprovement(eP, (PlayerTypes)A(0), A(1), A(2));
		break;
	case kHstApi_EngineUtility_sendMinorBullyGold:
		h->ResponseMinorCivBullyGold(eP, (PlayerTypes)A(0), A(1));
		break;
	case kHstApi_EngineUtility_sendMinorBullyUnit:
		h->ResponseMinorCivBullyUnit(eP, (PlayerTypes)A(0), (UnitTypes)A(1));
		break;
	case kHstApi_EngineUtility_sendMinorBuyout:
		h->ResponseMinorCivBuyout(eP, (PlayerTypes)A(0));
		break;
	case kHstApi_EngineUtility_sendGreatPersonChoice:
		h->ResponseGreatPersonChoice(eP, (UnitTypes)A(0));
		break;
	case kHstApi_EngineUtility_sendMayaBonusChoice:
		h->ResponseMayaBonusChoice(eP, (UnitTypes)A(0));
		break;
	case kHstApi_EngineUtility_sendFaithGreatPersonChoice:
		h->ResponseFaithGreatPersonChoice(eP, (UnitTypes)A(0));
		break;
	case kHstApi_EngineUtility_sendSetCityAIFocus:
		h->ResponseSetCityAIFocus(eP, A(0), (CityAIFocusTypes)A(1));
		break;
	case kHstApi_EngineUtility_sendSetCityAvoidGrowth:
		h->ResponseSetCityAvoidGrowth(eP, A(0), B(1));
		break;
	case kHstApi_EngineUtility_sendFromUIDiploEvent:
		h->ResponseDiplomacyFromUI(eP, (PlayerTypes)A(0), (FromUIDiploEventTypes)A(1), A(2), A(3));
		break;
	case kHstApi_EngineUtility_sendGoodyChoice:
		h->ResponseGoodyChoice(eP, A(0), A(1), (GoodyTypes)A(2), A(3));
		break;
	case kHstApi_EngineUtility_sendSetSwappableGreatWork:
		h->ResponseSetSwappableGreatWork(eP, A(0), A(1));
		break;
	case kHstApi_EngineUtility_sendSwapGreatWorks:
		h->ResponseSwapGreatWorks(eP, A(0), (PlayerTypes)A(1), A(2));
		break;
	case kHstApi_EngineUtility_sendMoveGreatWorks:
		h->ResponseMoveGreatWorks(eP, A(0), A(1), A(2), A(3), A(4), A(5));
		break;
	case kHstApi_EngineUtility_SendLeagueVoteEnact:
		h->ResponseLeagueVoteEnact((LeagueTypes)A(0), A(1), eP, A(2), A(3));
		break;
	case kHstApi_EngineUtility_SendLeagueVoteRepeal:
		h->ResponseLeagueVoteRepeal((LeagueTypes)A(0), A(1), eP, A(2), A(3));
		break;
	case kHstApi_EngineUtility_SendLeagueVoteAbstain:
		h->ResponseLeagueVoteAbstain((LeagueTypes)A(0), eP, A(1));
		break;
	case kHstApi_EngineUtility_SendLeagueProposeEnact:
		h->ResponseLeagueProposeEnact((LeagueTypes)A(0), (ResolutionTypes)A(1), eP, A(2));
		break;
	case kHstApi_EngineUtility_SendLeagueProposeRepeal:
		h->ResponseLeagueProposeRepeal((LeagueTypes)A(0), A(1), eP);
		break;
	case kHstApi_EngineUtility_SendLeagueEditName:
		h->ResponseLeagueEditName((LeagueTypes)A(0), eP, kMsg.strText.c_str());
		break;
	case kHstApi_EngineUtility_sendChangeIdeology:
		h->ResponseChangeIdeology(eP);
		break;
	default:
		HstLog("net: no delivery for queued %s", g_akHstApiInfo[kMsg.eId].szMethod);
		break;
	}
}

#undef A
#undef B

// ---- turn handshake --------------------------------------------------------------------------------------------------

// Start of CvGame::doTurn (CvGame.cpp gDLL->DoTurn()): a new turn clears the per-turn net state.
void HstEngineUtility::DoTurn()
{
	HST_API(kHstApi_EngineUtility_DoTurn);
	for(int i = 0; i < MAX_PLAYERS; ++i)
	{
		m_abTurnCompleteSent[i] = false;
		m_abTurnCompleteReceived[i] = false;
		m_abAICivsProcessedFrom[i] = false;
	}
	m_bAICivsProcessedSent = false;
	m_bAICivsProcessedReceived = false;
	m_bInitialAIProcessedSent = false;
	// turn_model exe: the new-turn event handler 0x1fd5b0 -> 0x35dc20 clears the local sent flags (0x2de0444..46) and every
	// player's +0xf/+0x10; the turn-stamped fields (+4, +0x14, barrier turn) are not reset (they compare with the game turn).
	m_bSentTurnAllComplete = false;
	m_bTurnUnreadySent = false;
}

// Single mode: no net barrier to wait for. Peer mode with --turn-barrier: true only once the coordinator granted this
// turn (every peer asked for it), so all peers run CvGame::doTurn in the same frame. GUESS: the exe's implementation was
// not reverse engineered; without the barrier the frame lockstep alone keeps peers aligned (default).
bool HstEngineUtility::CanAdvanceTurn()
{
	HST_API(kHstApi_EngineUtility_CanAdvanceTurn);
	if(IsExeTurns())
	{
		// exe 0x36ef20: all humans sent NetTurnComplete (0x2de690) && HasReceivedTurnAllCompleteFromAllPlayers (0x2a0880)
		// && (not network MP || barrier turn 0x2de0440 == game turn).
		return AllHumansTurnComplete() && HasReceivedTurnAllCompleteFromAllPlayers() && m_iBarrierTurn == GetGameTurnNow();
	}
	if(!m_kPeer.bEnabled || !m_kPeer.bTurnBarrier)
		return true;
	if(m_iAdvanceGranted == m_iHostTurn)
		return true;
	m_iAdvanceWanted = m_iHostTurn;
	return false;
}

void HstEngineUtility::sendTurnComplete()
{
	HST_API(kHstApi_EngineUtility_sendTurnComplete);
	const PlayerTypes eActive = GetActivePlayer();
	if(IsExeTurns())
	{
		// exe 0x21d540(active): once per turn (until a NetTurnUnready clears the sent flag 0x2de0446).
		if(eActive == NO_PLAYER || m_abTurnCompleteSent[eActive])
			return;
		m_abTurnCompleteSent[eActive] = true;
		Queue(kHstApi_EngineUtility_sendTurnComplete, eActive);
		return;
	}
	if(eActive >= 0 && eActive < MAX_PLAYERS)
		m_abTurnCompleteSent[eActive] = true;
	Queue(kHstApi_EngineUtility_sendTurnComplete, eActive);
}

bool HstEngineUtility::HasSentTurnComplete()
{
	HST_API(kHstApi_EngineUtility_HasSentTurnComplete);
	const PlayerTypes eActive = GetActivePlayer();
	return eActive >= 0 && eActive < MAX_PLAYERS && m_abTurnCompleteSent[eActive];
}

bool HstEngineUtility::HasReceivedTurnComplete(PlayerTypes ePlayer)
{
	HST_API(kHstApi_EngineUtility_HasReceivedTurnComplete);
	return ePlayer >= 0 && ePlayer < MAX_PLAYERS && m_abTurnCompleteReceived[ePlayer];
}

// "All complete" = every peer finished its turn; with one local peer it is the same as turn complete.
bool HstEngineUtility::HasSentTurnAllComplete()
{
	HST_API(kHstApi_EngineUtility_HasSentTurnAllComplete);
	if(IsExeTurns())
		return m_bSentTurnAllComplete;		// exe 0x3491d0 (0x2de0445)
	const PlayerTypes eActive = GetActivePlayer();
	return eActive >= 0 && eActive < MAX_PLAYERS && m_abTurnCompleteSent[eActive];
}

bool HstEngineUtility::HasReceivedTurnAllComplete(PlayerTypes ePlayer)
{
	HST_API(kHstApi_EngineUtility_HasReceivedTurnAllComplete);
	if(IsExeTurns())
		return ePlayer >= 0 && ePlayer < MAX_PLAYERS && m_aiTurnAllCompleteTurn[ePlayer] == GetGameTurnNow();	// exe 0x2e74f0
	return ePlayer >= 0 && ePlayer < MAX_PLAYERS && m_abTurnCompleteReceived[ePlayer];
}

// True when every connected (human, SS_TAKEN) slot has sent turn complete; all-AI games have none -> true.
bool HstEngineUtility::HasReceivedTurnAllCompleteFromAllPlayers()
{
	HST_API(kHstApi_EngineUtility_HasReceivedTurnAllCompleteFromAllPlayers);
	if(!m_pkPreGame)
		return true;
	if(IsExeTurns())
	{
		// exe 0x2a0880: every connected human's NetTurnAllComplete is for this game turn.
		const int iTurn = GetGameTurnNow();
		for(int i = 0; i < MAX_PLAYERS; ++i)
			if(IsNetHuman((PlayerTypes)i) && m_aiTurnAllCompleteTurn[i] != iTurn)
				return false;
		return true;
	}
	for(int i = 0; i < MAX_PLAYERS; ++i)
	{
		if(m_pkPreGame->slotStatus((PlayerTypes)i) == SS_TAKEN && !m_abTurnCompleteReceived[i])
			return false;
	}
	return true;
}

// The un-ready is always possible in single mode (CvPlayer.cpp: "sometimes you can't in MP games").
bool HstEngineUtility::sendTurnUnready()
{
	HST_API(kHstApi_EngineUtility_sendTurnUnready);
	const PlayerTypes eActive = GetActivePlayer();
	if(IsExeTurns())
	{
		// exe 0x4c9270 (simplified: the unit tests at its start are not modelled): allowed until the local NetTurnAllComplete
		// went out; broadcasts NetTurnUnready once per turn and clears the local sent flag.
		if(m_bSentTurnAllComplete || eActive == NO_PLAYER)
			return false;
		if(!m_bTurnUnreadySent)
		{
			Queue(kHstApi_EngineUtility_sendTurnUnready, eActive);
			m_bTurnUnreadySent = true;
			m_abTurnCompleteSent[eActive] = false;
		}
		return true;
	}
	if(eActive >= 0 && eActive < MAX_PLAYERS)
	{
		m_abTurnCompleteSent[eActive] = false;
		m_abTurnCompleteReceived[eActive] = false;
	}
	return true;
}

// Human-only path (CvGame.cpp); the exe's AutoMoves message has no Response* in ICvNetMessageHandler3.
void HstEngineUtility::sendAutoMoves()
{
	HST_API(kHstApi_EngineUtility_sendAutoMoves);
	LogNoResponse(kHstApi_EngineUtility_sendAutoMoves);
}

void HstEngineUtility::sendPlayerInitialAIProcessed() const
{
	HST_API(kHstApi_EngineUtility_sendPlayerInitialAIProcessed);
	m_bInitialAIProcessedSent = true;
	if(m_kPeer.bEnabled)	// NetInitialAIProcessed (132) goes on the wire; it changes no DLL state on delivery
	{
		// exe 0x264490: NetInitialAIProcessed(game->GetActivePlayer(), game->GetGameTurn()) (immediate); the turn rides in arg 0
		HstNetMessage& kMsg = const_cast<HstEngineUtility*>(this)->Queue(kHstApi_EngineUtility_sendPlayerInitialAIProcessed, GetActivePlayer());
		kMsg.aiArg[0] = GetGameTurnNow();
	}
}

bool HstEngineUtility::allAIProcessedThisTurn() const
{
	HST_API(kHstApi_EngineUtility_allAIProcessedThisTurn);
	if(IsExeTurns())
		return m_iAllAIProcessedTurn == GetGameTurnNow();	// exe 0x3855f0 -> vfunc +0x3f4
	return m_bAICivsProcessedReceived;
}

// CvGame::SetLastTurnAICivsProcessed sends once per turn; "all" = received from every peer (one here).
bool HstEngineUtility::allAICivsProcessedThisTurn() const
{
	HST_API(kHstApi_EngineUtility_allAICivsProcessedThisTurn);
	if(IsExeTurns())
		return m_iAllAIProcessedTurn == GetGameTurnNow();	// exe 0x54790 (0xc98de4 == game turn)
	return m_bAICivsProcessedReceived;
}

void HstEngineUtility::SendAICivsProcessed() const
{
	HST_API(kHstApi_EngineUtility_SendAICivsProcessed);
	HstEngineUtility* pkThis = const_cast<HstEngineUtility*>(this);
	m_bAICivsProcessedSent = true;
	HstNetMessage& kMsg = pkThis->Queue(kHstApi_EngineUtility_SendAICivsProcessed, GetActivePlayer());
	if(IsExeTurns())
		kMsg.iTurn = GetGameTurnNow();	// exe 0x218400: NetAICivsProcessed(active player, game->GetGameTurn())
}

bool HstEngineUtility::HasReceivedAICivsProcessed(PlayerTypes ePlayer) const
{
	HST_API(kHstApi_EngineUtility_HasReceivedAICivsProcessed);
	if(IsExeTurns())
		return ePlayer >= 0 && ePlayer < MAX_PLAYERS && m_aiAICivsProcessedTurn[ePlayer] == GetGameTurnNow();	// exe 0x260d40
	if(m_kPeer.bEnabled)
		return ePlayer >= 0 && ePlayer < MAX_PLAYERS && m_abAICivsProcessedFrom[ePlayer];
	return ePlayer == GetActivePlayer() && m_bAICivsProcessedReceived;
}

int HstEngineUtility::GetNumAICivsProcessed() const
{
	HST_API(kHstApi_EngineUtility_GetNumAICivsProcessed);
	if(IsExeTurns())
	{
		int iCount = 0;	// exe 0x175e90: connected humans whose NetAICivsProcessed is for this turn
		const int iTurn = GetGameTurnNow();
		for(int i = 0; i < MAX_PLAYERS; ++i)
			iCount += (IsNetHuman((PlayerTypes)i) && m_aiAICivsProcessedTurn[i] == iTurn) ? 1 : 0;
		return iCount;
	}
	if(m_kPeer.bEnabled)
		return GetNumAICivsProcessedPeers();
	return m_bAICivsProcessedReceived ? 1 : 0;
}

// ---- commands --------------------------------------------------------------------------------------------------------

void HstEngineUtility::SendFoundPantheon(PlayerTypes ePlayer, BeliefTypes eBelief)
{
	HST_API(kHstApi_EngineUtility_SendFoundPantheon);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_SendFoundPantheon, ePlayer);
	k.aiArg[0] = eBelief;
}

void HstEngineUtility::SendFoundReligion(PlayerTypes ePlayer, ReligionTypes eReligion, const char* szCustomName, BeliefTypes eBelief1, BeliefTypes eBelief2, BeliefTypes eBelief3, BeliefTypes eBelief4, int iCityX, int iCityY)
{
	HST_API(kHstApi_EngineUtility_SendFoundReligion);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_SendFoundReligion, ePlayer);
	k.aiArg[0] = eReligion; k.aiArg[1] = eBelief1; k.aiArg[2] = eBelief2; k.aiArg[3] = eBelief3; k.aiArg[4] = eBelief4;
	k.aiArg[5] = iCityX; k.aiArg[6] = iCityY;
	k.strText = szCustomName ? szCustomName : "";
}

void HstEngineUtility::SendEnhanceReligion(PlayerTypes ePlayer, ReligionTypes eReligion, const char* szCustomName, BeliefTypes eBelief1, BeliefTypes eBelief2, int iCityX, int iCityY)
{
	HST_API(kHstApi_EngineUtility_SendEnhanceReligion);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_SendEnhanceReligion, ePlayer);
	k.aiArg[0] = eReligion; k.aiArg[1] = eBelief1; k.aiArg[2] = eBelief2; k.aiArg[3] = iCityX; k.aiArg[4] = iCityY;
	k.strText = szCustomName ? szCustomName : "";
}

void HstEngineUtility::SendMoveSpy(PlayerTypes ePlayer, int iSpyIndex, int iTargetPlayer, int iTargetCity, bool bAsDiplomat)
{
	HST_API(kHstApi_EngineUtility_SendMoveSpy);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_SendMoveSpy, ePlayer);
	k.aiArg[0] = iSpyIndex; k.aiArg[1] = iTargetPlayer; k.aiArg[2] = iTargetCity; k.aiArg[3] = bAsDiplomat;
}

void HstEngineUtility::SendStageCoup(PlayerTypes eSpyPlayer, int iSpyIndex)
{
	HST_API(kHstApi_EngineUtility_SendStageCoup);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_SendStageCoup, eSpyPlayer);
	k.aiArg[0] = iSpyIndex;
}

void HstEngineUtility::SendFaithPurchase(PlayerTypes ePlayer, FaithPurchaseTypes eFaithPurchaseType, int iFaithPurchaseIndex)
{
	HST_API(kHstApi_EngineUtility_SendFaithPurchase);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_SendFaithPurchase, ePlayer);
	k.aiArg[0] = eFaithPurchaseType; k.aiArg[1] = iFaithPurchaseIndex;
}

void HstEngineUtility::sendPushMission(int iUnitID, MissionTypes eMission, int iData1, int iData2, int iFlags, bool bShift)
{
	HST_API(kHstApi_EngineUtility_sendPushMission);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_sendPushMission, GetActivePlayer());
	k.aiArg[0] = iUnitID; k.aiArg[1] = eMission; k.aiArg[2] = iData1; k.aiArg[3] = iData2; k.aiArg[4] = iFlags; k.aiArg[5] = bShift;
}

void HstEngineUtility::sendAutoMission(int iUnitID)
{
	HST_API(kHstApi_EngineUtility_sendAutoMission);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_sendAutoMission, GetActivePlayer());
	k.aiArg[0] = iUnitID;
}

void HstEngineUtility::sendDoCommand(int iUnitID, CommandTypes eCommand, int iData1, int iData2, bool bAlt)
{
	HST_API(kHstApi_EngineUtility_sendDoCommand);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_sendDoCommand, GetActivePlayer());
	k.aiArg[0] = iUnitID; k.aiArg[1] = eCommand; k.aiArg[2] = iData1; k.aiArg[3] = iData2; k.aiArg[4] = bAlt;
}

void HstEngineUtility::sendPushOrder(int iCityID, OrderTypes eOrder, int iData, bool bAlt, bool bShift, bool bCtrl)
{
	HST_API(kHstApi_EngineUtility_sendPushOrder);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_sendPushOrder, GetActivePlayer());
	k.aiArg[0] = iCityID; k.aiArg[1] = eOrder; k.aiArg[2] = iData; k.aiArg[3] = bAlt; k.aiArg[4] = bShift; k.aiArg[5] = bCtrl;
}

void HstEngineUtility::sendPopOrder(int iCity, int iNum)
{
	HST_API(kHstApi_EngineUtility_sendPopOrder);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_sendPopOrder, GetActivePlayer());
	k.aiArg[0] = iCity; k.aiArg[1] = iNum;
}

void HstEngineUtility::sendSwapOrder(int iCity, int iNum0)
{
	HST_API(kHstApi_EngineUtility_sendSwapOrder);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_sendSwapOrder, GetActivePlayer());
	k.aiArg[0] = iCity; k.aiArg[1] = iNum0;
}

void HstEngineUtility::sendPurchase(int iCity, UnitTypes eUnitType, BuildingTypes eBuildingType, ProjectTypes eProjectType)
{
	HST_API(kHstApi_EngineUtility_sendPurchase_1);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_sendPurchase_1, GetActivePlayer());
	k.aiArg[0] = iCity; k.aiArg[1] = eUnitType; k.aiArg[2] = eBuildingType; k.aiArg[3] = eProjectType;
}

void HstEngineUtility::sendPurchase(int iCity, UnitTypes eUnitType, BuildingTypes eBuildingType, ProjectTypes eProjectType, int ePurchaseYield)
{
	HST_API(kHstApi_EngineUtility_sendPurchase_2);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_sendPurchase_2, GetActivePlayer());
	k.aiArg[0] = iCity; k.aiArg[1] = eUnitType; k.aiArg[2] = eBuildingType; k.aiArg[3] = eProjectType; k.aiArg[4] = ePurchaseYield;
}

void HstEngineUtility::sendDoTask(int iCity, TaskTypes eTask, int iData1, int iData2, bool bOption, bool bAlt, bool bShift, bool bCtrl)
{
	HST_API(kHstApi_EngineUtility_sendDoTask);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_sendDoTask, GetActivePlayer());
	k.aiArg[0] = iCity; k.aiArg[1] = eTask; k.aiArg[2] = iData1; k.aiArg[3] = iData2;
	k.aiArg[4] = bOption; k.aiArg[5] = bAlt; k.aiArg[6] = bShift; k.aiArg[7] = bCtrl;
}

void HstEngineUtility::sendResearch(TechTypes eTech, int iDiscover, PlayerTypes ePlayerToStealFrom, bool bShift)
{
	HST_API(kHstApi_EngineUtility_sendResearch);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_sendResearch, GetActivePlayer());
	k.aiArg[0] = eTech; k.aiArg[1] = iDiscover; k.aiArg[2] = ePlayerToStealFrom; k.aiArg[3] = bShift;
}

void HstEngineUtility::sendChangeWar(TeamTypes iRivalTeam, bool bWar)
{
	HST_API(kHstApi_EngineUtility_sendChangeWar);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_sendChangeWar, GetActivePlayer());
	k.aiArg[0] = iRivalTeam; k.aiArg[1] = bWar;
}

void HstEngineUtility::sendIgnoreWarning(TeamTypes iRivalTeam)
{
	HST_API(kHstApi_EngineUtility_sendIgnoreWarning);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_sendIgnoreWarning, GetActivePlayer());
	k.aiArg[0] = iRivalTeam;
}

void HstEngineUtility::SendPledgeMinorProtection(PlayerTypes eMinor, bool bValue)
{
	HST_API(kHstApi_EngineUtility_SendPledgeMinorProtection);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_SendPledgeMinorProtection, GetActivePlayer());
	k.aiArg[0] = eMinor; k.aiArg[1] = bValue;
}

void HstEngineUtility::SendMinorNoUnitSpawning(PlayerTypes eMinor, bool bValue)
{
	HST_API(kHstApi_EngineUtility_SendMinorNoUnitSpawning);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_SendMinorNoUnitSpawning, GetActivePlayer());
	k.aiArg[0] = eMinor; k.aiArg[1] = bValue;
}

void HstEngineUtility::SendLiberateMinor(PlayerTypes eMinor, int iCityID)
{
	HST_API(kHstApi_EngineUtility_SendLiberateMinor);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_SendLiberateMinor, GetActivePlayer());
	k.aiArg[0] = eMinor; k.aiArg[1] = iCityID;
}

void HstEngineUtility::sendUpdatePolicies(bool bNOTPolicyBranch, int iID, bool bValue)
{
	HST_API(kHstApi_EngineUtility_sendUpdatePolicies);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_sendUpdatePolicies, GetActivePlayer());
	k.aiArg[0] = bNOTPolicyBranch; k.aiArg[1] = iID; k.aiArg[2] = bValue;
}

void HstEngineUtility::SendDiploVote(PlayerTypes eVotePlayer)
{
	HST_API(kHstApi_EngineUtility_SendDiploVote);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_SendDiploVote, GetActivePlayer());
	k.aiArg[0] = eVotePlayer;
}

// CvCity: spaceship completion (AI path too).
void HstEngineUtility::sendLaunch(PlayerTypes ePlayer, VictoryTypes eVictory)
{
	HST_API(kHstApi_EngineUtility_sendLaunch);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_sendLaunch, ePlayer);
	k.aiArg[0] = eVictory;
}

void HstEngineUtility::sendAdvancedStartAction(AdvancedStartActionTypes eAction, PlayerTypes ePlayer, int iX, int iY, int iData, bool bAdd)
{
	HST_API(kHstApi_EngineUtility_sendAdvancedStartAction);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_sendAdvancedStartAction, ePlayer);
	k.aiArg[0] = eAction; k.aiArg[1] = iX; k.aiArg[2] = iY; k.aiArg[3] = iData; k.aiArg[4] = bAdd;
}

// The four minor-civ quest/intrusion messages have no Response* in ICvNetMessageHandler1-3.
void HstEngineUtility::sendMinorCivQuestNoInterest(PlayerTypes eMinor, bool bPeace)
{
	HST_API(kHstApi_EngineUtility_sendMinorCivQuestNoInterest);
	LogNoResponse(kHstApi_EngineUtility_sendMinorCivQuestNoInterest);
}

void HstEngineUtility::sendMinorCivQuestCompleted(PlayerTypes eMinor, bool bPeace)
{
	HST_API(kHstApi_EngineUtility_sendMinorCivQuestCompleted);
	LogNoResponse(kHstApi_EngineUtility_sendMinorCivQuestCompleted);
}

void HstEngineUtility::sendMinorCivIntrusion(PlayerTypes eMinor, int iOptionChosen)
{
	HST_API(kHstApi_EngineUtility_sendMinorCivIntrusion);
	LogNoResponse(kHstApi_EngineUtility_sendMinorCivIntrusion);
}

void HstEngineUtility::sendMinorCivEnterTerritory(PlayerTypes eMinor)
{
	HST_API(kHstApi_EngineUtility_sendMinorCivEnterTerritory);
	LogNoResponse(kHstApi_EngineUtility_sendMinorCivEnterTerritory);
}

void HstEngineUtility::sendBarbarianRansom(int iOptionChosen, int iUnitID)
{
	HST_API(kHstApi_EngineUtility_sendBarbarianRansom);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_sendBarbarianRansom, GetActivePlayer());
	k.aiArg[0] = iOptionChosen; k.aiArg[1] = iUnitID;
}

// Also used by Lekmod as an in-band "turn timer pause" signal (CvPlayer.cpp: sendGiftUnit(NO_PLAYER, -11)).
void HstEngineUtility::sendGiftUnit(PlayerTypes eMinor, int iUnitID)
{
	HST_API(kHstApi_EngineUtility_sendGiftUnit);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_sendGiftUnit, GetActivePlayer());
	k.aiArg[0] = eMinor; k.aiArg[1] = iUnitID;
}

void HstEngineUtility::sendReturnCivilian(bool bReturn, PlayerTypes ePlayer, int iUnitID)
{
	HST_API(kHstApi_EngineUtility_sendReturnCivilian);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_sendReturnCivilian, GetActivePlayer());
	k.aiArg[0] = bReturn; k.aiArg[1] = ePlayer; k.aiArg[2] = iUnitID;
}

void HstEngineUtility::SendUpdateCityCitizens(int iCityID)
{
	HST_API(kHstApi_EngineUtility_SendUpdateCityCitizens);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_SendUpdateCityCitizens, GetActivePlayer());
	k.aiArg[0] = iCityID;
}

void HstEngineUtility::SendSellBuilding(int iCityID, BuildingTypes eBuilding)
{
	HST_API(kHstApi_EngineUtility_SendSellBuilding);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_SendSellBuilding, GetActivePlayer());
	k.aiArg[0] = iCityID; k.aiArg[1] = eBuilding;
}

void HstEngineUtility::SendRenameCity(int iCityID, CvString szNewName)
{
	HST_API(kHstApi_EngineUtility_SendRenameCity);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_SendRenameCity, GetActivePlayer());
	k.aiArg[0] = iCityID;
	k.strText = szNewName.c_str();
}

void HstEngineUtility::SendRenameUnit(int iUnitID, CvString szNewName)
{
	HST_API(kHstApi_EngineUtility_SendRenameUnit);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_SendRenameUnit, GetActivePlayer());
	k.aiArg[0] = iUnitID;
	k.strText = szNewName.c_str();
}

void HstEngineUtility::sendPlayerHurry(HurryTypes eHurry)
{
	HST_API(kHstApi_EngineUtility_sendPlayerHurry);
	LogNoResponse(kHstApi_EngineUtility_sendPlayerHurry);
}

void HstEngineUtility::sendSwapUnits(int iUnitID, MissionTypes eMission, int iData1, int iData2, int iFlags, bool bShift)
{
	HST_API(kHstApi_EngineUtility_sendSwapUnits);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_sendSwapUnits, GetActivePlayer());
	k.aiArg[0] = iUnitID; k.aiArg[1] = eMission; k.aiArg[2] = iData1; k.aiArg[3] = iData2; k.aiArg[4] = iFlags; k.aiArg[5] = bShift;
}

void HstEngineUtility::sendCityBuyPlot(int iCityID, int iX, int iY)
{
	HST_API(kHstApi_EngineUtility_sendCityBuyPlot);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_sendCityBuyPlot, GetActivePlayer());
	k.aiArg[0] = iCityID; k.aiArg[1] = iX; k.aiArg[2] = iY;
}

void HstEngineUtility::sendMinorPledgeProtection(PlayerTypes eMajor, PlayerTypes eMinor, bool bProtect, bool bPledgeNowBroken)
{
	HST_API(kHstApi_EngineUtility_sendMinorPledgeProtection);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_sendMinorPledgeProtection, eMajor);
	k.aiArg[0] = eMinor; k.aiArg[1] = bProtect; k.aiArg[2] = bPledgeNowBroken;
}

void HstEngineUtility::sendMinorGiftGold(PlayerTypes eMinor, int iGold)
{
	HST_API(kHstApi_EngineUtility_sendMinorGiftGold);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_sendMinorGiftGold, GetActivePlayer());
	k.aiArg[0] = eMinor; k.aiArg[1] = iGold;
}

void HstEngineUtility::sendMinorGiftTileImprovement(PlayerTypes eMajor, PlayerTypes eMinor, int iPlotX, int iPlotY)
{
	HST_API(kHstApi_EngineUtility_sendMinorGiftTileImprovement);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_sendMinorGiftTileImprovement, eMajor);
	k.aiArg[0] = eMinor; k.aiArg[1] = iPlotX; k.aiArg[2] = iPlotY;
}

void HstEngineUtility::sendMinorBullyGold(PlayerTypes eBully, PlayerTypes eMinor, int iGold)
{
	HST_API(kHstApi_EngineUtility_sendMinorBullyGold);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_sendMinorBullyGold, eBully);
	k.aiArg[0] = eMinor; k.aiArg[1] = iGold;
}

void HstEngineUtility::sendMinorBullyUnit(PlayerTypes eBully, PlayerTypes eMinor, UnitTypes eUnitType)
{
	HST_API(kHstApi_EngineUtility_sendMinorBullyUnit);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_sendMinorBullyUnit, eBully);
	k.aiArg[0] = eMinor; k.aiArg[1] = eUnitType;
}

void HstEngineUtility::sendMinorBuyout(PlayerTypes eMajor, PlayerTypes eMinor)
{
	HST_API(kHstApi_EngineUtility_sendMinorBuyout);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_sendMinorBuyout, eMajor);
	k.aiArg[0] = eMinor;
}

void HstEngineUtility::sendGreatPersonChoice(PlayerTypes ePlayer, UnitTypes eUnit)
{
	HST_API(kHstApi_EngineUtility_sendGreatPersonChoice);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_sendGreatPersonChoice, ePlayer);
	k.aiArg[0] = eUnit;
}

void HstEngineUtility::sendMayaBonusChoice(PlayerTypes ePlayer, UnitTypes eUnit)
{
	HST_API(kHstApi_EngineUtility_sendMayaBonusChoice);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_sendMayaBonusChoice, ePlayer);
	k.aiArg[0] = eUnit;
}

void HstEngineUtility::sendFaithGreatPersonChoice(PlayerTypes ePlayer, UnitTypes eUnit)
{
	HST_API(kHstApi_EngineUtility_sendFaithGreatPersonChoice);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_sendFaithGreatPersonChoice, ePlayer);
	k.aiArg[0] = eUnit;
}

void HstEngineUtility::sendSetCityAIFocus(int iCityID, CityAIFocusTypes eFocus)
{
	HST_API(kHstApi_EngineUtility_sendSetCityAIFocus);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_sendSetCityAIFocus, GetActivePlayer());
	k.aiArg[0] = iCityID; k.aiArg[1] = eFocus;
}

void HstEngineUtility::sendSetCityAvoidGrowth(int iCityID, bool bAvoidGrowth)
{
	HST_API(kHstApi_EngineUtility_sendSetCityAvoidGrowth);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_sendSetCityAvoidGrowth, GetActivePlayer());
	k.aiArg[0] = iCityID; k.aiArg[1] = bAvoidGrowth;
}

void HstEngineUtility::sendFromUIDiploEvent(PlayerTypes eOtherPlayer, FromUIDiploEventTypes eEvent, int iArg1, int iArg2)
{
	HST_API(kHstApi_EngineUtility_sendFromUIDiploEvent);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_sendFromUIDiploEvent, GetActivePlayer());
	k.aiArg[0] = eOtherPlayer; k.aiArg[1] = eEvent; k.aiArg[2] = iArg1; k.aiArg[3] = iArg2;
}

// Human-offer paths (CvDealAI.cpp). Single mode: the deal object only lives for this call, so the acceptance is applied
// synchronously on the receiving player's deal AI (GUESS: the exe deserializes the deal and calls the same). Peer mode:
// NetDealAccepted carries the serialized deal (net_message_debug.log sizes 165..410) to every peer, the sender included.
static const char s_szHstHex[] = "0123456789abcdef";

static std::string HstDealToHex(ICvDeal1* pDeal)
{
	FMemoryStream kStream;
	pDeal->Write(kStream);
	const unsigned char* pData = (const unsigned char*)kStream.GetBuf();
	const unsigned int uiSize = kStream.GetEOF();
	std::string strHex;
	strHex.reserve(uiSize * 2);
	for(unsigned int i = 0; i < uiSize; ++i)
	{
		strHex += s_szHstHex[pData[i] >> 4];
		strHex += s_szHstHex[pData[i] & 15];
	}
	return strHex;
}

static bool HstHexToBytes(const std::string& strHex, std::string& strOut)
{
	if(strHex.size() % 2)
		return false;
	strOut.resize(strHex.size() / 2);
	for(size_t i = 0; i < strOut.size(); ++i)
	{
		int aiNibble[2];
		for(int j = 0; j < 2; ++j)
		{
			const char c = strHex[i * 2 + j];
			aiNibble[j] = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
			if(aiNibble[j] < 0)
				return false;
		}
		strOut[i] = (char)((aiNibble[0] << 4) | aiNibble[1]);
	}
	return true;
}

void HstEngineUtility::DeliverDealMessage(const HstNetMessage& kMsg)
{
	if(!m_pkCtx)
		return;
	std::string strBytes;
	if(!HstHexToBytes(kMsg.strText, strBytes) || strBytes.empty())
	{
		HstLog("net: %s with a bad deal blob dropped", MethodName(kMsg.eId));
		return;
	}
	std::auto_ptr<ICvGame1> pkGame(m_pkCtx->GetGame());
	std::auto_ptr<ICvGameDeals1> pkDeals(pkGame->GetGameDeals());
	std::auto_ptr<ICvPlayer1> pkTo(pkGame->GetPlayer((PlayerTypes)kMsg.aiArg[0]));
	std::auto_ptr<ICvDealAI1> pkDealAI(pkTo.get() ? pkTo->GetDealAI() : NULL);
	if(!pkDeals.get() || !pkDealAI.get())
		return;
	// The temp deal (CvGameDeals::m_TempDeal, not serialized; every synced user clears it first) holds the received copy:
	// no per-peer allocation of a deal object, so the deterministic heap stays the same on every peer.
	std::auto_ptr<ICvDeal1> pkDeal(pkDeals->GetTempDeal());
	if(!pkDeal.get())
		return;
	FMemoryStream kStream((int)strBytes.size(), strBytes.data());
	pkDeal->Read(kStream);
	if(kMsg.eId == kHstApi_EngineUtility_sendNetDealAccepted)
		pkDealAI->DoAcceptedDeal(kMsg.ePlayer, pkDeal.get(), kMsg.aiArg[1], kMsg.aiArg[2], kMsg.aiArg[3]);
	else
		pkDealAI->DoAcceptedDemand(kMsg.ePlayer, pkDeal.get());
}

void HstEngineUtility::GameplayDiplomacyAILeaderMessage(PlayerTypes ePlayer, DiploUIStateTypes eDiploUIState, const char* szLeaderMessage, LeaderheadAnimationTypes eAction, int iData1)
{
	HstEngineUtility4Gen::GameplayDiplomacyAILeaderMessage(ePlayer, eDiploUIState, szLeaderMessage, eAction, iData1);
	if(IsExeTurns() && g_pkHstLuaUI)
		g_pkHstLuaUI->HostLeaderMessage(ePlayer, (int)eDiploUIState, iData1);
}

void HstEngineUtility::sendNetDealAccepted(PlayerTypes fromPlayer, PlayerTypes toPlayer, ICvDeal1* pDeal, int iDealValueToMe, int iValueImOffering, int iValueTheyreOffering) const
{
	HST_API(kHstApi_EngineUtility_sendNetDealAccepted);
	if(!m_pkCtx || !pDeal)
		return;
	if(m_kPeer.bEnabled)
	{
		HstEngineUtility* pkThis = const_cast<HstEngineUtility*>(this);
		HstNetMessage& k = pkThis->Queue(kHstApi_EngineUtility_sendNetDealAccepted, fromPlayer);
		k.aiArg[0] = toPlayer; k.aiArg[1] = iDealValueToMe; k.aiArg[2] = iValueImOffering; k.aiArg[3] = iValueTheyreOffering;
		k.strText = HstDealToHex(pDeal);
		return;
	}
	std::auto_ptr<ICvGame1> pkGame(m_pkCtx->GetGame());
	std::auto_ptr<ICvPlayer1> pkTo(pkGame->GetPlayer(toPlayer));
	std::auto_ptr<ICvDealAI1> pkDealAI(pkTo.get() ? pkTo->GetDealAI() : NULL);
	if(pkDealAI.get())
		pkDealAI->DoAcceptedDeal(fromPlayer, pDeal, iDealValueToMe, iValueImOffering, iValueTheyreOffering);
}

void HstEngineUtility::sendNetDemandAccepted(PlayerTypes fromPlayer, PlayerTypes toPlayer, ICvDeal1* pDeal) const
{
	HST_API(kHstApi_EngineUtility_sendNetDemandAccepted);
	if(!m_pkCtx || !pDeal)
		return;
	if(m_kPeer.bEnabled)
	{
		HstEngineUtility* pkThis = const_cast<HstEngineUtility*>(this);
		HstNetMessage& k = pkThis->Queue(kHstApi_EngineUtility_sendNetDemandAccepted, fromPlayer);
		k.aiArg[0] = toPlayer;
		k.strText = HstDealToHex(pDeal);
		return;
	}
	std::auto_ptr<ICvGame1> pkGame(m_pkCtx->GetGame());
	std::auto_ptr<ICvPlayer1> pkTo(pkGame->GetPlayer(toPlayer));
	std::auto_ptr<ICvDealAI1> pkDealAI(pkTo.get() ? pkTo->GetDealAI() : NULL);
	if(pkDealAI.get())
		pkDealAI->DoAcceptedDemand(fromPlayer, pDeal);
}

void HstEngineUtility::sendGoodyChoice(PlayerTypes ePlayerID, int iPlotX, int iPlotY, GoodyTypes eGoody, int iUnitID)
{
	HST_API(kHstApi_EngineUtility_sendGoodyChoice);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_sendGoodyChoice, ePlayerID);
	k.aiArg[0] = iPlotX; k.aiArg[1] = iPlotY; k.aiArg[2] = eGoody; k.aiArg[3] = iUnitID;
}

void HstEngineUtility::sendSetSwappableGreatWork(PlayerTypes ePlayer, int iWorkClass, int iWorkIndex)
{
	HST_API(kHstApi_EngineUtility_sendSetSwappableGreatWork);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_sendSetSwappableGreatWork, ePlayer);
	k.aiArg[0] = iWorkClass; k.aiArg[1] = iWorkIndex;
}

void HstEngineUtility::sendSwapGreatWorks(PlayerTypes ePlayer1, int iWork1, PlayerTypes ePlayer2, int iWork2)
{
	HST_API(kHstApi_EngineUtility_sendSwapGreatWorks);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_sendSwapGreatWorks, ePlayer1);
	k.aiArg[0] = iWork1; k.aiArg[1] = ePlayer2; k.aiArg[2] = iWork2;
}

void HstEngineUtility::sendMoveGreatWorks(PlayerTypes ePlayer, int iCity1, int iBuildingClass1, int iWorkIndex1, int iCity2, int iBuildingClass2, int iWorkIndex2)
{
	HST_API(kHstApi_EngineUtility_sendMoveGreatWorks);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_sendMoveGreatWorks, ePlayer);
	k.aiArg[0] = iCity1; k.aiArg[1] = iBuildingClass1; k.aiArg[2] = iWorkIndex1;
	k.aiArg[3] = iCity2; k.aiArg[4] = iBuildingClass2; k.aiArg[5] = iWorkIndex2;
}

void HstEngineUtility::SendLeagueVoteEnact(LeagueTypes eLeague, int iResolutionID, PlayerTypes eVoter, int iNumVotes, int iChoice)
{
	HST_API(kHstApi_EngineUtility_SendLeagueVoteEnact);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_SendLeagueVoteEnact, eVoter);
	k.aiArg[0] = eLeague; k.aiArg[1] = iResolutionID; k.aiArg[2] = iNumVotes; k.aiArg[3] = iChoice;
}

void HstEngineUtility::SendLeagueVoteRepeal(LeagueTypes eLeague, int iResolutionID, PlayerTypes eVoter, int iNumVotes, int iChoice)
{
	HST_API(kHstApi_EngineUtility_SendLeagueVoteRepeal);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_SendLeagueVoteRepeal, eVoter);
	k.aiArg[0] = eLeague; k.aiArg[1] = iResolutionID; k.aiArg[2] = iNumVotes; k.aiArg[3] = iChoice;
}

void HstEngineUtility::SendLeagueVoteAbstain(LeagueTypes eLeague, PlayerTypes eVoter, int iNumVotes)
{
	HST_API(kHstApi_EngineUtility_SendLeagueVoteAbstain);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_SendLeagueVoteAbstain, eVoter);
	k.aiArg[0] = eLeague; k.aiArg[1] = iNumVotes;
}

void HstEngineUtility::SendLeagueProposeEnact(LeagueTypes eLeague, ResolutionTypes eResolution, PlayerTypes eProposer, int iChoice)
{
	HST_API(kHstApi_EngineUtility_SendLeagueProposeEnact);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_SendLeagueProposeEnact, eProposer);
	k.aiArg[0] = eLeague; k.aiArg[1] = eResolution; k.aiArg[2] = iChoice;
}

void HstEngineUtility::SendLeagueProposeRepeal(LeagueTypes eLeague, int iResolutionID, PlayerTypes eProposer)
{
	HST_API(kHstApi_EngineUtility_SendLeagueProposeRepeal);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_SendLeagueProposeRepeal, eProposer);
	k.aiArg[0] = eLeague; k.aiArg[1] = iResolutionID;
}

void HstEngineUtility::SendLeagueEditName(LeagueTypes eLeague, PlayerTypes ePlayer, const char* szCustomName)
{
	HST_API(kHstApi_EngineUtility_SendLeagueEditName);
	HstNetMessage& k = Queue(kHstApi_EngineUtility_SendLeagueEditName, ePlayer);
	k.aiArg[0] = eLeague;
	k.strText = szCustomName ? szCustomName : "";
}

void HstEngineUtility::sendChangeIdeology()
{
	HST_API(kHstApi_EngineUtility_sendChangeIdeology);
	Queue(kHstApi_EngineUtility_sendChangeIdeology, GetActivePlayer());
}

// ---- checkpoint state (M4b) --------------------------------------------------------------------------------------------
// The handshake flags and the queued (not yet delivered) commands are part of the simulation state at a turn boundary:
// commands sent during the Update that advanced the turn are delivered before the next Update.

static void HstPutInt(FDataStream& kStream, int iValue)
{
	kStream.WriteIt(sizeof(int), &iValue);
}

static int HstGetInt(FDataStream& kStream)
{
	int iValue = 0;
	kStream.ReadIt(sizeof(int), &iValue);
	return iValue;
}

void HstEngineUtility::WriteHostState(FDataStream& kStream) const
{
	HstPutInt(kStream, 1);	// version
	for(int i = 0; i < MAX_PLAYERS; ++i)
		HstPutInt(kStream, (m_abTurnCompleteSent[i] ? 1 : 0) | (m_abTurnCompleteReceived[i] ? 2 : 0));
	HstPutInt(kStream, (m_bAICivsProcessedSent ? 1 : 0) | (m_bAICivsProcessedReceived ? 2 : 0) | (m_bInitialAIProcessedSent ? 4 : 0));
	HstPutInt(kStream, (int)m_akNetQueue.size());
	for(size_t i = 0; i < m_akNetQueue.size(); ++i)
	{
		const HstNetMessage& kMsg = m_akNetQueue[i];
		HstPutInt(kStream, (int)kMsg.eId);
		HstPutInt(kStream, (int)kMsg.ePlayer);
		for(int j = 0; j < (int)HST_ARRAY_SIZE(kMsg.aiArg); ++j)
			HstPutInt(kStream, kMsg.aiArg[j]);
		HstPutInt(kStream, (int)kMsg.strText.size());
		if(!kMsg.strText.empty())
			kStream.WriteIt((unsigned int)kMsg.strText.size(), kMsg.strText.data());
	}
}

bool HstEngineUtility::ReadHostState(FDataStream& kStream)
{
	if(HstGetInt(kStream) != 1)
		return false;
	for(int i = 0; i < MAX_PLAYERS; ++i)
	{
		const int iFlags = HstGetInt(kStream);
		m_abTurnCompleteSent[i] = (iFlags & 1) != 0;
		m_abTurnCompleteReceived[i] = (iFlags & 2) != 0;
	}
	const int iFlags = HstGetInt(kStream);
	m_bAICivsProcessedSent = (iFlags & 1) != 0;
	m_bAICivsProcessedReceived = (iFlags & 2) != 0;
	m_bInitialAIProcessedSent = (iFlags & 4) != 0;
	const int iCount = HstGetInt(kStream);
	if(iCount < 0 || iCount > 100000)
		return false;
	m_akNetQueue.clear();
	for(int i = 0; i < iCount; ++i)
	{
		m_akNetQueue.push_back(HstNetMessage());
		HstNetMessage& kMsg = m_akNetQueue.back();
		kMsg.eId = (HstApiId)HstGetInt(kStream);
		kMsg.iPeer = 0;
		kMsg.iTurn = 0;
		kMsg.uiSeq = 0;
		kMsg.ePlayer = (PlayerTypes)HstGetInt(kStream);
		for(int j = 0; j < (int)HST_ARRAY_SIZE(kMsg.aiArg); ++j)
			kMsg.aiArg[j] = HstGetInt(kStream);
		const int iLen = HstGetInt(kStream);
		if(iLen < 0 || iLen > 65536)
			return false;
		kMsg.strText.resize(iLen);
		if(iLen > 0)
			kStream.ReadIt((unsigned int)iLen, &kMsg.strText[0]);
	}
	return true;
}

void HstEngineUtility::ResetHostState()
{
	for(int i = 0; i < MAX_PLAYERS; ++i)
	{
		m_abTurnCompleteSent[i] = false;
		m_abTurnCompleteReceived[i] = false;
	}
	m_bAICivsProcessedSent = false;
	m_bAICivsProcessedReceived = false;
	m_bInitialAIProcessedSent = false;
	m_akNetQueue.clear();
	m_akNetDelivering.clear();
	m_iNetMessagesDelivered = 0;
	// Peer mode: per-game network state (the configuration itself survives).
	for(int i = 0; i < MAX_PLAYERS; ++i)
	{
		m_abAICivsProcessedFrom[i] = false;
		m_aiTurnAllCompleteTurn[i] = -1;	// exe 0x11afd0 (game start reset): +0x14 = -1, +4 = -1
		m_aiAICivsProcessedTurn[i] = -1;
	}
	m_bSentTurnAllComplete = false;
	m_bTurnUnreadySent = false;
	m_iBarrierTurn = -1;					// 0x11afd0: 0x2de0440 = -1
	m_iAllAIProcessedTurn = -1;
	m_iLastTurnAllCompleteTurn = -1;
	m_bInLoopBarrier = false;
	m_iLoopBarrierCount = 0;
	m_iLoopBarrierCompleteCount = 0;
	// Turn slices: NetPlayerReady Execute 0x80c80 at game start (Sync_NetLoopBarrier_Reset 0x125b70 zeroes the slice state first).
	m_bTurnSlicingActive = m_kPeer.bEnabled && m_kPeer.bExeTurns && m_kPeer.bTurnSlices && m_kPeer.iNumPeers > 1;
	m_bQueueOutgoing = m_bTurnSlicingActive;	// 0x3fef90 keeps the flag off with fewer than 2 players
	m_akSendQueue.clear();
	m_iTurnSliceNumber = 0;
	m_iTurnSliceReceived = 0;
	m_bTurnSliceSent = false;
	m_iLastTurnInitialAIProcessed = -1;
	for(int i = 0; i < MAX_PLAYERS; ++i)
		m_aiInitialAIProcessedTurn[i] = -1;
	m_iSelfInFlight = 0;
	m_iTurnSliceFlushes = 0;
	m_iTurnSliceFlushedMsgs = 0;
	m_akOutbox.clear();
	m_iHostTurn = 0;
	m_iAdvanceWanted = -1;
	m_iAdvanceGranted = -1;
	m_uiSeq = 0;
	m_iPeerSent = 0;
	m_iPeerDropped = 0;
	m_iPeerReceived = 0;
}
