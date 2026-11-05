// rt_ui: the UserInterface (DLLUI) the runtime hands to the gamecore (task F1, M4a).
// Overrides the generated stubs that would end the human's turn on their own or hide its state:
//  - IsSPAutoEndTurnEnabled / IsMPAutoEndTurnEnabled are false: CvGame::updateTestEndTurn never sends turn complete by itself; the
//    human's turn ends only through RtSession::EndTurn (CONTROL_ENDTURN / CONTROL_FORCEENDTURN).
//  - canEndTurn is the flag the DLL sets (setCanEndTurn: false at turn start, CvGame.cpp:8770 / 6487) and the host sets after every
//    UpdateTestEndTurn (RtSession::Pump): true while the human's turn has no end-turn blocker and nothing is busy.
//  - isHasMovedUnit / setHasMovedUnit keep the value the DLL sets (CvUnitMission.cpp:245, reset at turn start).
//  - AddNotification / RemoveNotification keep the active player's live notifications by lookup id, so the blocker's
//    notification index (CvPlayer::GetEndTurnBlockingNotificationIndex) can be turned into a unit / city / plot. The event queue
//    for the ABI is task F7 (done): the registry also keeps the localized message / summary and the turn, AddNotification /
//    RemoveNotification / AddPopup / PublishActivePlayerTurnStart push GC_EVENT_NOTIFICATION_ADDED / _REMOVED / POPUP_INFO /
//    PLAYER_TURN_STARTED into the RtEventQueue (rt_events.h); Q_NOTIFICATIONS (abi/gc_notify.cpp) lists the registry.
//    Diplomacy (isDiploActive, rt_diplo) is F6.
// Ported from headless/host/src/engine/HstUserInterface.h (canEndTurn / setCanEndTurn, AddNotification / RemoveNotification parts).
#pragma once

#include <map>
#include <string>
#include <vector>

#include "../host/PortableHost.h"

namespace lekmod_portable {

class RtDiplo;
class RtEventQueue;
class RtUiReplica;

// One notification of the active player as the DLL broadcast it (CvNotifications::Add / Update -> AddNotification).
struct RtNotification
{
	int id;              // lookup index (CvNotifications::Notification::m_iLookupIndex)
	int type;            // NotificationTypes (hash)
	int gameData;        // m_iGameDataIndex
	int extraGameData;   // m_iExtraGameData
	int player;
	int x, y;            // -1 when the notification has no plot
	int turn;            // game turn when the host saw it
	std::string message; // localized text as the DLL built it (CvNotifications m_strMessage)
	std::string summary; // localized title (m_strSummary)
	RtNotification() : id(-1), type(-1), gameData(-1), extraGameData(-1), player(-1), x(-1), y(-1), turn(-1) {}
};

class RtUserInterface : public UserInterface
{
public:
	RtUserInterface();

	// The diplomacy queue (F6): isDiploActive / isDiploOrPopupWaiting are true only while it holds an unanswered message; the scratch deal the DLL
	// sets for an AI offer / request is handed to it. NULL = no diplomacy (always false, as the stubs).
	void SetDiplo(RtDiplo* p) { m_diplo = p; }
	// The event queue (F7); NULL = no events.
	void SetEvents(RtEventQueue* p) { m_events = p; }
	// B4 M2 WO-1: the interface half of a replica (rt_ui_replica.h). In its role the methods marked "I" in that file's table answer from it and send to its sink;
	// without the role (a server, a single game) every method is what it was.
	void SetReplica(RtUiReplica* p) { m_replica = p; }
	// Replica: compares the seat's notifications in the core with the previous call and sends NOTIFICATION_ADDED / _REMOVED to the sink.
	void SyncReplicaNotifications();

