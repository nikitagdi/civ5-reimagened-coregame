//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstEngineUtility.cpp
//!  \brief     Hand-written sim-class methods of ICvEngineUtility4 (see engine-api.csv for the class table).
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#include "HstCommon.h"
#include "engine/HstEngineUtility.h"
#include "platform/HstPlatform.h"

// Virtual turn-slice timing. GUESS: the exe's values are not pinned down yet; only CvGame::getMinutesPlayed
// (turnSlice / getTurnsPerMinute) and the combat mission timer use them, so they must just be constant and non-zero.
static const int s_iHstMillisecsPerTurnSlice = 250;

HstEngineUtility::HstEngineUtility()
	: m_pkScriptSystem(NULL)
	, m_pkCtx(NULL)
	, m_pkPreGame(NULL)
	, m_pkNetHandler(NULL)
	, m_iNetMessagesDelivered(0)
	, m_bAICivsProcessedSent(false)
	, m_bAICivsProcessedReceived(false)
	, m_bInitialAIProcessedSent(false)
	, m_iHostTurn(0)
	, m_iAdvanceWanted(-1)
	, m_iAdvanceGranted(-1)
	, m_uiSeq(0)
	, m_iPeerSent(0)
	, m_iPeerDropped(0)
	, m_iPeerReceived(0)
{
	for(int i = 0; i < MAX_PLAYERS; ++i)
	{
		m_abTurnCompleteSent[i] = false;
		m_abTurnCompleteReceived[i] = false;
		m_abAICivsProcessedFrom[i] = false;
		m_aiTurnAllCompleteTurn[i] = -1;
		m_aiAICivsProcessedTurn[i] = -1;
	}
	m_bSentTurnAllComplete = false;
	m_bTurnUnreadySent = false;
	m_iBarrierTurn = -1;
	m_iAllAIProcessedTurn = -1;
	m_iLastTurnAllCompleteTurn = -1;
	m_bInLoopBarrier = false;
	m_iLoopBarrierCount = 0;
	m_iLoopBarrierCompleteCount = 0;
	m_bQueueOutgoing = false;
	m_bTurnSlicingActive = false;
	m_iTurnSliceNumber = 0;
	m_iTurnSliceReceived = 0;
	m_bTurnSliceSent = false;
	m_iLastTurnInitialAIProcessed = -1;
	for(int i = 0; i < MAX_PLAYERS; ++i)
		m_aiInitialAIProcessedTurn[i] = -1;
	m_iSelfInFlight = 0;
	m_iTurnSliceFlushes = 0;
	m_iTurnSliceFlushedMsgs = 0;
	m_kDropped.eId = (HstApiId)0;
	m_kDropped.ePlayer = NO_PLAYER;
	m_kDropped.iPeer = 0;
	m_kDropped.iTurn = 0;
	m_kDropped.uiSeq = 0;
}

// Single mode: the one local peer is the host. Peer mode: only the configured host peer (exe Sync_Net_IsHost 0x19f470).
// The DLL reads it for sync-check authority of AI objects (CvCity/CvUnit/CvPlayer Sync*), pitboss kicks and pregame.
bool HstEngineUtility::IsHost() const
{
	HST_API(kHstApi_EngineUtility_IsHost);
	return !m_kPeer.bEnabled || m_kPeer.iPeerId == m_kPeer.iHostPeer;
}

void HstEngineUtility::SetContext(ICvGameContext1* pkCtx)
{
	m_pkCtx = pkCtx;
	m_pkPreGame = pkCtx->GetPreGame();
	// CvDllGameContext::GetNetMessageHandler QIs its member; CvDllNetMessageHandler::QueryInterface returns this.
	ICvNetMessageHandler1* pkHandler1 = pkCtx->GetNetMessageHandler();
	m_pkNetHandler = pkHandler1 ? pkHandler1->QueryInterface<ICvNetMessageHandler3>() : NULL;
	if(!m_pkNetHandler)
		HstLog("engine: WARNING no ICvNetMessageHandler3; send* commands will be dropped");
}

void HstEngineUtility::SetCacheFolder(const std::string& strUtf8Folder)
{
	m_strCacheFolder = strUtf8Folder;
}

void HstEngineUtility::SetScriptSystem(ICvEngineScriptSystem1* pkScriptSystem)
{
	m_pkScriptSystem = pkScriptSystem;
}

ICvEngineScriptSystem1* HstEngineUtility::GetScriptSystem()
{
	HST_API(kHstApi_EngineUtility_GetScriptSystem);
	if(!m_pkScriptSystem)
		HstUnimplemented(kHstApi_EngineUtility_GetScriptSystem);
	return m_pkScriptSystem;
}

// CvMap::init / CvMap::reset notify the renderer; nothing to do headless.
void HstEngineUtility::DoMapSetup(int iWorldNumPlots)
{
	HST_API(kHstApi_EngineUtility_DoMapSetup);
}

// All slots are AI in V1 (SS_COMPUTER), so no player is network-connected. turn_model exe: the human slots (and observer
// slots some peer plays) are connected network players, as in a real MP game where everybody is in the session.
bool HstEngineUtility::IsPlayerConnected(PlayerTypes playerId)
{
	HST_API(kHstApi_EngineUtility_IsPlayerConnected);
	return IsExeTurns() && IsNetHuman(playerId);
}

int HstEngineUtility::getMillisecsPerTurn()
{
	HST_API(kHstApi_EngineUtility_getMillisecsPerTurn);
	return s_iHstMillisecsPerTurnSlice;
}

float HstEngineUtility::getSecsPerTurn()
{
	HST_API(kHstApi_EngineUtility_getSecsPerTurn);
	return s_iHstMillisecsPerTurnSlice / 1000.0f;
}

int HstEngineUtility::getTurnsPerSecond()
{
	HST_API(kHstApi_EngineUtility_getTurnsPerSecond);
	return 1000 / s_iHstMillisecsPerTurnSlice;
}

int HstEngineUtility::getTurnsPerMinute()
{
	HST_API(kHstApi_EngineUtility_getTurnsPerMinute);
	return 60000 / s_iHstMillisecsPerTurnSlice;
}

// CvGlobals::init/uninit: "some globals need to be allocated outside the dll" - nothing for a headless host.
void HstEngineUtility::InitGlobals()
{
	HST_API(kHstApi_EngineUtility_InitGlobals);
}

void HstEngineUtility::UninitGlobals()
{
	HST_API(kHstApi_EngineUtility_UninitGlobals);
}

// Checkpoint hooks (M3+): counted only for now.
void HstEngineUtility::SaveGame(SaveGameTypes eSaveGame)
{
	HST_API(kHstApi_EngineUtility_SaveGame);
}

void HstEngineUtility::AutoSave(bool bInitial, bool bPostTurn)
{
	HST_API(kHstApi_EngineUtility_AutoSave);
}

// I1: CvGame::reset deletes and recreates <cache>Civ5SavedGameDatabase.db; it must land in the run dir.
const char* HstEngineUtility::GetCacheFolderPath()
{
	HST_API(kHstApi_EngineUtility_GetCacheFolderPath);
	return m_strCacheFolder.c_str();
}
