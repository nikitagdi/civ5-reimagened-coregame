// Ported from headless/host/src/net/HstLoopback.cpp (single mode)
#include "CvGameCoreDLLPCH.h"

#include "CvDealAI.h"
#include "CvDealClasses.h"
#include "CvNotifications.h"
#include "rt_active.h"
#include "rt_engine.h"
#include "rt_events.h"
#include "lekmod_portable/host_log.h"

namespace lekmod_portable {

void RtEngine::SetContext(ICvGameContext1* ctx)
{
	m_ctx = ctx;
	m_preGame = ctx->GetPreGame();
	// CvDllGameContext::GetNetMessageHandler QIs its member; CvDllNetMessageHandler::QueryInterface returns this (a singleton, never deleted).
	ICvNetMessageHandler1* handler1 = ctx->GetNetMessageHandler();
	m_netHandler = handler1 ? handler1->QueryInterface<ICvNetMessageHandler3>() : NULL;
	if (!m_netHandler) Log(LOG_WARN, "net: no ICvNetMessageHandler3; send* commands will be dropped");
}

PlayerTypes RtEngine::GetActivePlayer() const { return m_preGame ? m_preGame->activePlayer() : NO_PLAYER; }

RtNetMessage& RtEngine::Queue(int id, PlayerTypes player)
{
	std::vector<RtNetMessage>& q = (m_outboxOn && m_coreDepth == 0) ? m_outbox : m_netQueue;
	q.push_back(RtNetMessage());
	RtNetMessage& m = q.back();
	m.eId = id;
	m.ePlayer = player;
	return m;
}

void RtEngine::LogNoResponse(int id)
{
	if (id != kRtNet_sendAutoMoves) CountDropped(id == kRtNet_sendMinorCivQuestNoInterest ? "sendMinorCivQuestNoInterest" : id == kRtNet_sendMinorCivQuestCompleted ? "sendMinorCivQuestCompleted"
		: id == kRtNet_sendMinorCivIntrusion ? "sendMinorCivIntrusion" : id == kRtNet_sendMinorCivEnterTerritory ? "sendMinorCivEnterTerritory" : id == kRtNet_sendPlayerHurry ? "sendPlayerHurry" : "other");
	if (m_noResponseLogged.insert(id).second) Log(LOG_INFO, "net: command %d has no ICvNetMessageHandler3 response; dropped (logged once)", id);
}

int RtEngine::DispatchNetMessages()
{
	if (m_netQueue.empty()) return 0;
	// Responses may send again; those go to the next slice (swap out the queue first).
	m_netDelivering.clear();
	m_netDelivering.swap(m_netQueue);
	for (size_t i = 0; i < m_netDelivering.size(); ++i) Deliver(m_netDelivering[i]);
	const int count = (int)m_netDelivering.size();
	m_netDelivered += count;
	m_netDelivering.clear();
	return count;
}

// ---- turn handshake (single mode; network = the exe turn model, spike S1) ------------------------------------------------
// Network: ported from headless/host/src/net/HstLoopback.cpp (turn_model exe) with one machine that is the peer of every human:
// "every connected human" = every living SS_TAKEN slot (the server seat, SS_OBSERVER, is not waited for).

int RtEngine::TurnNow() const { return GC.getGame().getGameTurn(); }

namespace {
bool NetHuman(ICvPreGame1* pre, int p) { return pre && pre->slotStatus((PlayerTypes)p) == SS_TAKEN && GET_PLAYER((PlayerTypes)p).isAlive(); }
}

// exe 0x2de690: every human has its NetTurnComplete flag (+0xf) set.
bool RtEngine::AllHumansTurnComplete() const
{
	for (int i = 0; i < MAX_PLAYERS; ++i)
		if (NetHuman(m_preGame, i) && !m_turnCompleteReceived[i]) return false;
	return true;
}

// Start of CvGame::doTurn (gDLL->DoTurn()): a new turn clears the per-turn net state.
void RtEngine::DoTurn()
{
	for (int i = 0; i < MAX_PLAYERS; ++i)
	{
		m_turnCompleteSent[i] = false;
		m_turnCompleteReceived[i] = false;
		m_turnUnreadySent[i] = false;
	}
	m_aiCivsProcessedSent = false;
	m_aiCivsProcessedReceived = false;
	// exe new-turn handler 0x1fd5b0 -> 0x35dc20: the sent flags; the turn-stamped fields compare with the game turn and stay.
	m_sentTurnAllComplete = false;
}

void RtEngine::sendTurnComplete()
{
	const PlayerTypes active = GetActivePlayer();
	if (m_network)
	{
		// exe 0x21d540(active): once per turn (until a NetTurnUnready clears the sent flag).
		if (active < 0 || active >= MAX_PLAYERS || m_turnCompleteSent[active]) return;
		m_turnCompleteSent[active] = true;
		// A human may end the turn again after CANCEL_END_TURN: ending the turn re-arms the once-per-turn unready.
		m_turnUnreadySent[active] = false;
		Queue(kRtNet_sendTurnComplete, active);
		// event 12 PLAYER_TURN_ENDED: a human sent the end of its turn; args: player, ended 1. The server seat's own end is not announced.
		if (m_events && NetHuman(m_preGame, (int)active)) m_events->Push(GC_EVENT_PLAYER_TURN_ENDED, 2, (int)active, 1);
		return;
	}
	if (active >= 0 && active < MAX_PLAYERS) m_turnCompleteSent[active] = true;
	Queue(kRtNet_sendTurnComplete, active);
}

bool RtEngine::HasSentTurnComplete()
{
	const PlayerTypes active = GetActivePlayer();
	return active >= 0 && active < MAX_PLAYERS && m_turnCompleteSent[active];
}

bool RtEngine::HasReceivedTurnComplete(PlayerTypes player) { return player >= 0 && player < MAX_PLAYERS && m_turnCompleteReceived[player]; }

// "All complete" = every peer finished its turn; with one local peer it is the same as turn complete.
bool RtEngine::HasSentTurnAllComplete()
{
	if (m_network) return m_sentTurnAllComplete;  // exe 0x3491d0
	const PlayerTypes active = GetActivePlayer();
	return active >= 0 && active < MAX_PLAYERS && m_turnCompleteSent[active];
}

bool RtEngine::HasReceivedTurnAllComplete(PlayerTypes player)
{
	if (m_network) return player >= 0 && player < MAX_PLAYERS && m_turnAllCompleteTurn[player] == TurnNow();  // exe 0x2e74f0
	return player >= 0 && player < MAX_PLAYERS && m_turnCompleteReceived[player];
}

// True when every human (SS_TAKEN) slot has sent turn complete; an all-AI game has none -> true.
bool RtEngine::HasReceivedTurnAllCompleteFromAllPlayers()
{
	if (!m_preGame) return true;
	if (m_network)
	{
		// exe 0x2a0880: every connected human's NetTurnAllComplete is for this game turn.
		const int turn = TurnNow();
		for (int i = 0; i < MAX_PLAYERS; ++i)
			if (NetHuman(m_preGame, i) && m_turnAllCompleteTurn[i] != turn) return false;
		return true;
	}
	for (int i = 0; i < MAX_PLAYERS; ++i)
		if (m_preGame->slotStatus((PlayerTypes)i) == SS_TAKEN && !m_turnCompleteReceived[i]) return false;
	return true;
}

// The un-ready is always possible in single mode (CvPlayer.cpp: "sometimes you can't in MP games").
bool RtEngine::sendTurnUnready()
{
	const PlayerTypes active = GetActivePlayer();
	if (m_network)
	{
		CountExe("sendTurnUnready");
		// exe 0x4c9270 (civhost simplification: the unit tests at its start are not modelled): allowed until NetTurnAllComplete went out;
		// NetTurnUnready once per turn and player, the local sent flag cleared.
		if (m_sentTurnAllComplete || active < 0 || active >= MAX_PLAYERS) return false;
		if (!m_turnUnreadySent[active])
		{
			Queue(kRtNet_sendTurnUnready, active);
			m_turnUnreadySent[active] = true;
			m_turnCompleteSent[active] = false;
			if (m_events && NetHuman(m_preGame, (int)active)) m_events->Push(GC_EVENT_PLAYER_TURN_ENDED, 2, (int)active, 0);  // taken back: ended 0
		}
		return true;
	}
	if (active >= 0 && active < MAX_PLAYERS)
	{
		m_turnCompleteSent[active] = false;
		m_turnCompleteReceived[active] = false;
	}
	return true;
}

// Human-only path (CvGame.cpp); the exe's AutoMoves message has no Response* in ICvNetMessageHandler3 (what NetAutoMoves does in the exe: re-world RW-24).
void RtEngine::sendAutoMoves()
{
	CountDropped("sendAutoMoves");
	LogNoResponse(kRtNet_sendAutoMoves);
}

// Only the exe's turn slices read NetInitialAIProcessed (HstLoopback.cpp); one machine has no slices.
void RtEngine::sendPlayerInitialAIProcessed() const
{
	if (m_network) CountExe("sendPlayerInitialAIProcessed");
}

bool RtEngine::allAIProcessedThisTurn() const { return m_network ? m_allAIProcessedTurn == TurnNow() : m_aiCivsProcessedReceived; }

// CvGame::SetLastTurnAICivsProcessed sends once per turn; with one peer "all received" is this peer's own message.
bool RtEngine::allAICivsProcessedThisTurn() const { return m_network ? m_allAIProcessedTurn == TurnNow() : m_aiCivsProcessedReceived; }

void RtEngine::SendAICivsProcessed() const
{
	m_aiCivsProcessedSent = true;
	RtNetMessage& m = const_cast<RtEngine*>(this)->Queue(kRtNet_SendAICivsProcessed, GetActivePlayer());
	m.aiArg[0] = TurnNow();  // exe 0x218400: NetAICivsProcessed(active player, game turn)
}

bool RtEngine::HasReceivedAICivsProcessed(PlayerTypes player) const
{
	if (m_network) return NetHuman(m_preGame, (int)player) && m_allAIProcessedTurn == TurnNow();  // exe 0x260d40
	return player == GetActivePlayer() && m_aiCivsProcessedReceived;
}

int RtEngine::GetNumAICivsProcessed() const
{
	if (!m_network) return m_aiCivsProcessedReceived ? 1 : 0;
	int n = 0;
	for (int i = 0; i < MAX_PLAYERS; ++i)
		if (NetHuman(m_preGame, i) && m_allAIProcessedTurn == TurnNow()) ++n;
	return n;
}

// exe GameViewState frame 0x3253e0 (HstLoopback.cpp ExeFrame), between game-core updates:
//   barrier turn != turn && all AI civs processed && all humans sent NetTurnComplete:
//     NetTurnAllComplete not sent yet: send it. One machine: it "arrives from every human" at once (+0x14 = turn and
//       SetBusyUnitUpdatesRemaining(600) on each human, Execute 0x1c7d60 -> 0x35cba0);
//     else, no player turn-active and every human's NetTurnAllComplete there: NetLoopBarrier from every peer -> NetLoopBarrierComplete at
//       once (0x7f0d0 -> 0x1a5a10): barrier turn = turn, all-AI-processed reset; CanAdvanceTurn then lets CvGame::update run doTurn.
void RtEngine::ExeFrame()
{
	if (!m_network || !m_preGame) return;
	const int turn = TurnNow();
	if (m_barrierTurn == turn || m_allAIProcessedTurn != turn || !AllHumansTurnComplete()) return;
	if (!m_sentTurnAllComplete)
	{
		m_sentTurnAllComplete = true;
		for (int i = 0; i < MAX_PLAYERS; ++i)
		{
			if (!NetHuman(m_preGame, i)) continue;
			m_turnAllCompleteTurn[i] = turn;
			GET_PLAYER((PlayerTypes)i).setBusyUnitUpdatesRemaining(600);
		}
		return;
	}
	if (GC.getGame().getNumGameTurnActive() != 0 || !HasReceivedTurnAllCompleteFromAllPlayers()) return;
	m_barrierTurn = turn;
	m_allAIProcessedTurn = -1;
}

// ---- exe methods the gamecore calls in a network game (S1 report: counted; answers as the generated stubs / civhost) ------------

bool RtEngine::IsHost() const { CountExe("IsHost"); return EngineUtility::IsHost(); }
// the connection state of gc_slot_control (package P8, rt_slots.cpp). Only a seated human can be on line.
bool RtEngine::SlotConnected(PlayerTypes p) const
{
	return m_network && p >= 0 && p < MAX_PLAYERS && m_connected[p] && m_preGame && m_preGame->slotStatus(p) == SS_TAKEN;
}
void RtEngine::InitSlotConnections()
{
	for (int i = 0; i < MAX_PLAYERS; ++i)
	{
		m_connected[i] = m_preGame && m_preGame->slotStatus((PlayerTypes)i) == SS_TAKEN;
		m_kicked[i] = false;
		m_offlineByLoad[i] = false;
	}
}
void RtEngine::ClearSlotConnections()
{
	for (int i = 0; i < MAX_PLAYERS; ++i)
	{
		m_connected[i] = false;
		m_offlineByLoad[i] = m_network;  // only read for a seated human (the pre-game is not loaded yet here)
	}
}
void RtEngine::MarkTurnEnded(PlayerTypes p)
{
	if (p < 0 || p >= MAX_PLAYERS) return;
	m_turnCompleteSent[p] = true;
	m_turnCompleteReceived[p] = true;
	m_turnUnreadySent[p] = false;
	if (m_events && m_network) m_events->Push(GC_EVENT_PLAYER_TURN_ENDED, 2, (int)p, 1);
}
bool RtEngine::IsPlayerConnected(PlayerTypes p)
{
	CountExe("IsPlayerConnected");
	return SlotConnected(p);
}
// the room host's kick (gc_slot_control KICK sets it before CvPlayer::disconnected asks, CvPlayer.cpp:31127).
bool RtEngine::IsPlayerKicked(int p) { CountExe("IsPlayerKicked"); return p >= 0 && p < MAX_PLAYERS && m_kicked[p]; }
bool RtEngine::IsPlayerHotJoining(int p) { CountExe("IsPlayerHotJoining"); return PortableEngineUtility4Gen::IsPlayerHotJoining(p); }
bool RtEngine::IsPitbossHost() const { CountExe("IsPitbossHost"); return PortableEngineUtility4Gen::IsPitbossHost(); }
int RtEngine::getAssignedNetworkID(int p) { CountExe("getAssignedNetworkID"); return PortableEngineUtility4Gen::getAssignedNetworkID(p); }
void RtEngine::sendPlayerInfo(PlayerTypes p) { CountExe("sendPlayerInfo"); PortableEngineUtility4Gen::sendPlayerInfo(p); }
void RtEngine::sendPlayerOption(PlayerOptionTypes o, bool v) { CountExe("sendPlayerOption"); PortableEngineUtility4Gen::sendPlayerOption(o, v); }
void RtEngine::sendExtendedGame() { CountDropped("sendExtendedGame"); PortableEngineUtility4Gen::sendExtendedGame(); }
void RtEngine::sendPause(int id) { CountDropped("sendPause"); PortableEngineUtility4Gen::sendPause(id); }
void RtEngine::sendTurnReminder(PlayerTypes p) { CountExe("sendTurnReminder"); PortableEngineUtility4Gen::sendTurnReminder(p); }
void RtEngine::FlushTurnReminders() { if (m_network) CountExe("FlushTurnReminders"); PortableEngineUtility4Gen::FlushTurnReminders(); }
void RtEngine::sendPbemTurn(PlayerTypes p) { CountDropped("sendPbemTurn"); PortableEngineUtility4Gen::sendPbemTurn(p); }
// B4 M4 WO-D: the deal of an accepted human offer / demand travels as a message (ported from HstLoopback.cpp peer mode: ICvDeal1::Write as hex).
namespace {
const char kDealHex[] = "0123456789abcdef";

std::string DealToHex(ICvDeal1* d)
{
	FMemoryStream s;
	d->Write(s);
	const unsigned char* p = (const unsigned char*)s.GetBuf();
	const unsigned int n = s.GetEOF();
	std::string out;
	out.reserve(n * 2);
	for (unsigned int i = 0; i < n; ++i)
	{
		out += kDealHex[p[i] >> 4];
		out += kDealHex[p[i] & 15];
	}
	return out;
}

bool HexToBytes(const std::string& hex, std::string& out)
{
	if (hex.size() % 2) return false;
	out.resize(hex.size() / 2);
	for (size_t i = 0; i < out.size(); ++i)
	{
		int v[2];
		for (int j = 0; j < 2; ++j)
		{
			const char c = hex[i * 2 + j];
			v[j] = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
			if (v[j] < 0) return false;
		}
		out[i] = (char)((v[0] << 4) | v[1]);
	}
	return true;
}
}  // namespace

void RtEngine::sendNetDealAccepted(PlayerTypes from, PlayerTypes to, ICvDeal1* d, int a, int b, int c) const
{
	if (!d) return;
	RtNetMessage& k = const_cast<RtEngine*>(this)->Queue(kRtNet_sendNetDealAccepted, from);
	k.aiArg[0] = to; k.aiArg[1] = a; k.aiArg[2] = b; k.aiArg[3] = c;
	k.strText = DealToHex(d);
}
void RtEngine::sendNetDemandAccepted(PlayerTypes from, PlayerTypes to, ICvDeal1* d) const
{
	if (!d) return;
	RtNetMessage& k = const_cast<RtEngine*>(this)->Queue(kRtNet_sendNetDemandAccepted, from);
	k.aiArg[0] = to;
	k.strText = DealToHex(d);
}

// Delivery of the two deal messages (server and journal replay): the receiving AI applies the accepted deal / demand. The temp deal of
// CvGameDeals (not serialized; every user clears it first) holds the copy, as in HstLoopback::DeliverDealMessage.
void RtEngine::DeliverDeal(const RtNetMessage& m)
{
	const PlayerTypes from = m.ePlayer, to = (PlayerTypes)m.aiArg[0];
	if (from < 0 || from >= MAX_MAJOR_CIVS || to < 0 || to >= MAX_MAJOR_CIVS || from == to) return;
	CvPlayer& pTo = GET_PLAYER(to);
	if (!pTo.isAlive() || pTo.isHuman() || !GET_PLAYER(from).isAlive()) return;  // human-human deals are not carried (SIMP-exe-300)
	std::string bytes;
	if (!HexToBytes(m.strText, bytes) || bytes.empty())
	{
		Log(LOG_WARN, "net: deal message with a bad blob dropped");
		return;
	}
	CvDeal* deal = GC.getGame().GetGameDeals()->GetTempDeal();
	if (!deal) return;
	FMemoryStream s((int)bytes.size(), bytes.data());
	std::auto_ptr<ICvDeal1> wrapped = GC.WrapDealPointer(deal);
	wrapped->Read(s);
	// the sender and the receiver are the connection's (never what the blob says)
	if (deal->GetFromPlayer() != from || deal->GetToPlayer() != to) return;
	if (m.eId == kRtNet_sendNetDealAccepted)
		pTo.GetDealAI()->DoAcceptedDeal(from, *deal, m.aiArg[1], m.aiArg[2], m.aiArg[3]);
	else
		pTo.GetDealAI()->DoAcceptedDemand(from, *deal);
}

// ---- B4 fix-mp-trade: deals between two humans (the exe's NetPlayerDealProposed / NetPlayerDealFinalized) ------------------------------------
// The exe: NetPlayerDealPropose (0x78) adds the deal to the proposed list (a counter-offer
// first drops the reverse proposal with FinalizeDeal(to, from, false)) and posts PLAYER_DEAL_RECEIVED to the receiver and PLAYER_DEAL to the proposer, iX = the
// other player; NetPlayerDealFinalized (0x77) forwards to ResponsePlayerDealFinalized(from, to, actBy, accepted).
// SIMPLIFIED(SIMP-exe-300): the exe checks nothing (UI guards only); the server checks the sender, the at-war options of the UI and one proposal per player.
void RtEngine::SendNetDealProposed(PlayerTypes from, PlayerTypes to, ICvDeal1* d)
{
	if (!d) return;
	RtNetMessage& k = Queue(kRtNet_sendNetDealProposed, from);
	k.aiArg[0] = to;
	k.strText = DealToHex(d);
}

void RtEngine::SendNetPlayerDealFinalized(PlayerTypes from, PlayerTypes to, bool bAccepted)
{
	RtNetMessage& k = Queue(kRtNet_sendNetPlayerDealFinalized, GetActivePlayer());
	k.aiArg[0] = from; k.aiArg[1] = to; k.aiArg[2] = bAccepted ? 1 : 0;
}

namespace {
bool LiveHumanMajor(PlayerTypes p) { return p >= 0 && p < MAX_MAJOR_CIVS && GET_PLAYER(p).isAlive() && GET_PLAYER(p).isHuman(); }

// The other player's nick goes into the message, or into the summary for the counter-offer texts (as the exe passes them).
void DealNote(PlayerTypes to, NotificationTypes type, const char* summaryKey, const char* messageKey, PlayerTypes other, bool nickInSummary)
{
	CvNotifications* n = GET_PLAYER(to).GetNotifications();
	if (!n) return;
	Localization::String strSummary = Localization::Lookup(summaryKey);
	Localization::String strMessage = Localization::Lookup(messageKey);
	if (nickInSummary)
		strSummary << GET_PLAYER(other).getNickName();
	else
		strMessage << GET_PLAYER(other).getNickName();
	n->Add(type, strMessage.toUTF8(), strSummary.toUTF8(), other, -1, -1);
}
}  // namespace

// Delivery (server and journal replay): the sender is the connection's seat (never what the message says).
void RtEngine::DeliverPlayerDeal(const RtNetMessage& m)
{
	const PlayerTypes eP = m.ePlayer;
	CvGameDeals* deals = GC.getGame().GetGameDeals();
	if (!deals) return;
	if (m.eId == kRtNet_sendNetPlayerDealFinalized)
	{
		const PlayerTypes from = (PlayerTypes)m.aiArg[0], to = (PlayerTypes)m.aiArg[1];
		const bool accepted = m.aiArg[2] != 0;
		if (from < 0 || from >= MAX_MAJOR_CIVS || to < 0 || to >= MAX_MAJOR_CIVS || from == to) return;
		// The receiver accepts or refuses, the proposer only withdraws. A deal resolved already (both sides clicked) gets the exe's "deal expired" notifications.
		if (eP != to && !(eP == from && !accepted)) return;
		if (!m_netHandler) return;
		m_netHandler->ResponsePlayerDealFinalized(from, to, eP, accepted);
		return;
	}
	const PlayerTypes from = eP, to = (PlayerTypes)m.aiArg[0];
	if (from == to || !LiveHumanMajor(from) || !LiveHumanMajor(to)) return;
	std::string bytes;
	if (!HexToBytes(m.strText, bytes) || bytes.empty())
	{
		Log(LOG_WARN, "net: deal proposal with a bad blob dropped");
		return;
	}
	CvDeal deal;
	{
		FMemoryStream s((int)bytes.size(), bytes.data());
		std::auto_ptr<ICvDeal1> wrapped = GC.WrapDealPointer(&deal);
		wrapped->Read(s);
	}
	if (deal.GetFromPlayer() != from || deal.GetToPlayer() != to || deal.GetNumItems() <= 0) return;
	// TradeLogic's OnOpenPlayerDealScreen: no deal screen at war when war and peace cannot change.
	if (GET_TEAM(GET_PLAYER(from).getTeam()).isAtWar(GET_PLAYER(to).getTeam()) && (GC.getGame().isOption(GAMEOPTION_ALWAYS_WAR) || GC.getGame().isOption(GAMEOPTION_NO_CHANGING_WAR_PEACE)))
		return;
	// One open proposal per pair; a player has one proposal out at a time (DiploCorner's rule), except a counter-offer, which replaces theirs.
	if (deals->ProposedDealExists(from, to)) return;
	const bool counter = deals->ProposedDealExists(to, from);
	if (!counter && deals->HasMadeProposal(from) != NO_PLAYER) return;
	if (counter) deals->FinalizeDeal(to, from, false);
	deals->AddProposedDeal(deal);
	if (counter)
	{
		DealNote(to, NOTIFICATION_PLAYER_DEAL_RECEIVED, "TXT_KEY_DEAL_COUNTER_BY_THEM", "TXT_KEY_DEAL_COUNTER_RECIEVED", from, true);
		DealNote(from, NOTIFICATION_PLAYER_DEAL, "TXT_KEY_DEAL_COUNTER_BY_YOU", "TXT_KEY_DEAL_COUNTER_SENT", to, true);
	}
	else
	{
		DealNote(to, NOTIFICATION_PLAYER_DEAL_RECEIVED, "TXT_KEY_DEAL_OFFERED", "TXT_KEY_DEAL_OFFERED_BY_THEM", from, false);
		DealNote(from, NOTIFICATION_PLAYER_DEAL, "TXT_KEY_DEAL_OFFERED", "TXT_KEY_DEAL_OFFERED_BY_YOU", to, false);
	}
}
void RtEngine::hotJoinComplete() const { CountExe("hotJoinComplete"); PortableEngineUtility4Gen::hotJoinComplete(); }
void RtEngine::netDisconnect() { CountExe("netDisconnect"); PortableEngineUtility4Gen::netDisconnect(); }
void RtEngine::NotifySpecificAILeaderInGame(PlayerTypes p) { CountExe("NotifySpecificAILeaderInGame"); PortableEngineUtility4Gen::NotifySpecificAILeaderInGame(p); }
void RtEngine::ReseatPlayer(PlayerTypes p, bool b) const { CountExe("ReseatPlayer"); PortableEngineUtility4Gen::ReseatPlayer(p, b); }
bool RtEngine::ReseatConnectedPlayer(PlayerTypes p) const { CountExe("ReseatConnectedPlayer"); return PortableEngineUtility4Gen::ReseatConnectedPlayer(p); }
bool RtEngine::IsNetPlayer(PlayerTypes p) const { CountExe("IsNetPlayer"); return PortableEngineUtility4Gen::IsNetPlayer(p); }
void RtEngine::SendGameDoTurnProcessed() const { if (m_network) CountExe("SendGameDoTurnProcessed"); PortableEngineUtility4Gen::SendGameDoTurnProcessed(); }
void RtEngine::VerifyPlayerSlot(PlayerTypes p) const { CountExe("VerifyPlayerSlot"); PortableEngineUtility4Gen::VerifyPlayerSlot(p); }
void RtEngine::openSlot(PlayerTypes p) { CountExe("openSlot"); PortableEngineUtility4Gen::openSlot(p); }
void RtEngine::closeSlot(PlayerTypes p) { CountExe("closeSlot"); PortableEngineUtility4Gen::closeSlot(p); }
void RtEngine::sendUnitSyncCheck(PlayerTypes p, int u, FMemoryStream& s, const std::vector<std::pair<std::string, std::string> >& v) const
{
	CountExe("sendUnitSyncCheck");
	PortableEngineUtility4Gen::sendUnitSyncCheck(p, u, s, v);
}
void RtEngine::sendPlotSyncCheck(PlayerTypes p, short x, short y, FMemoryStream& s, const std::vector<std::pair<std::string, std::string> >& v) const
{
	CountExe("sendPlotSyncCheck");
	PortableEngineUtility4Gen::sendPlotSyncCheck(p, x, y, s, v);
}
void RtEngine::sendCitySyncCheck(PlayerTypes p, int c, FMemoryStream& s, const std::vector<std::pair<std::string, std::string> >& v) const
{
	CountExe("sendCitySyncCheck");
	PortableEngineUtility4Gen::sendCitySyncCheck(p, c, s, v);
}
void RtEngine::sendRandomNumberGeneratorSyncCheck(PlayerTypes p, ICvRandom1* r) const
{
	CountExe("sendRandomNumberGeneratorSyncCheck");
	PortableEngineUtility4Gen::sendRandomNumberGeneratorSyncCheck(p, r);
}
void RtEngine::sendPlayerSyncCheck(PlayerTypes p, FMemoryStream& s, const std::vector<std::pair<std::string, std::string> >& v) const
{
	CountExe("sendPlayerSyncCheck");
	PortableEngineUtility4Gen::sendPlayerSyncCheck(p, s, v);
}

// ---- delivery ------------------------------------------------------------------------------------------------------------

#define A(i) (m.aiArg[i])
#define B(i) (m.aiArg[i] != 0)

void RtEngine::Deliver(const RtNetMessage& m)
{
	const PlayerTypes eP = m.ePlayer;
	switch (m.eId)
	{
	case kRtNet_sendTurnComplete:
		if (eP >= 0 && eP < MAX_PLAYERS) m_turnCompleteReceived[eP] = true;
		return;
	case kRtNet_sendTurnUnready:
		// NetTurnUnready Execute 0x1af980: the player's turn-complete flag (+0xf) is cleared.
		if (eP >= 0 && eP < MAX_PLAYERS) m_turnCompleteReceived[eP] = false;
		return;
	case kRtNet_SendAICivsProcessed:
		m_aiCivsProcessedReceived = true;
		// Network: Execute 0x975b0 stamps the sender; the server is every human's peer, so "every human's stamp is the game turn" at once.
		if (m_network && A(0) == TurnNow()) m_allAIProcessedTurn = A(0);
		return;
	default:
		break;
	}
	ICvNetMessageHandler3* h = m_netHandler;
	if (!h) return;
	// a human's message runs with that human active (quick combat and moves inside Response* see P as the local player).
	const bool asSender = m_network && eP >= 0 && eP < MAX_PLAYERS && NetHuman(m_preGame, (int)eP);
	ActiveAs as(asSender ? eP : GetActivePlayer());
	if (m.eId == kRtNet_sendNetDealAccepted || m.eId == kRtNet_sendNetDemandAccepted)
	{
		DeliverDeal(m);
		return;
	}
	if (m.eId == kRtNet_sendNetDealProposed || m.eId == kRtNet_sendNetPlayerDealFinalized)
	{
		DeliverPlayerDeal(m);
		return;
	}
	switch (m.eId)
	{
	case kRtNet_IdeologyChoice:
		h->ResponseIdeologyChoice(eP, (PolicyBranchTypes)A(0));
		break;
	case kRtNet_ArchaeologyChoice:
		h->ResponseArchaeologyChoice(eP, (ArchaeologyChoiceType)A(1));  // Lua (player, unit index, choice)
		break;
	case kRtNet_SendFoundPantheon:
		h->ResponseFoundPantheon(eP, (BeliefTypes)A(0));
		break;
	case kRtNet_SendFoundReligion:
		h->ResponseFoundReligion(eP, (ReligionTypes)A(0), m.strText.c_str(), (BeliefTypes)A(1), (BeliefTypes)A(2), (BeliefTypes)A(3), (BeliefTypes)A(4), A(5), A(6));
		break;
	case kRtNet_SendEnhanceReligion:
		h->ResponseEnhanceReligion(eP, (ReligionTypes)A(0), m.strText.c_str(), (BeliefTypes)A(1), (BeliefTypes)A(2), A(3), A(4));
		break;
	case kRtNet_SendMoveSpy:
		h->ResponseMoveSpy(eP, A(0), A(1), A(2), B(3));
		break;
	case kRtNet_SendStageCoup:
		h->ResponseStageCoup(eP, A(0));
		break;
	case kRtNet_SendFaithPurchase:
		h->ResponseFaithPurchase(eP, (FaithPurchaseTypes)A(0), A(1));
		break;
	case kRtNet_sendPushMission:
		h->ResponsePushMission(eP, A(0), (MissionTypes)A(1), A(2), A(3), A(4), B(5));
		break;
	case kRtNet_sendAutoMission:
		h->ResponseAutoMission(eP, A(0));
		break;
	case kRtNet_sendDoCommand:
		h->ResponseDoCommand(eP, A(0), (CommandTypes)A(1), A(2), A(3), B(4));
		break;
	case kRtNet_sendPushOrder:
		h->ResponseCityPushOrder(eP, A(0), (OrderTypes)A(1), A(2), B(3), B(4), B(5));
		break;
	case kRtNet_sendPopOrder:
		h->ResponseCityPopOrder(eP, A(0), A(1));
		break;
	case kRtNet_sendSwapOrder:
		h->ResponseCitySwapOrder(eP, A(0), A(1));
		break;
	case kRtNet_sendPurchase_1:
		static_cast<ICvNetMessageHandler1*>(h)->ResponseCityPurchase(eP, A(0), (UnitTypes)A(1), (BuildingTypes)A(2), (ProjectTypes)A(3));	// v2 overload hides v1
		break;
	case kRtNet_sendPurchase_2:
		h->ResponseCityPurchase(eP, A(0), (UnitTypes)A(1), (BuildingTypes)A(2), (ProjectTypes)A(3), A(4));
		break;
	case kRtNet_sendDoTask:
		h->ResponseCityDoTask(eP, A(0), (TaskTypes)A(1), A(2), A(3), B(4), B(5), B(6), B(7));
		break;
	case kRtNet_sendResearch:
		h->ResponseResearch(eP, (TechTypes)A(0), A(1), (PlayerTypes)A(2), B(3));
		break;
	case kRtNet_sendChangeWar:
		h->ResponseChangeWar(eP, (TeamTypes)A(0), B(1));
		break;
	case kRtNet_sendIgnoreWarning:
		h->ResponseIgnoreWarning(eP, (TeamTypes)A(0));
		break;
	case kRtNet_SendPledgeMinorProtection:
		h->ResponsePledgeMinorProtection(eP, (PlayerTypes)A(0), B(1), false);	// GUESS: v1 message has no "pledge broken" flag
		break;
	case kRtNet_SendMinorNoUnitSpawning:
		h->ResponseMinorNoUnitSpawning(eP, (PlayerTypes)A(0), B(1));
		break;
	case kRtNet_SendLiberateMinor:
		h->ResponseLiberatePlayer(eP, (PlayerTypes)A(0), A(1));
		break;
	case kRtNet_sendUpdatePolicies:
		h->ResponseUpdatePolicies(eP, B(0), A(1), B(2));
		break;
	case kRtNet_SendDiploVote:
		h->ResponseDiploVote(eP, (PlayerTypes)A(0));
		break;
	case kRtNet_sendLaunch:
		h->ResponseLaunchSpaceship(eP, (VictoryTypes)A(0));
		break;
	case kRtNet_sendAdvancedStartAction:
		h->ResponseAdvancedStartAction(eP, (AdvancedStartActionTypes)A(0), A(1), A(2), A(3), B(4));
		break;
	case kRtNet_sendBarbarianRansom:
		h->ResponseBarbarianRansom(eP, A(0), A(1));
		break;
	case kRtNet_sendGiftUnit:
		h->ResponseGiftUnit(eP, (PlayerTypes)A(0), A(1));
		break;
	case kRtNet_sendReturnCivilian:
		h->ResponseReturnCivilian(eP, (PlayerTypes)A(1), A(2), B(0));
		break;
	case kRtNet_SendUpdateCityCitizens:
		h->ResponseUpdateCityCitizens(eP, A(0));
		break;
	case kRtNet_SendSellBuilding:
		h->ResponseSellBuilding(eP, A(0), (BuildingTypes)A(1));
		break;
	case kRtNet_SendRenameCity:
		h->ResponseRenameCity(eP, A(0), m.strText.c_str());
		break;
	case kRtNet_SendRenameUnit:
		h->ResponseRenameUnit(eP, A(0), m.strText.c_str());
		break;
	case kRtNet_sendSwapUnits:
		h->ResponseSwapUnits(eP, A(0), (MissionTypes)A(1), A(2), A(3), A(4), B(5));
		break;
	case kRtNet_sendCityBuyPlot:
		h->ResponseCityBuyPlot(eP, A(0), A(1), A(2));
		break;
	case kRtNet_sendMinorPledgeProtection:
		h->ResponsePledgeMinorProtection(eP, (PlayerTypes)A(0), B(1), B(2));
		break;
	case kRtNet_sendMinorGiftGold:
		h->ResponseMinorCivGiftGold(eP, (PlayerTypes)A(0), A(1));
		break;
	case kRtNet_sendMinorGiftTileImprovement:
		h->ResponseMinorCivGiftTileImprovement(eP, (PlayerTypes)A(0), A(1), A(2));
		break;
	case kRtNet_sendMinorBullyGold:
		h->ResponseMinorCivBullyGold(eP, (PlayerTypes)A(0), A(1));
		break;
	case kRtNet_sendMinorBullyUnit:
		h->ResponseMinorCivBullyUnit(eP, (PlayerTypes)A(0), (UnitTypes)A(1));
		break;
	case kRtNet_sendMinorBuyout:
		h->ResponseMinorCivBuyout(eP, (PlayerTypes)A(0));
		break;
	case kRtNet_sendGreatPersonChoice:
		h->ResponseGreatPersonChoice(eP, (UnitTypes)A(0));
		break;
	case kRtNet_sendMayaBonusChoice:
		h->ResponseMayaBonusChoice(eP, (UnitTypes)A(0));
		break;
	case kRtNet_sendFaithGreatPersonChoice:
		h->ResponseFaithGreatPersonChoice(eP, (UnitTypes)A(0));
		break;
	case kRtNet_sendSetCityAIFocus:
		h->ResponseSetCityAIFocus(eP, A(0), (CityAIFocusTypes)A(1));
		break;
	case kRtNet_sendSetCityAvoidGrowth:
		h->ResponseSetCityAvoidGrowth(eP, A(0), B(1));
		break;
	case kRtNet_sendFromUIDiploEvent:
		h->ResponseDiplomacyFromUI(eP, (PlayerTypes)A(0), (FromUIDiploEventTypes)A(1), A(2), A(3));
		break;
	case kRtNet_sendGoodyChoice:
		h->ResponseGoodyChoice(eP, A(0), A(1), (GoodyTypes)A(2), A(3));
		break;
	case kRtNet_sendSetSwappableGreatWork:
		h->ResponseSetSwappableGreatWork(eP, A(0), A(1));
		break;
	case kRtNet_sendSwapGreatWorks:
		h->ResponseSwapGreatWorks(eP, A(0), (PlayerTypes)A(1), A(2));
		break;
	case kRtNet_sendMoveGreatWorks:
		h->ResponseMoveGreatWorks(eP, A(0), A(1), A(2), A(3), A(4), A(5));
		break;
	case kRtNet_SendLeagueVoteEnact:
		h->ResponseLeagueVoteEnact((LeagueTypes)A(0), A(1), eP, A(2), A(3));
		break;
	case kRtNet_SendLeagueVoteRepeal:
		h->ResponseLeagueVoteRepeal((LeagueTypes)A(0), A(1), eP, A(2), A(3));
		break;
	case kRtNet_SendLeagueVoteAbstain:
		h->ResponseLeagueVoteAbstain((LeagueTypes)A(0), eP, A(1));
		break;
	case kRtNet_SendLeagueProposeEnact:
		h->ResponseLeagueProposeEnact((LeagueTypes)A(0), (ResolutionTypes)A(1), eP, A(2));
		break;
	case kRtNet_SendLeagueProposeRepeal:
		h->ResponseLeagueProposeRepeal((LeagueTypes)A(0), A(1), eP);
		break;
	case kRtNet_SendLeagueEditName:
		h->ResponseLeagueEditName((LeagueTypes)A(0), eP, m.strText.c_str());
		break;
	case kRtNet_sendChangeIdeology:
		h->ResponseChangeIdeology(eP);
		break;
	default:
		Log(LOG_WARN, "net: no delivery for queued command %d", m.eId);
		break;
	}
}

#undef A
#undef B

void RtEngine::SendArchaeologyChoice(PlayerTypes ePlayer, int iUnitIndex, ArchaeologyChoiceType eChoice)
{
	RtNetMessage& k = Queue(kRtNet_ArchaeologyChoice, ePlayer);
	k.aiArg[0] = iUnitIndex; k.aiArg[1] = eChoice;
}

// ---- commands ------------------------------------------------------------------------------------------------------------

void RtEngine::SendFoundPantheon(PlayerTypes ePlayer, BeliefTypes eBelief)
{
	RtNetMessage& k = Queue(kRtNet_SendFoundPantheon, ePlayer);
	k.aiArg[0] = eBelief;
}

void RtEngine::SendFoundReligion(PlayerTypes ePlayer, ReligionTypes eReligion, const char* szCustomName, BeliefTypes eBelief1, BeliefTypes eBelief2, BeliefTypes eBelief3, BeliefTypes eBelief4, int iCityX, int iCityY)
{
	RtNetMessage& k = Queue(kRtNet_SendFoundReligion, ePlayer);
	k.aiArg[0] = eReligion; k.aiArg[1] = eBelief1; k.aiArg[2] = eBelief2; k.aiArg[3] = eBelief3; k.aiArg[4] = eBelief4;
	k.aiArg[5] = iCityX; k.aiArg[6] = iCityY;
	k.strText = szCustomName ? szCustomName : "";
}

void RtEngine::SendEnhanceReligion(PlayerTypes ePlayer, ReligionTypes eReligion, const char* szCustomName, BeliefTypes eBelief1, BeliefTypes eBelief2, int iCityX, int iCityY)
{
	RtNetMessage& k = Queue(kRtNet_SendEnhanceReligion, ePlayer);
	k.aiArg[0] = eReligion; k.aiArg[1] = eBelief1; k.aiArg[2] = eBelief2; k.aiArg[3] = iCityX; k.aiArg[4] = iCityY;
	k.strText = szCustomName ? szCustomName : "";
}

void RtEngine::SendMoveSpy(PlayerTypes ePlayer, int iSpyIndex, int iTargetPlayer, int iTargetCity, bool bAsDiplomat)
{
	RtNetMessage& k = Queue(kRtNet_SendMoveSpy, ePlayer);
	k.aiArg[0] = iSpyIndex; k.aiArg[1] = iTargetPlayer; k.aiArg[2] = iTargetCity; k.aiArg[3] = bAsDiplomat;
}

void RtEngine::SendStageCoup(PlayerTypes eSpyPlayer, int iSpyIndex)
{
	RtNetMessage& k = Queue(kRtNet_SendStageCoup, eSpyPlayer);
	k.aiArg[0] = iSpyIndex;
}

void RtEngine::SendFaithPurchase(PlayerTypes ePlayer, FaithPurchaseTypes eFaithPurchaseType, int iFaithPurchaseIndex)
{
	RtNetMessage& k = Queue(kRtNet_SendFaithPurchase, ePlayer);
	k.aiArg[0] = eFaithPurchaseType; k.aiArg[1] = iFaithPurchaseIndex;
}

void RtEngine::sendPushMission(int iUnitID, MissionTypes eMission, int iData1, int iData2, int iFlags, bool bShift)
{
	RtNetMessage& k = Queue(kRtNet_sendPushMission, GetActivePlayer());
	k.aiArg[0] = iUnitID; k.aiArg[1] = eMission; k.aiArg[2] = iData1; k.aiArg[3] = iData2; k.aiArg[4] = iFlags; k.aiArg[5] = bShift;
}

void RtEngine::sendAutoMission(int iUnitID)
{
	RtNetMessage& k = Queue(kRtNet_sendAutoMission, GetActivePlayer());
	k.aiArg[0] = iUnitID;
}

void RtEngine::sendDoCommand(int iUnitID, CommandTypes eCommand, int iData1, int iData2, bool bAlt)
{
	RtNetMessage& k = Queue(kRtNet_sendDoCommand, GetActivePlayer());
	k.aiArg[0] = iUnitID; k.aiArg[1] = eCommand; k.aiArg[2] = iData1; k.aiArg[3] = iData2; k.aiArg[4] = bAlt;
}

void RtEngine::sendPushOrder(int iCityID, OrderTypes eOrder, int iData, bool bAlt, bool bShift, bool bCtrl)
{
	RtNetMessage& k = Queue(kRtNet_sendPushOrder, GetActivePlayer());
	k.aiArg[0] = iCityID; k.aiArg[1] = eOrder; k.aiArg[2] = iData; k.aiArg[3] = bAlt; k.aiArg[4] = bShift; k.aiArg[5] = bCtrl;
}

void RtEngine::sendPopOrder(int iCity, int iNum)
{
	RtNetMessage& k = Queue(kRtNet_sendPopOrder, GetActivePlayer());
	k.aiArg[0] = iCity; k.aiArg[1] = iNum;
}

void RtEngine::sendSwapOrder(int iCity, int iNum0)
{
	RtNetMessage& k = Queue(kRtNet_sendSwapOrder, GetActivePlayer());
	k.aiArg[0] = iCity; k.aiArg[1] = iNum0;
}

void RtEngine::sendPurchase(int iCity, UnitTypes eUnitType, BuildingTypes eBuildingType, ProjectTypes eProjectType)
{
	RtNetMessage& k = Queue(kRtNet_sendPurchase_1, GetActivePlayer());
	k.aiArg[0] = iCity; k.aiArg[1] = eUnitType; k.aiArg[2] = eBuildingType; k.aiArg[3] = eProjectType;
}

void RtEngine::sendPurchase(int iCity, UnitTypes eUnitType, BuildingTypes eBuildingType, ProjectTypes eProjectType, int ePurchaseYield)
{
	RtNetMessage& k = Queue(kRtNet_sendPurchase_2, GetActivePlayer());
	k.aiArg[0] = iCity; k.aiArg[1] = eUnitType; k.aiArg[2] = eBuildingType; k.aiArg[3] = eProjectType; k.aiArg[4] = ePurchaseYield;
}

void RtEngine::sendDoTask(int iCity, TaskTypes eTask, int iData1, int iData2, bool bOption, bool bAlt, bool bShift, bool bCtrl)
{
	RtNetMessage& k = Queue(kRtNet_sendDoTask, GetActivePlayer());
	k.aiArg[0] = iCity; k.aiArg[1] = eTask; k.aiArg[2] = iData1; k.aiArg[3] = iData2;
	k.aiArg[4] = bOption; k.aiArg[5] = bAlt; k.aiArg[6] = bShift; k.aiArg[7] = bCtrl;
}

void RtEngine::sendResearch(TechTypes eTech, int iDiscover, PlayerTypes ePlayerToStealFrom, bool bShift)
{
	RtNetMessage& k = Queue(kRtNet_sendResearch, GetActivePlayer());
	k.aiArg[0] = eTech; k.aiArg[1] = iDiscover; k.aiArg[2] = ePlayerToStealFrom; k.aiArg[3] = bShift;
}

void RtEngine::sendChangeWar(TeamTypes iRivalTeam, bool bWar)
{
	RtNetMessage& k = Queue(kRtNet_sendChangeWar, GetActivePlayer());
	k.aiArg[0] = iRivalTeam; k.aiArg[1] = bWar;
}

void RtEngine::sendIgnoreWarning(TeamTypes iRivalTeam)
{
	RtNetMessage& k = Queue(kRtNet_sendIgnoreWarning, GetActivePlayer());
	k.aiArg[0] = iRivalTeam;
}

void RtEngine::SendPledgeMinorProtection(PlayerTypes eMinor, bool bValue)
{
	RtNetMessage& k = Queue(kRtNet_SendPledgeMinorProtection, GetActivePlayer());
	k.aiArg[0] = eMinor; k.aiArg[1] = bValue;
}

void RtEngine::SendMinorNoUnitSpawning(PlayerTypes eMinor, bool bValue)
{
	RtNetMessage& k = Queue(kRtNet_SendMinorNoUnitSpawning, GetActivePlayer());
	k.aiArg[0] = eMinor; k.aiArg[1] = bValue;
}

void RtEngine::SendLiberateMinor(PlayerTypes eMinor, int iCityID)
{
	RtNetMessage& k = Queue(kRtNet_SendLiberateMinor, GetActivePlayer());
	k.aiArg[0] = eMinor; k.aiArg[1] = iCityID;
}

void RtEngine::sendUpdatePolicies(bool bNOTPolicyBranch, int iID, bool bValue)
{
	RtNetMessage& k = Queue(kRtNet_sendUpdatePolicies, GetActivePlayer());
	k.aiArg[0] = bNOTPolicyBranch; k.aiArg[1] = iID; k.aiArg[2] = bValue;
}

void RtEngine::SendDiploVote(PlayerTypes eVotePlayer)
{
	RtNetMessage& k = Queue(kRtNet_SendDiploVote, GetActivePlayer());
	k.aiArg[0] = eVotePlayer;
}

// CvCity: spaceship completion (AI path too).
void RtEngine::sendLaunch(PlayerTypes ePlayer, VictoryTypes eVictory)
{
	RtNetMessage& k = Queue(kRtNet_sendLaunch, ePlayer);
	k.aiArg[0] = eVictory;
}

void RtEngine::sendAdvancedStartAction(AdvancedStartActionTypes eAction, PlayerTypes ePlayer, int iX, int iY, int iData, bool bAdd)
{
	RtNetMessage& k = Queue(kRtNet_sendAdvancedStartAction, ePlayer);
	k.aiArg[0] = eAction; k.aiArg[1] = iX; k.aiArg[2] = iY; k.aiArg[3] = iData; k.aiArg[4] = bAdd;
}

// The four minor-civ quest/intrusion messages have no Response* in ICvNetMessageHandler1-3.
void RtEngine::sendMinorCivQuestNoInterest(PlayerTypes eMinor, bool bPeace)
{
	LogNoResponse(kRtNet_sendMinorCivQuestNoInterest);
}

void RtEngine::sendMinorCivQuestCompleted(PlayerTypes eMinor, bool bPeace)
{
	LogNoResponse(kRtNet_sendMinorCivQuestCompleted);
}

void RtEngine::sendMinorCivIntrusion(PlayerTypes eMinor, int iOptionChosen)
{
	LogNoResponse(kRtNet_sendMinorCivIntrusion);
}

void RtEngine::sendMinorCivEnterTerritory(PlayerTypes eMinor)
{
	LogNoResponse(kRtNet_sendMinorCivEnterTerritory);
}

void RtEngine::sendBarbarianRansom(int iOptionChosen, int iUnitID)
{
	RtNetMessage& k = Queue(kRtNet_sendBarbarianRansom, GetActivePlayer());
	k.aiArg[0] = iOptionChosen; k.aiArg[1] = iUnitID;
}

// Also used by Lekmod as an in-band "turn timer pause" signal (CvPlayer.cpp: sendGiftUnit(NO_PLAYER, -11)).
void RtEngine::sendGiftUnit(PlayerTypes eMinor, int iUnitID)
{
	RtNetMessage& k = Queue(kRtNet_sendGiftUnit, GetActivePlayer());
	k.aiArg[0] = eMinor; k.aiArg[1] = iUnitID;
}

void RtEngine::sendReturnCivilian(bool bReturn, PlayerTypes ePlayer, int iUnitID)
{
	RtNetMessage& k = Queue(kRtNet_sendReturnCivilian, GetActivePlayer());
	k.aiArg[0] = bReturn; k.aiArg[1] = ePlayer; k.aiArg[2] = iUnitID;
}

void RtEngine::SendUpdateCityCitizens(int iCityID)
{
	RtNetMessage& k = Queue(kRtNet_SendUpdateCityCitizens, GetActivePlayer());
	k.aiArg[0] = iCityID;
}

void RtEngine::SendSellBuilding(int iCityID, BuildingTypes eBuilding)
{
	RtNetMessage& k = Queue(kRtNet_SendSellBuilding, GetActivePlayer());
	k.aiArg[0] = iCityID; k.aiArg[1] = eBuilding;
}

void RtEngine::SendRenameCity(int iCityID, CvString szNewName)
{
	RtNetMessage& k = Queue(kRtNet_SendRenameCity, GetActivePlayer());
	k.aiArg[0] = iCityID;
	k.strText = szNewName.c_str();
}

void RtEngine::SendRenameUnit(int iUnitID, CvString szNewName)
{
	RtNetMessage& k = Queue(kRtNet_SendRenameUnit, GetActivePlayer());
	k.aiArg[0] = iUnitID;
	k.strText = szNewName.c_str();
}

void RtEngine::sendPlayerHurry(HurryTypes eHurry)
{
	LogNoResponse(kRtNet_sendPlayerHurry);
}

void RtEngine::sendSwapUnits(int iUnitID, MissionTypes eMission, int iData1, int iData2, int iFlags, bool bShift)
{
	RtNetMessage& k = Queue(kRtNet_sendSwapUnits, GetActivePlayer());
	k.aiArg[0] = iUnitID; k.aiArg[1] = eMission; k.aiArg[2] = iData1; k.aiArg[3] = iData2; k.aiArg[4] = iFlags; k.aiArg[5] = bShift;
}

void RtEngine::sendCityBuyPlot(int iCityID, int iX, int iY)
{
	RtNetMessage& k = Queue(kRtNet_sendCityBuyPlot, GetActivePlayer());
	k.aiArg[0] = iCityID; k.aiArg[1] = iX; k.aiArg[2] = iY;
}

void RtEngine::sendMinorPledgeProtection(PlayerTypes eMajor, PlayerTypes eMinor, bool bProtect, bool bPledgeNowBroken)
{
	RtNetMessage& k = Queue(kRtNet_sendMinorPledgeProtection, eMajor);
	k.aiArg[0] = eMinor; k.aiArg[1] = bProtect; k.aiArg[2] = bPledgeNowBroken;
}

void RtEngine::sendMinorGiftGold(PlayerTypes eMinor, int iGold)
{
	RtNetMessage& k = Queue(kRtNet_sendMinorGiftGold, GetActivePlayer());
	k.aiArg[0] = eMinor; k.aiArg[1] = iGold;
}

void RtEngine::sendMinorGiftTileImprovement(PlayerTypes eMajor, PlayerTypes eMinor, int iPlotX, int iPlotY)
{
	RtNetMessage& k = Queue(kRtNet_sendMinorGiftTileImprovement, eMajor);
	k.aiArg[0] = eMinor; k.aiArg[1] = iPlotX; k.aiArg[2] = iPlotY;
}

void RtEngine::sendMinorBullyGold(PlayerTypes eBully, PlayerTypes eMinor, int iGold)
{
	RtNetMessage& k = Queue(kRtNet_sendMinorBullyGold, eBully);
	k.aiArg[0] = eMinor; k.aiArg[1] = iGold;
}

void RtEngine::sendMinorBullyUnit(PlayerTypes eBully, PlayerTypes eMinor, UnitTypes eUnitType)
{
	RtNetMessage& k = Queue(kRtNet_sendMinorBullyUnit, eBully);
	k.aiArg[0] = eMinor; k.aiArg[1] = eUnitType;
}

void RtEngine::sendMinorBuyout(PlayerTypes eMajor, PlayerTypes eMinor)
{
	RtNetMessage& k = Queue(kRtNet_sendMinorBuyout, eMajor);
	k.aiArg[0] = eMinor;
}

void RtEngine::sendGreatPersonChoice(PlayerTypes ePlayer, UnitTypes eUnit)
{
	RtNetMessage& k = Queue(kRtNet_sendGreatPersonChoice, ePlayer);
	k.aiArg[0] = eUnit;
}

void RtEngine::sendMayaBonusChoice(PlayerTypes ePlayer, UnitTypes eUnit)
{
	RtNetMessage& k = Queue(kRtNet_sendMayaBonusChoice, ePlayer);
	k.aiArg[0] = eUnit;
}

void RtEngine::sendFaithGreatPersonChoice(PlayerTypes ePlayer, UnitTypes eUnit)
{
	RtNetMessage& k = Queue(kRtNet_sendFaithGreatPersonChoice, ePlayer);
	k.aiArg[0] = eUnit;
}

void RtEngine::sendSetCityAIFocus(int iCityID, CityAIFocusTypes eFocus)
{
	RtNetMessage& k = Queue(kRtNet_sendSetCityAIFocus, GetActivePlayer());
	k.aiArg[0] = iCityID; k.aiArg[1] = eFocus;
}

void RtEngine::sendSetCityAvoidGrowth(int iCityID, bool bAvoidGrowth)
{
	RtNetMessage& k = Queue(kRtNet_sendSetCityAvoidGrowth, GetActivePlayer());
	k.aiArg[0] = iCityID; k.aiArg[1] = bAvoidGrowth;
}

void RtEngine::sendFromUIDiploEvent(PlayerTypes eOtherPlayer, FromUIDiploEventTypes eEvent, int iArg1, int iArg2)
{
	RtNetMessage& k = Queue(kRtNet_sendFromUIDiploEvent, GetActivePlayer());
	k.aiArg[0] = eOtherPlayer; k.aiArg[1] = eEvent; k.aiArg[2] = iArg1; k.aiArg[3] = iArg2;
}

void RtEngine::sendGoodyChoice(PlayerTypes ePlayerID, int iPlotX, int iPlotY, GoodyTypes eGoody, int iUnitID)
{
	RtNetMessage& k = Queue(kRtNet_sendGoodyChoice, ePlayerID);
	k.aiArg[0] = iPlotX; k.aiArg[1] = iPlotY; k.aiArg[2] = eGoody; k.aiArg[3] = iUnitID;
}

void RtEngine::sendSetSwappableGreatWork(PlayerTypes ePlayer, int iWorkClass, int iWorkIndex)
{
	RtNetMessage& k = Queue(kRtNet_sendSetSwappableGreatWork, ePlayer);
	k.aiArg[0] = iWorkClass; k.aiArg[1] = iWorkIndex;
}

void RtEngine::sendSwapGreatWorks(PlayerTypes ePlayer1, int iWork1, PlayerTypes ePlayer2, int iWork2)
{
	RtNetMessage& k = Queue(kRtNet_sendSwapGreatWorks, ePlayer1);
	k.aiArg[0] = iWork1; k.aiArg[1] = ePlayer2; k.aiArg[2] = iWork2;
}

void RtEngine::sendMoveGreatWorks(PlayerTypes ePlayer, int iCity1, int iBuildingClass1, int iWorkIndex1, int iCity2, int iBuildingClass2, int iWorkIndex2)
{
	RtNetMessage& k = Queue(kRtNet_sendMoveGreatWorks, ePlayer);
	k.aiArg[0] = iCity1; k.aiArg[1] = iBuildingClass1; k.aiArg[2] = iWorkIndex1;
	k.aiArg[3] = iCity2; k.aiArg[4] = iBuildingClass2; k.aiArg[5] = iWorkIndex2;
}

void RtEngine::SendLeagueVoteEnact(LeagueTypes eLeague, int iResolutionID, PlayerTypes eVoter, int iNumVotes, int iChoice)
{
	RtNetMessage& k = Queue(kRtNet_SendLeagueVoteEnact, eVoter);
	k.aiArg[0] = eLeague; k.aiArg[1] = iResolutionID; k.aiArg[2] = iNumVotes; k.aiArg[3] = iChoice;
}

void RtEngine::SendLeagueVoteRepeal(LeagueTypes eLeague, int iResolutionID, PlayerTypes eVoter, int iNumVotes, int iChoice)
{
	RtNetMessage& k = Queue(kRtNet_SendLeagueVoteRepeal, eVoter);
	k.aiArg[0] = eLeague; k.aiArg[1] = iResolutionID; k.aiArg[2] = iNumVotes; k.aiArg[3] = iChoice;
}

void RtEngine::SendLeagueVoteAbstain(LeagueTypes eLeague, PlayerTypes eVoter, int iNumVotes)
{
	RtNetMessage& k = Queue(kRtNet_SendLeagueVoteAbstain, eVoter);
	k.aiArg[0] = eLeague; k.aiArg[1] = iNumVotes;
}

void RtEngine::SendLeagueProposeEnact(LeagueTypes eLeague, ResolutionTypes eResolution, PlayerTypes eProposer, int iChoice)
{
	RtNetMessage& k = Queue(kRtNet_SendLeagueProposeEnact, eProposer);
	k.aiArg[0] = eLeague; k.aiArg[1] = eResolution; k.aiArg[2] = iChoice;
}

void RtEngine::SendLeagueProposeRepeal(LeagueTypes eLeague, int iResolutionID, PlayerTypes eProposer)
{
	RtNetMessage& k = Queue(kRtNet_SendLeagueProposeRepeal, eProposer);
	k.aiArg[0] = eLeague; k.aiArg[1] = iResolutionID;
}

void RtEngine::SendLeagueEditName(LeagueTypes eLeague, PlayerTypes ePlayer, const char* szCustomName)
{
	RtNetMessage& k = Queue(kRtNet_SendLeagueEditName, ePlayer);
	k.aiArg[0] = eLeague;
	k.strText = szCustomName ? szCustomName : "";
}

void RtEngine::sendChangeIdeology()
{
	Queue(kRtNet_sendChangeIdeology, GetActivePlayer());
}

}  // namespace lekmod_portable