	// ---- host side ----
	// A ruin reward the human must pick (F5): CvPlayer::doGoody opens BUTTONPOPUP_CHOOSE_GOODY_HUT_REWARD for a unit with the goody hut picker promotion
	// and keeps no state of its own, so AddPopup records it here until rt_autochoice (flag auto_choices.goody_hut) or the host answers it.
	struct PendingGoody
	{
		int player, unit, x, y;  // the unit's plot when the popup opened
		PendingGoody() : player(-1), unit(-1), x(-1), y(-1) {}
	};
	std::vector<PendingGoody>& PendingGoodies() { return m_pendingGoodies; }
	// The host's verdict after UpdateTestEndTurn (RtSession::Pump / EndTurn); the DLL's own setCanEndTurn calls also land here.
	// Network game: canEndTurn is per human. The gamecore asks canEndTurn() / sets setCanEndTurn(true) for the active player, which
	// is the human under ActiveAs; the turn-start reset setCanEndTurn(false) (CvGame::doTurn) clears everybody's.
	void SetPerPlayerCanEndTurn(bool on) { m_perPlayerCanEndTurn = on; }
	void HostSetCanEndTurn(bool value);
	bool CanEndTurnOf(PlayerTypes who) const { return who >= 0 && who < MAX_PLAYERS && m_canEndTurnBy[who]; }
	// The live notification with this lookup id, or NULL (dismissed / removed, or never broadcast to the active player).
	const RtNotification* FindNotification(int id) const;
	int NotificationCount() const { return (int)m_notifications.size(); }
	// All live notifications, ascending by lookup id (Q_NOTIFICATIONS of a single-type game; the registry above).
	const std::map<int, RtNotification>& Notifications() const { return m_notifications; }

	// ---- v2, network game (task N3.7) ----
	// The CvNotifications of a human are not broadcast to the UI while the server seat is the active player (CvNotifications::Add / Update test the active
	// player), so the registry above stays empty in a network game: the live notifications of any human are read from the core instead, through the public
	// CvNotifications::Write into a memory stream (no DLL change). Ascending by lookup id, dismissed ones left out. False for a player without a list.
	static bool ReadCoreNotifications(PlayerTypes who, std::vector<RtNotification>& out);
	// The live notification of `who` with this lookup id: the registry (single game type) or the core's list (network game). False when there is none.
	bool FindNotificationOf(PlayerTypes who, int id, RtNotification* out) const;
	// The live notifications of `who` as Q_NOTIFICATIONS lists them: the registry in a single game, the core's list in a network game.
	void NotificationsOf(PlayerTypes who, std::vector<RtNotification>& out) const;
	// Network game: compares the core's notifications of every seated human with what the previous call saw and queues NOTIFICATION_ADDED / _REMOVED
	// (args end with the player; the key of a notification is the pair player, id). A human who is not seated any more has his list removed. RtSession::Pump
	// calls it after every pump call; a notification added and dismissed between two calls is never reported.
	void SyncNetworkNotifications();

