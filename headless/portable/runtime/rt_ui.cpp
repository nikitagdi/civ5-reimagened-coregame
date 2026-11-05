// Ported from headless/host/src/engine/HstUserInterface.h (canEndTurn / setCanEndTurn, IsMPAutoEndTurnEnabled, AddNotification / RemoveNotification)
#include "CvGameCoreDLLPCH.h"

#include <string.h>

#include "CvGameCoreUtils.h"

#include "CvDllCity.h"
#include "CvDllPlot.h"
#include "CvDllUnit.h"
#include "CvDllDeal.h"
#include "rt_ui_deal.h"

#include "rt_active.h"
#include "rt_diplo.h"
#include "rt_events.h"
#include "rt_ui.h"
#include "rt_ui_replica.h"

#include "lekmod_portable/host_log.h"

namespace lekmod_portable {

RtUserInterface::RtUserInterface() : m_canEndTurn(false), m_perPlayerCanEndTurn(false), m_hasMovedUnit(false), m_policyNotificationSeen(false), m_diplo(NULL), m_events(NULL), m_replica(NULL)
{
	for (int i = 0; i < MAX_PLAYERS; ++i) m_canEndTurnBy[i] = false;
}

const RtNotification* RtUserInterface::FindNotification(int id) const
{
	std::map<int, RtNotification>::const_iterator it = m_notifications.find(id);
	return it == m_notifications.end() ? NULL : &it->second;
}

bool RtUserInterface::canEndTurn() const
{
	if (!m_perPlayerCanEndTurn) return m_canEndTurn;
	return CanEndTurnOf(CvPreGame::activePlayer());
}

void RtUserInterface::setCanEndTurn(bool bNewValue)
{
	if (!m_perPlayerCanEndTurn)
	{
		m_canEndTurn = bNewValue;
		return;
	}
	if (!bNewValue)
	{
		for (int i = 0; i < MAX_PLAYERS; ++i) m_canEndTurnBy[i] = false;  // CvGame::doTurn: the new turn starts for everybody
		return;
	}
	const PlayerTypes active = CvPreGame::activePlayer();
	if (active >= 0 && active < MAX_PLAYERS) m_canEndTurnBy[active] = true;
}

void RtUserInterface::HostSetCanEndTurn(bool value)
{
	if (!m_perPlayerCanEndTurn)
	{
		m_canEndTurn = value;
		return;
	}
	const PlayerTypes active = CvPreGame::activePlayer();
	if (active >= 0 && active < MAX_PLAYERS) m_canEndTurnBy[active] = value;
}

bool RtUserInterface::isHasMovedUnit() { return m_hasMovedUnit; }

void RtUserInterface::setHasMovedUnit(bool bNewValue) { m_hasMovedUnit = bNewValue; }

// The host ends the human's turn (RtSession::EndTurn); the exe option "auto end turn" is off.
bool RtUserInterface::IsSPAutoEndTurnEnabled() { return false; }

void RtUserInterface::SetSPAutoEndTurnEnabled(bool) {}

bool RtUserInterface::IsMPAutoEndTurnEnabled() { return false; }

void RtUserInterface::SetMPAutoEndTurnEnabled(bool) {}

// The DLL broadcasts a notification to the active player only (CvNotifications::Add / Update), the strings are localized already. A repeated
// broadcast of the same id (CvNotifications::Update after hotseat) refreshes the entry without a new event.
// event 3 NOTIFICATION_ADDED: args id, type (NotificationTypes hash), x, y (-1 = none), data1, data2; text = message, text2 = summary.
void RtUserInterface::AddNotification(int iID, NotificationTypes type, const char* cTooltip, const char* cSummary, int iGameData, int iExtraGameData, PlayerTypes ePlayer, int iX, int iY)
{
	if (CvPreGame::isNetworkMultiplayerGame()) return;  // v2: the notifications of every human are read from the core (SyncNetworkNotifications)
	const bool isNew = m_notifications.find(iID) == m_notifications.end();
	RtNotification& n = m_notifications[iID];
	n.id = iID;
	n.type = (int)type;
	n.gameData = iGameData;
	n.extraGameData = iExtraGameData;
	n.player = (int)ePlayer;
	n.x = iX;
	n.y = iY;
	if (isNew) n.turn = GC.getGame().getGameTurn();
	n.message = cTooltip ? cTooltip : "";
	n.summary = cSummary ? cSummary : "";
	if (isNew && m_events) m_events->Push(GC_EVENT_NOTIFICATION_ADDED, 7, iID, (int)type, iX, iY, iGameData, iExtraGameData, n.message.c_str(), n.summary.c_str(), (int)ePlayer);
}

// event 4 NOTIFICATION_REMOVED: args id, player. Lookup ids count per player, so a removal for another player does not touch the active player's entry.
void RtUserInterface::RemoveNotification(int iID, PlayerTypes ePlayer)
{
	if (CvPreGame::isNetworkMultiplayerGame()) return;
	std::map<int, RtNotification>::iterator it = m_notifications.find(iID);
	if (it == m_notifications.end() || it->second.player != (int)ePlayer) return;
	m_notifications.erase(it);
	if (m_events) m_events->Push(GC_EVENT_NOTIFICATION_REMOVED, 2, iID, (int)ePlayer);
}

// ---- network game: the notifications of every human, read from the core (task N3.7) ----

namespace {
// CvNotifications::Read / Write keep this many records (CvNotifications.cpp MAX_NOTIFICATIONS, a .cpp define: the parse below must consume the stream exactly).
const unsigned kCoreNotificationSlots = 100;
}  // namespace

bool RtUserInterface::ReadCoreNotifications(PlayerTypes who, std::vector<RtNotification>& out)
{
	out.clear();
	if (who < 0 || who >= MAX_MAJOR_CIVS || !GET_PLAYER(who).isEverAlive()) return false;  // only the civs a player can play as have a list (CvPlayer::init)
	CvNotifications* core = GET_PLAYER(who).GetNotifications();
	if (!core) return false;
	FMemoryStream s;
	core->Write(s);
	s.Rewind();
	unsigned version = 0;
	PlayerTypes owner = NO_PLAYER;
	int lookup = 0, begin = 0, end = 0;
	s >> version;
	s >> owner;
	s >> lookup;
	s >> begin;
	s >> end;
	std::vector<CvNotifications::Notification> records(kCoreNotificationSlots);
	for (unsigned i = 0; i < kCoreNotificationSlots; ++i)
	{
		records[i].Clear();
		s >> records[i];
	}
	for (unsigned i = 0; version >= 3 && i < kCoreNotificationSlots; ++i)
	{
		// B4 gc-replica: version 3 carries the rule's broadcast flag and m_bWaitExtraTurn of every record after the list.
		bool ruleNeedsBroadcast = false, waitExtraTurn = false;
		s >> ruleNeedsBroadcast;
		s >> waitExtraTurn;
	}
	if (s.GetPosition() != s.GetEOF() || begin < 0 || end < 0 || begin >= (int)kCoreNotificationSlots || end >= (int)kCoreNotificationSlots)
	{
		Log(LOG_ERROR, "rt_ui: the notification list of player %d does not have the expected layout (CvNotifications::Write)", (int)who);
		return false;
	}
	for (int i = begin; i != end; i = (i + 1) % (int)kCoreNotificationSlots)
	{
		const CvNotifications::Notification& n = records[i];
		if (n.m_bDismissed || n.m_iLookupIndex < 0) continue;
		RtNotification r;
		r.id = n.m_iLookupIndex;
		r.type = (int)n.m_eNotificationType;
		r.gameData = n.m_iGameDataIndex;
		r.extraGameData = n.m_iExtraGameData;
		r.player = (int)who;
		r.x = n.m_iX;
		r.y = n.m_iY;
		r.turn = n.m_iTurn;
		r.message = n.m_strMessage.c_str();
		r.summary = n.m_strSummary.c_str();
		out.push_back(r);
	}
	return true;
}

bool RtUserInterface::FindNotificationOf(PlayerTypes who, int id, RtNotification* out) const
{
	if (!CvPreGame::isNetworkMultiplayerGame())
	{
		const RtNotification* n = FindNotification(id);
		if (!n || n->player != (int)who) return false;
		if (out) *out = *n;
		return true;
	}
	std::vector<RtNotification> live;
	if (!ReadCoreNotifications(who, live)) return false;
	for (size_t i = 0; i < live.size(); ++i)
		if (live[i].id == id)
		{
			if (out) *out = live[i];
			return true;
		}
	return false;
}

void RtUserInterface::NotificationsOf(PlayerTypes who, std::vector<RtNotification>& out) const
{
	out.clear();
	if (CvPreGame::isNetworkMultiplayerGame())
	{
		ReadCoreNotifications(who, out);
		return;
	}
	for (std::map<int, RtNotification>::const_iterator it = m_notifications.begin(); it != m_notifications.end(); ++it)
		if (it->second.player == (int)who) out.push_back(it->second);
}

void RtUserInterface::SyncNetworkNotifications()
{
	if (!m_events || !CvPreGame::isNetworkMultiplayerGame()) return;
	for (int i = 0; i < MAX_MAJOR_CIVS; ++i)
	{
		const CvPlayer& p = GET_PLAYER((PlayerTypes)i);
		std::map<int, RtNotification>& seen = m_seen[i];
		std::vector<RtNotification> live;
		if (p.isHuman() && !p.isObserver()) ReadCoreNotifications((PlayerTypes)i, live);
		else if (seen.empty()) continue;
		std::map<int, RtNotification> now;
		for (size_t k = 0; k < live.size(); ++k) now[live[k].id] = live[k];
		for (std::map<int, RtNotification>::iterator it = seen.begin(); it != seen.end(); ++it)
			if (now.find(it->first) == now.end()) m_events->Push(GC_EVENT_NOTIFICATION_REMOVED, 2, it->first, i);
		for (std::map<int, RtNotification>::const_iterator it = now.begin(); it != now.end(); ++it)
			if (seen.find(it->first) == seen.end())
			{
				const RtNotification& n = it->second;
				RtEvent e = RtEventQueue::Make(GC_EVENT_NOTIFICATION_ADDED, 7, n.id, n.type, n.x, n.y, n.gameData, n.extraGameData, i);
				e.hasText = e.hasText2 = true;
				e.text = n.message;
				e.text2 = n.summary;
				m_events->Push(e);
			}
		seen.swap(now);
	}
}

namespace {

// event 11 MESSAGE (see gamecore_abi.h). Dropped: empty texts and messages for a player the engine UI would not show them to: in a single game anyone but the
// active player (the human), in a network game anyone but a seated human (audit F-05: the active player is the server seat there, so the old filter dropped all).
void PushMessage(RtEventQueue* events, PlayerTypes ePlayer, bool bForce, const char* text, InterfaceMessageTypes eType, int source, int x, int y)
{
	if (!events || !text || !text[0]) return;
	if (CvPreGame::isNetworkMultiplayerGame())
	{
		if (!IsSeatedHuman((int)ePlayer)) return;
	}
	else if (ePlayer != GC.getGame().getActivePlayer())
		return;
	events->Push(GC_EVENT_MESSAGE, 6, (int)ePlayer, x, y, (int)eType, source, bForce ? 1 : 0, text);
}

}  // namespace

namespace {

// B4 M2 WO-1: the lines for the replica's seat go to its UI as MESSAGE (player, force, length, type, x, y, source, owner, id; text, sound).
void SinkMessage(RtUiReplica* rep, PlayerTypes ePlayer, bool bForce, int iLength, const char* text, LPCTSTR sound, InterfaceMessageTypes eType, int source, int x, int y, int owner, int id)
{
	if (!rep || !rep->Active() || ePlayer != rep->Seat() || !text || !text[0]) return;
	RtUiEv ev(GC_UI_EV_MESSAGE);
	ev.A((int)ePlayer).A(bForce ? 1 : 0).A(iLength).A((int)eType).A(x).A(y).A(source).A(owner).A(id).T(text).T2(sound);
	rep->Send(ev);
}

}  // namespace

uint RtUserInterface::AddMessage(uint, PlayerTypes ePlayer, bool bForce, int iLength, const char* strString, LPCTSTR pszSound, InterfaceMessageTypes eType, LPCSTR, ColorTypes, int iFlashX, int iFlashY, bool, bool)
{
	PushMessage(m_events, ePlayer, bForce, strString, eType, 0, iFlashX, iFlashY);
	SinkMessage(m_replica, ePlayer, bForce, iLength, strString, pszSound, eType, 0, iFlashX, iFlashY, -1, -1);
	return 0;
}

uint RtUserInterface::AddUnitMessage(uint, const IDInfo& unitID, PlayerTypes ePlayer, bool bForce, int iLength, const char* szString, LPCTSTR pszSound, InterfaceMessageTypes eType, LPCSTR, ColorTypes, int iFlashX, int iFlashY, bool, bool)
{
	int x = iFlashX, y = iFlashY;
	if (unitID.eOwner >= 0 && unitID.eOwner < MAX_PLAYERS)
	{
		const CvUnit* u = GET_PLAYER(unitID.eOwner).getUnit(unitID.iID);
		if (u) { x = u->getX(); y = u->getY(); }
	}
	PushMessage(m_events, ePlayer, bForce, szString, eType, 1, x, y);
	SinkMessage(m_replica, ePlayer, bForce, iLength, szString, pszSound, eType, 1, x, y, (int)unitID.eOwner, unitID.iID);
	return 0;
}

uint RtUserInterface::AddPlotMessage(uint, int iPlotID, PlayerTypes ePlayer, bool bForce, int iLength, const char* szString, LPCTSTR pszSound, InterfaceMessageTypes eType, LPCSTR, ColorTypes, int iFlashX, int iFlashY, bool, bool)
{
	int x = iFlashX, y = iFlashY;
	const CvPlot* p = iPlotID >= 0 ? GC.getMap().plotByIndex(iPlotID) : NULL;
	if (p) { x = p->getX(); y = p->getY(); }
	PushMessage(m_events, ePlayer, bForce, szString, eType, 2, x, y);
	SinkMessage(m_replica, ePlayer, bForce, iLength, szString, pszSound, eType, 2, x, y, -1, iPlotID);
	return 0;
}

namespace {

// szText of a popup is a text key for some popups (CvUnitMission.cpp: TXT_KEY_ADVISOR_*_BODY) and ready text for others (CvNotifications.cpp).
std::string PopupText(const char* s)
{
	if (!s || !s[0]) return std::string();
	if (strncmp(s, "TXT_KEY_", 8) == 0) return GetLocalizedText(s).GetCString();
	return s;
}

}  // namespace

// event 7 POPUP_INFO: args popup_type (ButtonPopupTypes), data1, data2, data3, player (the player the popup is for: the one the popup names where it names one,
// else the active player at the moment of the popup, who is the human under ActiveAs for what a human does); text = the popup's text when it has one (localized), else NULL.
// Nothing waits for the popup: the gamecore does not block on it (the blocking ones are notifications, see RtSession::CurrentBlocking).
void RtUserInterface::AddPopup(const CvPopupInfo& kPopup)
{
	if (m_replica && m_replica->Active())
	{
		// POPUP: type, data1..3, option1, option2, player (the active player at the moment: the seat inside its own command, else the server seat), flags; text.
		RtUiEv ev(GC_UI_EV_POPUP);
		ev.A((int)kPopup.eButtonPopupType).A(kPopup.iData1).A(kPopup.iData2).A(kPopup.iData3).A(kPopup.bOption1 ? 1 : 0).A(kPopup.bOption2 ? 1 : 0);
		ev.A((int)CvPreGame::activePlayer()).A(kPopup.iFlags).T(kPopup.szText[0] ? kPopup.szText : NULL);
		m_replica->Send(ev);
	}
	if (kPopup.eButtonPopupType == BUTTONPOPUP_CHOOSE_GOODY_HUT_REWARD)
	{
		// CvPlayer.cpp:8583: data1 = player, data2 = the unit that popped the hut.
		PendingGoody g;
		g.player = kPopup.iData1;
		g.unit = kPopup.iData2;
		const CvUnit* unit = (g.player >= 0 && g.player < MAX_PLAYERS) ? GET_PLAYER((PlayerTypes)g.player).getUnit(g.unit) : NULL;
		if (unit) { g.x = unit->getX(); g.y = unit->getY(); }
		m_pendingGoodies.push_back(g);
	}
	if (!m_events) return;
	const std::string text = PopupText(kPopup.szText);
	// The ruin reward names its player (data1); the other popups the core opens only for the active player (isActive / isLocalPlayer branches).
	int target = (int)CvPreGame::activePlayer();
	if (kPopup.eButtonPopupType == BUTTONPOPUP_CHOOSE_GOODY_HUT_REWARD && kPopup.iData1 >= 0 && kPopup.iData1 < MAX_PLAYERS) target = kPopup.iData1;
	m_events->Push(GC_EVENT_POPUP_INFO, 5, (int)kPopup.eButtonPopupType, kPopup.iData1, kPopup.iData2, kPopup.iData3, target, 0, text.empty() ? NULL : text.c_str());
}

// The int-array overload (CvPlayer.cpp:3466, city captured): data1..data3 are the first three ints, -1 when the array is shorter.
void RtUserInterface::AddPopup(int iPopupType, int iIntArraySize, const int* pIntArray, int iBoolArraySize, const bool* pBoolArray)
{
	if (m_replica && m_replica->Active())
	{
		RtUiEv ev(GC_UI_EV_POPUP_ARRAY);
		const int ni = iIntArraySize < 0 ? 0 : (iIntArraySize > 10 ? 10 : iIntArraySize);
		const int nb = iBoolArraySize < 0 ? 0 : (iBoolArraySize > 10 ? 10 : iBoolArraySize);
		ev.A(iPopupType).A(ni);
		for (int i = 0; i < ni; ++i) ev.A(pIntArray ? pIntArray[i] : 0);
		ev.A(nb);
		for (int i = 0; i < nb; ++i) ev.A(pBoolArray && pBoolArray[i] ? 1 : 0);
		m_replica->Send(ev);
	}
	if (!m_events) return;
	int d[3] = {-1, -1, -1};
	for (int i = 0; i < 3 && pIntArray && i < iIntArraySize; ++i) d[i] = pIntArray[i];
	m_events->Push(GC_EVENT_POPUP_INFO, 5, iPopupType, d[0], d[1], d[2], (int)CvPreGame::activePlayer());
}

// CvPlayer::setTurnActive for the active player (the human). event 2 PLAYER_TURN_STARTED: args player. Other (AI) players' turns have no hook.
// A network game: the active player is the server seat, so the event comes from PublishPlayerTurnStatus below, for every human.
void RtUserInterface::PublishActivePlayerTurnStart()
{
	if (m_replica && m_replica->Active())
	{
		RtUiEv ev(GC_UI_EV_ACTIVE_PLAYER_TURN_START);
		m_replica->Send(ev);
	}
	if (m_events && !CvPreGame::isNetworkMultiplayerGame()) m_events->Push(GC_EVENT_PLAYER_TURN_STARTED, 1, (int)GC.getGame().getActivePlayer());
}

// CvPlayer::setTurnActive(true) of any player (CvPlayer.cpp:20981). Network game: event 2 PLAYER_TURN_STARTED for each seated human (in a
// simultaneous turn all at once, in a hybrid turn the war humans one after the other); args player.
void RtUserInterface::PublishPlayerTurnStatus(CvDLLInterfaceIFaceBase::TURN_STATUS_TYPE eStatus, PlayerTypes ePlayer, const char* pszTag)
{
	if (m_replica && m_replica->Active())
	{
		RtUiEv ev(GC_UI_EV_PLAYER_TURN_STATUS);
		ev.A((int)eStatus).A((int)ePlayer).T(pszTag);
		m_replica->Send(ev);
		// The seat's turn started (a network game: the active player is the server seat, so PublishActivePlayerTurnStart does not come for the seat).
		if (eStatus == CvDLLInterfaceIFaceBase::TURN_START && ePlayer == m_replica->Seat() && CvPreGame::isNetworkMultiplayerGame())
		{
			RtUiEv start(GC_UI_EV_ACTIVE_PLAYER_TURN_START);
			m_replica->Send(start);
		}
	}
	// event 16 PLAYER_TURN_STATUS: every setTurnActive(true/false) of any player, for the server that shows the AI phase (the turn queue of the original UI).
	if (m_events && !(m_replica && m_replica->IsReplica()) && (eStatus == CvDLLInterfaceIFaceBase::TURN_START || eStatus == CvDLLInterfaceIFaceBase::TURN_END) && ePlayer >= 0 && ePlayer < MAX_PLAYERS)
		m_events->Push(GC_EVENT_PLAYER_TURN_STATUS, 3, (eStatus == CvDLLInterfaceIFaceBase::TURN_START) ? 0 : 1, (int)ePlayer, GET_PLAYER(ePlayer).isHuman() ? 1 : 0);
	if (!m_events || eStatus != CvDLLInterfaceIFaceBase::TURN_START || !CvPreGame::isNetworkMultiplayerGame()) return;
	if (ePlayer < 0 || ePlayer >= MAX_MAJOR_CIVS) return;
	const CvPlayer& p = GET_PLAYER(ePlayer);
	if (p.isHuman() && !p.isObserver()) m_events->Push(GC_EVENT_PLAYER_TURN_STARTED, 1, (int)ePlayer);
}

bool RtUserInterface::IsPolicyNotificationSeen() const { return m_policyNotificationSeen; }

void RtUserInterface::SetPolicyNotificationSeen(bool bValue) { m_policyNotificationSeen = bValue; }

// True only while an AI leader message to the human is queued and not yet answered (RtDiplo); CvGame::update waits on it while
// IsWaitingForBlockingInput, CvDiplomacyRequests::DoAIDiplomacy does not start a new AI request while it holds.
bool RtUserInterface::isDiploActive() { return m_diplo && m_diplo->Active(); }

bool RtUserInterface::isDiploOrPopupWaiting() { return m_diplo && m_diplo->Active(); }

void RtUserInterface::SetScratchDeal(ICvDeal1* pDeal)
{
	if (m_diplo) m_diplo->SetScratchDeal(pDeal);
	// B4 M4 WO-D: a replica's trade screen reads the same deal (the AI's offer to its human).
	if (m_replica && m_replica->IsReplica() && pDeal)
	{
		CvDeal* src = static_cast<CvDllDeal*>(pDeal)->GetInstance();
		if (src) *RtUiScratchDeal() = *src;
	}
}

// The DLL's own reads (CvDealAI::DoTradeScreenOpened) of the trade screen's deal: on a replica the scratch deal of the seat's UI; elsewhere (the server,
// a host without a UI) a deal of its own that nobody shows. Never NULL: DoTradeScreenOpened writes the peace items into it unchecked, and it also runs on
// the server (FROM_UI_DIPLO_EVENT_HUMAN_NEGOTIATE_PEACE of a human at war with an AI that wants peace, CvDiplomacyAI::DoFromUIDiploEvent as the sender);
// the NULL of the stub crashed the server's worker there (CvDeal::ClearItems on NULL, live game 925, 2026-10-30).
ICvDeal1* RtUserInterface::GetScratchDeal()
{
	if (m_replica && m_replica->IsReplica()) return GC.WrapDealPointer(RtUiScratchDeal()).release();
	static CvDeal* s_hostScratch = NULL;
	if (!s_hostScratch) s_hostScratch = new CvDeal();
	return GC.WrapDealPointer(s_hostScratch).release();
}


// ============================================================================================================================================
// B4 M2 WO-1: the interface methods of the replica role (the table in rt_ui_replica.h says which are "I"). Without the role each one is the generated stub it was.
// ============================================================================================================================================

namespace {

CvUnit* UnitOf(ICvUnit1* u) { return u ? static_cast<CvDllUnit*>(u)->GetInstance() : NULL; }
CvCity* CityOf(ICvCity1* c) { return c ? static_cast<CvDllCity*>(c)->GetInstance() : NULL; }

// The replica's own role-bound state, or NULL (a server, or a game nobody replicates).
inline RtUiReplica* Rep(RtUiReplica* r) { return r && r->IsReplica() ? r : NULL; }

// A sink event with plot arguments x, y from an ICvPlot1.
void SendPlot(RtUiReplica* r, int kind, const ICvPlot1* plot, int extra = 0, bool withExtra = false)
{
	if (!r || !r->Active()) return;
	RtUiEv ev(kind);
	int x, y;
	RtUiReplica::PlotXY(plot, &x, &y);
	ev.A(x).A(y);
	if (withExtra) ev.A(extra);
	r->Send(ev);
}

void SendPlain(RtUiReplica* r, int kind)
{
	if (!r || !r->Active()) return;
	RtUiEv ev(kind);
	r->Send(ev);
}

}  // namespace

void RtUserInterface::lookAtSelectionPlot(bool bRelease)
{
	UserInterface::lookAtSelectionPlot(bRelease);
	if (m_replica && m_replica->Active())
	{
		RtUiEv ev(GC_UI_EV_LOOK_AT_SELECTION_PLOT);
		ev.A(bRelease ? 1 : 0);
		m_replica->Send(ev);
	}
}

bool RtUserInterface::CanDoInterfaceMode(InterfaceModeTypes eInterfaceMode, bool bTestVisible)
{
	if (RtUiReplica* r = Rep(m_replica)) return r->CanDoInterfaceMode(eInterfaceMode, bTestVisible);
	return UserInterface::CanDoInterfaceMode(eInterfaceMode, bTestVisible);
}

ICvPlot1* RtUserInterface::getSelectionPlot()
{
	if (RtUiReplica* r = Rep(m_replica))
	{
		CvUnit* head = r->HeadSelectedUnit();
		CvPlot* plot = head ? head->plot() : NULL;
		return plot ? new CvDllPlot(plot) : NULL;
	}
	return UserInterface::getSelectionPlot();
}

ICvUnit1* RtUserInterface::GetHeadSelectedUnit()
{
	if (RtUiReplica* r = Rep(m_replica))
	{
		CvUnit* head = r->HeadSelectedUnit();
		return head ? new CvDllUnit(head) : NULL;
	}
	return UserInterface::GetHeadSelectedUnit();
}

bool RtUserInterface::IsUnitSelected(PlayerTypes playerID, int unitID)
{
	if (RtUiReplica* r = Rep(m_replica)) return r->IsUnitSelected(playerID, unitID);
	return UserInterface::IsUnitSelected(playerID, unitID);
}

void RtUserInterface::selectUnit(ICvUnit1* pUnit, bool bClear, bool bToggle, bool bSound)
{
	if (RtUiReplica* r = Rep(m_replica)) { r->SelectUnit(UnitOf(pUnit), bClear, bToggle); return; }
	UserInterface::selectUnit(pUnit, bClear, bToggle, bSound);
}

void RtUserInterface::selectGroup(ICvUnit1* pUnit, bool bShift, bool bCtrl, bool bAlt)
{
	if (RtUiReplica* r = Rep(m_replica)) { r->SelectGroup(UnitOf(pUnit), bShift, bCtrl, bAlt); return; }
	UserInterface::selectGroup(pUnit, bShift, bCtrl, bAlt);
}

void RtUserInterface::selectAll(ICvPlot1* pPlot)
{
	if (RtUiReplica* r = Rep(m_replica))
	{
		if (pPlot)
		{
			int x, y;
			pPlot->GetPosition(x, y);
			r->SelectAllAt(GC.getMap().plot(x, y));
		}
		return;
	}
	UserInterface::selectAll(pPlot);
}

bool RtUserInterface::RemoveFromSelectionList(ICvUnit1* pUnit)
{
	if (RtUiReplica* r = Rep(m_replica)) return pUnit && r->RemoveFromSelection(pUnit->GetOwner(), pUnit->GetID());
	return UserInterface::RemoveFromSelectionList(pUnit);
}

bool RtUserInterface::CanSelectionListFound()
{
	if (RtUiReplica* r = Rep(m_replica)) return r->CanSelectionListFound();
	return UserInterface::CanSelectionListFound();
}

ICvUnit1* RtUserInterface::getLastSelectedUnit()
{
	if (RtUiReplica* r = Rep(m_replica))
	{
		CvUnit* u = r->LastSelectedUnit();
		return u ? new CvDllUnit(u) : NULL;
	}
	return UserInterface::getLastSelectedUnit();
}

void RtUserInterface::setLastSelectedUnit(ICvUnit1* pUnit)
{
	if (RtUiReplica* r = Rep(m_replica))
	{
		if (pUnit) r->SetLastSelected(pUnit->GetOwner(), pUnit->GetID()); else r->SetLastSelected(NO_PLAYER, -1);
		return;
	}
	UserInterface::setLastSelectedUnit(pUnit);
}

void RtUserInterface::ClearSelectionList()
{
	if (RtUiReplica* r = Rep(m_replica)) { r->ClearSelection(); return; }
	UserInterface::ClearSelectionList();
}

void RtUserInterface::InsertIntoSelectionList(ICvUnit1* pUnit, bool bClear, bool bToggle, bool bGroup, bool bSound, bool bMinimalChange)
{
	if (RtUiReplica* r = Rep(m_replica))
	{
		CvUnit* u = UnitOf(pUnit);
		if (bGroup) r->SelectGroup(u, !bClear, false, false); else r->SelectUnit(u, bClear, bToggle);
		return;
	}
	UserInterface::InsertIntoSelectionList(pUnit, bClear, bToggle, bGroup, bSound, bMinimalChange);
}

// The replica's selection as the DLL's Lua reads it: Player:GetRecommendedWorkerPlots / GetRecommendedFoundCityPlots (CvLuaPlayer.cpp) walk
// GetSelectedUnits for the first worker / settler; InGame.lua OnUpdateSelection asks for them on every selection change (the tile
// recommendations of the original, live report 2026-10-31). Ref counted like the DLL's CvDllUnit (QueryInterface increments, Destroy
// decrements); a copy of the selection at the call.
namespace {
class RtSelectionEnumerator : public ICvEnumerator
{
public:
	explicit RtSelectionEnumerator(const std::vector<IDInfo>& sel) : m_sel(sel), m_pos(-1), m_ref(1) {}
	void* operator new(size_t n) { return ::operator new(n); }
	void operator delete(void* p) { ::operator delete(p); }
	virtual void* DLLCALL QueryInterface(GUID g)
	{
		if (g == ICvUnknown::GetInterfaceId() || g == ICvEnumerator::GetInterfaceId()) { ++m_ref; return this; }
		return NULL;
	}
	virtual bool DLLCALL MoveNext() { return ++m_pos < (int)m_sel.size(); }
	virtual void DLLCALL Reset() { m_pos = -1; }
	virtual ICvUnknown* DLLCALL GetCurrent()
	{
		if (m_pos < 0 || m_pos >= (int)m_sel.size()) return NULL;
		const IDInfo& id = m_sel[m_pos];
		if (id.eOwner < 0 || id.eOwner >= MAX_PLAYERS) return NULL;
		CvUnit* u = GET_PLAYER(id.eOwner).getUnit(id.iID);
		return u ? new CvDllUnit(u) : NULL;
	}
protected:
	virtual void DLLCALL Destroy() { if (--m_ref == 0) delete this; }
	std::vector<IDInfo> m_sel;
	int m_pos;
	unsigned int m_ref;
};
}  // namespace

ICvEnumerator* RtUserInterface::GetSelectedUnits()
{
	if (RtUiReplica* r = Rep(m_replica)) return new RtSelectionEnumerator(r->Selected());
	return UserInterface::GetSelectedUnits();
}

int RtUserInterface::GetLengthSelectionList()
{
	if (RtUiReplica* r = Rep(m_replica)) return r->SelectionLength();
	return UserInterface::GetLengthSelectionList();
}

void RtUserInterface::selectCity(ICvCity1* pNewValue, bool bTestProduction)
{
	if (RtUiReplica* r = Rep(m_replica)) { r->SelectCity(CityOf(pNewValue)); return; }
	UserInterface::selectCity(pNewValue, bTestProduction);
}

void RtUserInterface::addSelectedCity(ICvCity1* pNewValue, bool bToggle)
{
	if (RtUiReplica* r = Rep(m_replica)) { r->AddSelectedCity(CityOf(pNewValue), bToggle); return; }
	UserInterface::addSelectedCity(pNewValue, bToggle);
}

void RtUserInterface::clearSelectedCities()
{
	if (RtUiReplica* r = Rep(m_replica)) { r->ClearSelectedCities(); return; }
	UserInterface::clearSelectedCities();
}

bool RtUserInterface::isCitySelected(ICvCity1* pCity)
{
	if (RtUiReplica* r = Rep(m_replica)) return pCity && r->IsCitySelected(pCity->GetOwner(), pCity->GetID());
	return UserInterface::isCitySelected(pCity);
}

ICvCity1* RtUserInterface::getHeadSelectedCity()
{
	if (RtUiReplica* r = Rep(m_replica))
	{
		CvCity* c = r->HeadSelectedCity();
		return c ? new CvDllCity(c) : NULL;
	}
	return UserInterface::getHeadSelectedCity();
}

bool RtUserInterface::isCitySelection()
{
	if (RtUiReplica* r = Rep(m_replica)) return !r->SelectedCities().empty();
	return UserInterface::isCitySelection();
}

IDInfo* RtUserInterface::nextSelectedCitiesNode(IDInfo* pNode)
{
	if (RtUiReplica* r = Rep(m_replica))
	{
		std::vector<IDInfo>& v = r->SelectedCities();
		if (!pNode || v.empty() || pNode < &v[0] || pNode + 1 >= &v[0] + v.size()) return NULL;
		return pNode + 1;
	}
	return UserInterface::nextSelectedCitiesNode(pNode);
}

const IDInfo* RtUserInterface::nextSelectedCitiesNode(const IDInfo* pNode) const
{
	if (RtUiReplica* r = Rep(m_replica))
	{
		std::vector<IDInfo>& v = r->SelectedCities();
		if (!pNode || v.empty() || pNode < &v[0] || pNode + 1 >= &v[0] + v.size()) return NULL;
		return pNode + 1;
	}
	return UserInterface::nextSelectedCitiesNode(pNode);
}

IDInfo* RtUserInterface::headSelectedCitiesNode()
{
	if (RtUiReplica* r = Rep(m_replica))
	{
		std::vector<IDInfo>& v = r->SelectedCities();
		return v.empty() ? NULL : &v[0];
	}
	return UserInterface::headSelectedCitiesNode();
}

void RtUserInterface::updateCityScreen(ICvCity1* pOldCity)
{
	UserInterface::updateCityScreen(pOldCity);
	if (m_replica && m_replica->Active())
	{
		RtUiEv ev(GC_UI_EV_UPDATE_CITY_SCREEN);
		ev.A(pOldCity ? (int)pOldCity->GetOwner() : -1).A(pOldCity ? pOldCity->GetID() : -1);
		m_replica->Send(ev);
	}
}

uint RtUserInterface::AddCityMessage(uint, const IDInfo& cityID, PlayerTypes ePlayer, bool bForce, int iLength, const char* szString, LPCTSTR pszSound, InterfaceMessageTypes eType, LPCSTR, ColorTypes, int iFlashX, int iFlashY, bool, bool)
{
	int x = iFlashX, y = iFlashY;
	if (cityID.eOwner >= 0 && cityID.eOwner < MAX_PLAYERS)
	{
		const CvCity* c = GET_PLAYER(cityID.eOwner).getCity(cityID.iID);
		if (c) { x = c->getX(); y = c->getY(); }
	}
	SinkMessage(m_replica, ePlayer, bForce, iLength, szString, pszSound, eType, 3, x, y, (int)cityID.eOwner, cityID.iID);
	return 0;
}

void RtUserInterface::clearEventMessages() { SendPlain(m_replica, GC_UI_EV_CLEAR_EVENT_MESSAGES); }

void RtUserInterface::AddPopupText(int x, int y, const char* szText, float fDelay)
{
	if (m_replica && m_replica->Active() && szText)
	{
		RtUiEv ev(GC_UI_EV_POPUP_TEXT);
		ev.A(x).A(y).A((int)(fDelay * 1000.0f)).T(szText);
		m_replica->Send(ev);
	}
}

void RtUserInterface::PublishActivePlayerTurnEnd() { SendPlain(m_replica, GC_UI_EV_ACTIVE_PLAYER_TURN_END); }
void RtUserInterface::PublishRemotePlayerTurnStart() { SendPlain(m_replica, GC_UI_EV_REMOTE_PLAYER_TURN_START); }
void RtUserInterface::PublishRemotePlayerTurnEnd() { SendPlain(m_replica, GC_UI_EV_REMOTE_PLAYER_TURN_END); }

void RtUserInterface::ActivateNotification(int iID, NotificationTypes type, const char* cString, int iX, int iY, int iGameIndex, int iExtraGameDataValue, PlayerTypes ePlayer)
{
	if (m_replica && m_replica->Active())
	{
		RtUiEv ev(GC_UI_EV_NOTIFICATION_ACTIVATED);
		ev.A(iID).A((int)type).A(iX).A(iY).A(iGameIndex).A(iExtraGameDataValue).A((int)ePlayer).T(cString);
		m_replica->Send(ev);
	}
}

int RtUserInterface::getCycleSelectionCounter()
{
	if (RtUiReplica* r = Rep(m_replica)) return r->CycleCounter();
	return UserInterface::getCycleSelectionCounter();
}

void RtUserInterface::setCycleSelectionCounter(int iNewValue)
{
	if (RtUiReplica* r = Rep(m_replica)) { r->SetCycleCounter(iNewValue); return; }
	UserInterface::setCycleSelectionCounter(iNewValue);
}

void RtUserInterface::changeCycleSelectionCounter(int iChange)
{
	if (RtUiReplica* r = Rep(m_replica)) { r->SetCycleCounter(r->CycleCounter() + iChange); return; }
	UserInterface::changeCycleSelectionCounter(iChange);
}

bool RtUserInterface::IsSelectedUnitRevealingNewPlots() const
{
	if (RtUiReplica* r = Rep(m_replica)) return r->SelectedUnitRevealingNewPlots();
	return UserInterface::IsSelectedUnitRevealingNewPlots();
}

void RtUserInterface::SetSelectedUnitRevealingNewPlots(bool bValue)
{
	if (RtUiReplica* r = Rep(m_replica)) { r->SetSelectedUnitRevealingNewPlots(bValue); return; }
	UserInterface::SetSelectedUnitRevealingNewPlots(bValue);
}

void RtUserInterface::setDirty(InterfaceDirtyBits eDirtyItem, bool bNewValue)
{
	if (m_replica && m_replica->Active())
	{
		RtUiEv ev(GC_UI_EV_SET_DIRTY);
		ev.A((int)eDirtyItem).A(bNewValue ? 1 : 0);
		m_replica->Send(ev);
	}
}

void RtUserInterface::lookAt(const ICvPlot1* pPlot, CameraLookAtTypes type)
{
	SendPlot(m_replica, GC_UI_EV_LOOK_AT, pPlot, (int)type, true);
}

void RtUserInterface::centerCamera(ICvUnit1* p0)
{
	if (m_replica && m_replica->Active())
	{
		RtUiEv ev(GC_UI_EV_CENTER_CAMERA);
		m_replica->PackUnit(ev, p0);
		m_replica->Send(ev);
	}
}

bool RtUserInterface::isCityScreenUp()
{
	if (RtUiReplica* r = Rep(m_replica)) return r->CityScreenUp();
	return UserInterface::isCityScreenUp();
}

void RtUserInterface::setCityScreenUp(bool bUp)
{
	if (RtUiReplica* r = Rep(m_replica)) { r->SetCityScreenUp(bUp); return; }
	UserInterface::setCityScreenUp(bUp);
}

void RtUserInterface::UpdateEndTurn() { SendPlain(m_replica, GC_UI_EV_UPDATE_END_TURN); }

void RtUserInterface::SetEndTurnBlockingChanged(EndTurnBlockingTypes ePrevBlockingType, EndTurnBlockingTypes eNewBlockingType)
{
	if (m_replica && m_replica->Active())
	{
		RtUiEv ev(GC_UI_EV_END_TURN_BLOCKING_CHANGED);
		ev.A((int)ePrevBlockingType).A((int)eNewBlockingType);
		m_replica->Send(ev);
	}
}

void RtUserInterface::setInterfaceMode(InterfaceModeTypes eNewValue)
{
	if (RtUiReplica* r = Rep(m_replica)) { r->SetInterfaceMode(eNewValue); return; }
	UserInterface::setInterfaceMode(eNewValue);
}

InterfaceModeTypes RtUserInterface::getInterfaceMode()
{
	if (RtUiReplica* r = Rep(m_replica)) return r->InterfaceMode();
	return UserInterface::getInterfaceMode();
}

int RtUserInterface::GetInterfaceModeValue()
{
	if (RtUiReplica* r = Rep(m_replica)) return r->InterfaceModeValue();
	return UserInterface::GetInterfaceModeValue();
}

void RtUserInterface::setFlashing(PlayerTypes eWho, bool bFlashing)
{
	if (m_replica && m_replica->Active())
	{
		RtUiEv ev(GC_UI_EV_FLASHING);
		ev.A((int)eWho).A(bFlashing ? 1 : 0);
		m_replica->Send(ev);
	}
}

void RtUserInterface::lookAtCityOffset(int iCity)
{
	if (m_replica && m_replica->Active())
	{
		RtUiEv ev(GC_UI_EV_LOOK_AT_CITY_OFFSET);
		ev.A(iCity);
		m_replica->Send(ev);
	}
}

void RtUserInterface::UpdateCountryBorder(ICvPlot1* pThisPlot) { SendPlot(m_replica, GC_UI_EV_UPDATE_COUNTRY_BORDER, pThisPlot); }
void RtUserInterface::UpdateAllCountryBorders() { SendPlain(m_replica, GC_UI_EV_UPDATE_ALL_COUNTRY_BORDERS); }

void RtUserInterface::SetSpecificCityInfoDirty(ICvCity1* pCity, CityUpdateTypes eUpdateType)
{
	if (m_replica && m_replica->Active())
	{
		RtUiEv ev(GC_UI_EV_SPECIFIC_CITY_INFO_DIRTY);
		m_replica->PackCity(ev, pCity);
		ev.A((int)eUpdateType);
		m_replica->Send(ev);
	}
}

void RtUserInterface::AddHexToUIRange(ICvPlot1* pThisPlot) { SendPlot(m_replica, GC_UI_EV_ADD_HEX_TO_UI_RANGE, pThisPlot); }
void RtUserInterface::OpenEndGameMenu() { SendPlain(m_replica, GC_UI_EV_OPEN_END_GAME_MENU); }
void RtUserInterface::OpenTechTree() { SendPlain(m_replica, GC_UI_EV_OPEN_TECH_TREE); }

void RtUserInterface::OpenPlayerDealScreen(PlayerTypes ePlayer)
{
	if (m_replica && m_replica->Active())
	{
		RtUiEv ev(GC_UI_EV_OPEN_PLAYER_DEAL_SCREEN);
		ev.A((int)ePlayer);
		m_replica->Send(ev);
	}
}

void RtUserInterface::SetTempString(CvString strString)
{
	if (m_replica && m_replica->Active())
	{
		RtUiEv ev(GC_UI_EV_SET_TEMP_STRING);
		ev.T(strString.c_str());
		m_replica->Send(ev);
	}
}

void RtUserInterface::PublishEndTurnDirty() const { SendPlain(m_replica, GC_UI_EV_PUBLISH_END_TURN_DIRTY); }

void RtUserInterface::AddDeferredWonderCommand(WonderCommandType eCommandType, ICvCity1* pThisCity, BuildingTypes eBuildingIndex, int iState)
{
	if (m_replica && m_replica->Active())
	{
		RtUiEv ev(GC_UI_EV_ADD_DEFERRED_WONDER_COMMAND);
		ev.A((int)eCommandType);
		m_replica->PackCity(ev, pThisCity);
		ev.A((int)eBuildingIndex).A(iState);
		m_replica->Send(ev);
	}
}

void RtUserInterface::ToggleStrategicView() { SendPlain(m_replica, GC_UI_EV_TOGGLE_STRATEGIC_VIEW); }

// The seat's notifications in the core, compared with what the previous call saw. The DLL tells the UI about a notification only while its player is the active one
// (CvNotifications::Add / Dismiss), and during a replay the active player is the server seat, so the list itself is the source (as SyncNetworkNotifications does for the server).
void RtUserInterface::SyncReplicaNotifications()
{
	if (!m_replica || !m_replica->Active()) return;
	const PlayerTypes seat = m_replica->Seat();
	if (seat == NO_PLAYER) return;
	std::vector<RtNotification> live;
	ReadCoreNotifications(seat, live);
	std::map<int, RtNotification> now;
	std::set<int>& hidden = m_replica->HiddenNotifications();
	for (size_t i = 0; i < live.size(); ++i) now[live[i].id] = live[i];
	for (std::set<int>::iterator it = hidden.begin(); it != hidden.end();)
	{
		if (now.find(*it) == now.end()) { hidden.erase(it++); continue; }  // gone from the core too
		now.erase(*it);
		++it;
	}
	std::map<int, RtNotification>& seen = m_replica->SeenNotifications();
	for (std::map<int, RtNotification>::const_iterator it = seen.begin(); it != seen.end(); ++it)
		if (now.find(it->first) == now.end())
		{
			RtUiEv ev(GC_UI_EV_NOTIFICATION_REMOVED);
			ev.A(it->first).A((int)seat);
			m_replica->Send(ev);
		}
	for (std::map<int, RtNotification>::const_iterator it = now.begin(); it != now.end(); ++it)
		if (seen.find(it->first) == seen.end())
		{
			const RtNotification& n = it->second;
			RtUiEv ev(GC_UI_EV_NOTIFICATION_ADDED);
			ev.A(n.id).A(n.type).A(n.x).A(n.y).A(n.gameData).A(n.extraGameData).A((int)seat).T(n.message.c_str()).T2(n.summary.c_str());
			m_replica->Send(ev);
		}
	seen.swap(now);
}

}  // namespace lekmod_portable