	// ---- ICvUserInterface2 overrides ----
	virtual bool canEndTurn() const;
	virtual void setCanEndTurn(bool bNewValue);
	virtual bool isHasMovedUnit();
	virtual void setHasMovedUnit(bool bNewValue);
	virtual bool IsSPAutoEndTurnEnabled();
	virtual void SetSPAutoEndTurnEnabled(bool bEnable);
	virtual bool IsMPAutoEndTurnEnabled();
	virtual void SetMPAutoEndTurnEnabled(bool bEnable);
	virtual void AddNotification(int iID, NotificationTypes type, const char* cTooltip, const char* cSummary, int iGameData, int iExtraGameData, PlayerTypes ePlayer, int iX, int iY);
	virtual void RemoveNotification(int iID, PlayerTypes ePlayer);
	// G1: the core's info / combat lines for the active player become GC_EVENT_MESSAGE (no state kept).
	virtual uint AddMessage(uint uiParentEvent, PlayerTypes ePlayer, bool bForce, int iLength, const char* strString, LPCTSTR pszSound, InterfaceMessageTypes eType, LPCSTR pszIcon, ColorTypes eFlashColor, int iFlashX, int iFlashY, bool bShowOffScreenArrows, bool bShowOnScreenArrows);
	virtual uint AddUnitMessage(uint uiParentEvent, const IDInfo& unitID, PlayerTypes ePlayer, bool bForce, int iLength, const char* szString, LPCTSTR pszSound, InterfaceMessageTypes eType, LPCSTR pszIcon, ColorTypes eFlashColor, int iFlashX, int iFlashY, bool bShowOffScreenArrows, bool bShowOnScreenArrows);
	virtual uint AddPlotMessage(uint uiParentEvent, int iPlotID, PlayerTypes ePlayer, bool bForce, int iLength, const char* szString, LPCTSTR pszSound, InterfaceMessageTypes eType, LPCSTR pszIcon, ColorTypes eFlashColor, int iFlashX, int iFlashY, bool bShowOffScreenArrows, bool bShowOnScreenArrows);
	virtual void AddPopup(const CvPopupInfo& kPopup);
	virtual void AddPopup(int iPopupType, int iIntArraySize, const int* pIntArray, int iBoolArraySize, const bool* pBoolArray);
	virtual void PublishActivePlayerTurnStart();
	virtual void PublishPlayerTurnStatus(CvDLLInterfaceIFaceBase::TURN_STATUS_TYPE eStatus, PlayerTypes ePlayer, const char* pszTag);
	// F3: the exe's "policy notification seen" flag (false until the human dismisses a NOTIFICATION_POLICY, CvNotifications::Dismiss;
	// the DLL clears it again, CvPolicyClasses.cpp:3620). The generated stub said true, so the human never got NOTIFICATION_POLICY
	// (CvPlayer.cpp:5359, 13694) and never the POLICY blocker.
	virtual bool IsPolicyNotificationSeen() const;
	virtual void SetPolicyNotificationSeen(bool bValue);
	virtual bool isDiploActive();
	virtual bool isDiploOrPopupWaiting();
	virtual void SetScratchDeal(ICvDeal1* pDeal);
	virtual ICvDeal1* GetScratchDeal();

	// ---- B4 M2 WO-1: interface methods that answer from RtUiReplica / send to its sink in the replica role (table in rt_ui_replica.h) ----
	virtual void lookAtSelectionPlot(bool bRelease);
	virtual bool CanDoInterfaceMode(InterfaceModeTypes eInterfaceMode, bool bTestVisible);
	virtual ICvPlot1* getSelectionPlot();
	virtual ICvUnit1* GetHeadSelectedUnit();
	virtual bool IsUnitSelected(PlayerTypes playerID, int unitID);
	virtual void selectUnit(ICvUnit1* pUnit, bool bClear, bool bToggle, bool bSound);
	virtual void selectGroup(ICvUnit1* pUnit, bool bShift, bool bCtrl, bool bAlt);
	virtual void selectAll(ICvPlot1* pPlot);
	virtual bool RemoveFromSelectionList(ICvUnit1* pUnit);
	virtual bool CanSelectionListFound();
	virtual ICvUnit1* getLastSelectedUnit();
	virtual void setLastSelectedUnit(ICvUnit1* pUnit);
	virtual void ClearSelectionList();
	virtual void InsertIntoSelectionList(ICvUnit1* pUnit, bool bClear, bool bToggle, bool bGroup, bool bSound, bool bMinimalChange);
	virtual int GetLengthSelectionList();
	virtual ICvEnumerator* GetSelectedUnits();
	virtual void selectCity(ICvCity1* pNewValue, bool bTestProduction);
	virtual void addSelectedCity(ICvCity1* pNewValue, bool bToggle);
	virtual void clearSelectedCities();
	virtual bool isCitySelected(ICvCity1* pCity);
	virtual ICvCity1* getHeadSelectedCity();
	virtual bool isCitySelection();
	virtual IDInfo* nextSelectedCitiesNode(IDInfo* pNode);
	virtual const IDInfo* nextSelectedCitiesNode(const IDInfo* pNode) const;
	virtual IDInfo* headSelectedCitiesNode();
	virtual void updateCityScreen(ICvCity1* pOldCity);
	virtual uint AddCityMessage(uint uiParentEvent, const IDInfo& cityID, PlayerTypes ePlayer, bool bForce, int iLength, const char* szString, LPCTSTR pszSound, InterfaceMessageTypes eType, LPCSTR pszIcon, ColorTypes eFlashColor, int iFlashX, int iFlashY, bool bShowOffScreenArrows, bool bShowOnScreenArrows);
	virtual void clearEventMessages();
	virtual void AddPopupText(int x, int y, const char* szText, float fDelay);
	virtual void PublishActivePlayerTurnEnd();
	virtual void PublishRemotePlayerTurnStart();
	virtual void PublishRemotePlayerTurnEnd();
	virtual void ActivateNotification(int iID, NotificationTypes type, const char* cString, int iX, int iY, int iGameIndex, int iExtraGameDataValue, PlayerTypes ePlayer);
	virtual int getCycleSelectionCounter();
	virtual void setCycleSelectionCounter(int iNewValue);
	virtual void changeCycleSelectionCounter(int iChange);
	virtual bool IsSelectedUnitRevealingNewPlots() const;
	virtual void SetSelectedUnitRevealingNewPlots(bool bValue);
	virtual void setDirty(InterfaceDirtyBits eDirtyItem, bool bNewValue);
	virtual void lookAt(const ICvPlot1* pPlot, CameraLookAtTypes type);
	virtual void centerCamera(ICvUnit1* p0);
	virtual bool isCityScreenUp();
	virtual void setCityScreenUp(bool bUp);
	virtual void UpdateEndTurn();
	virtual void SetEndTurnBlockingChanged(EndTurnBlockingTypes ePrevBlockingType, EndTurnBlockingTypes eNewBlockingType);
	virtual void setInterfaceMode(InterfaceModeTypes eNewValue);
	virtual InterfaceModeTypes getInterfaceMode();
	virtual int GetInterfaceModeValue();
	virtual void setFlashing(PlayerTypes eWho, bool bFlashing);
	virtual void lookAtCityOffset(int iCity);
	virtual void UpdateCountryBorder(ICvPlot1* pThisPlot);
	virtual void UpdateAllCountryBorders();
	virtual void SetSpecificCityInfoDirty(ICvCity1* pCity, CityUpdateTypes eUpdateType);
	virtual void AddHexToUIRange(ICvPlot1* pThisPlot);
	virtual void OpenEndGameMenu();
	virtual void OpenTechTree();
	virtual void OpenPlayerDealScreen(PlayerTypes ePlayer);
	virtual void SetTempString(CvString strString);
	virtual void PublishEndTurnDirty() const;
	virtual void AddDeferredWonderCommand(WonderCommandType eCommandType, ICvCity1* pThisCity, BuildingTypes eBuildingIndex, int iState);
	virtual void ToggleStrategicView();

private:
	friend struct RtSaveAccess;  // rt_save.cpp (G4a): the runtime state a save carries
	bool m_canEndTurn;
	bool m_perPlayerCanEndTurn;
	bool m_canEndTurnBy[MAX_PLAYERS];  // network game: not saved, recomputed by the first frame of every human turn
	bool m_hasMovedUnit;
	bool m_policyNotificationSeen;
	RtDiplo* m_diplo;
	RtEventQueue* m_events;
	RtUiReplica* m_replica;
	std::map<int, RtNotification> m_notifications;
	std::map<int, RtNotification> m_seen[MAX_MAJOR_CIVS];  // network game: what SyncNetworkNotifications reported per human (not saved: the first call after a load re-reports)
	std::vector<PendingGoody> m_pendingGoodies;
};

}  // namespace lekmod_portable
