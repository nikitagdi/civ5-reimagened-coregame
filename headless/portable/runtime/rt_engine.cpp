// Ported from headless/host/src/engine/HstEngineUtility.cpp (single-player subset)
#include "CvGameCoreDLLPCH.h"

#include <string.h>

#include "rt_active.h"
#include "rt_diplo.h"
#include "rt_engine.h"
#include "rt_events.h"
#include "rt_ui_replica.h"

namespace lekmod_portable {

RtEngine::RtEngine()
	: m_scriptSystem(NULL), m_virtualMs(0), m_diplo(NULL), m_events(NULL), m_uiRep(NULL), m_ctx(NULL), m_preGame(NULL), m_netHandler(NULL), m_netDelivered(0), m_aiCivsProcessedSent(false), m_aiCivsProcessedReceived(false),
	  m_network(false), m_allAIProcessedTurn(-1), m_barrierTurn(-1), m_sentTurnAllComplete(false), m_outboxOn(false), m_coreDepth(0)
{
	memset(m_war, 0, sizeof(m_war));
	for (int i = 0; i < MAX_PLAYERS; ++i)
	{
		m_turnCompleteSent[i] = false;
		m_turnCompleteReceived[i] = false;
		m_turnAllCompleteTurn[i] = -1;
		m_turnUnreadySent[i] = false;
		m_connected[i] = false;
		m_kicked[i] = false;
		m_offlineByLoad[i] = false;
	}
}

ICvEngineScriptSystem1* RtEngine::GetScriptSystem() { return m_scriptSystem; }

namespace {
bool TeamHasSeatedHuman(TeamTypes team)
{
	for (int i = 0; i < MAX_MAJOR_CIVS; ++i)
		if (IsSeatedHuman(i) && GET_PLAYER((PlayerTypes)i).getTeam() == team) return true;
	return false;
}
}  // namespace

// CvTeam::setAtWar calls this for both directions and also when the value did not change (game setup sets every pair to peace): an event
// only when the pair's state differs from the last one reported. args: team_a (the lower id), team_b, at_war.
void RtEngine::GameplayWarStateChanged(TeamTypes eTeam1, TeamTypes eTeam2, bool bWar)
{
	if (eTeam1 < 0 || eTeam2 < 0 || eTeam1 >= MAX_TEAMS || eTeam2 >= MAX_TEAMS || eTeam1 == eTeam2) return;
	const int a = eTeam1 < eTeam2 ? (int)eTeam1 : (int)eTeam2;
	const int b = eTeam1 < eTeam2 ? (int)eTeam2 : (int)eTeam1;
	if (m_war[a][b] == bWar) return;
	m_war[a][b] = bWar;
	if (m_events) m_events->Push(GC_EVENT_WAR_STATE, 3, a, b, bWar ? 1 : 0);
	if (m_uiRep && m_uiRep->Active())
	{
		RtUiEv ev(GC_UI_EV_WAR_STATE_CHANGED);
		ev.A(a).A(b).A(bWar ? 1 : 0);
		m_uiRep->Send(ev);
	}
	// event 15 HUMAN_WAR_DECLARED: a declaration between two teams with a seated human each. CvTeam::declareWar sets its own side first
	// (CvTeam.cpp:1312), so the first reported change names the declarer. extends_timer: a side moves simultaneously now, the condition of Lekmod's rule that
	// refills the turn timer (CvTeam.cpp:1417-1424); the server applies the extension.
	if (m_events && m_network && bWar && TeamHasSeatedHuman(eTeam1) && TeamHasSeatedHuman(eTeam2))
	{
		const int extends = (GET_TEAM(eTeam1).isSimultaneousTurns() || GET_TEAM(eTeam2).isSimultaneousTurns()) ? 1 : 0;
		m_events->Push(GC_EVENT_HUMAN_WAR_DECLARED, 3, (int)eTeam1, (int)eTeam2, extends);
	}
}

int RtEngine::getMillisecsPerTurn() { return STEP_MS; }
float RtEngine::getSecsPerTurn() { return STEP_MS / 1000.0f; }
int RtEngine::getTurnsPerSecond() { return 1000 / STEP_MS; }
// CvGame.cpp:5103 divides by this; the generated stub returned 0 (a trap in wasm).
int RtEngine::getTurnsPerMinute() { return 60000 / STEP_MS; }

void RtEngine::DoMapSetup(int) {}

// The DLL shows the leader screen here (CvDiplomacyRequests::Send, CvDiplomacyAI::DoFromUIDiploEvent replies). No screen: queued for RtDiplo::Process,
// which RtSession::Pump runs after Update (never inside this callback).
void RtEngine::GameplayDiplomacyAILeaderMessage(PlayerTypes ePlayer, DiploUIStateTypes eDiploUIState, const char* szLeaderMessage, LeaderheadAnimationTypes eAction, int iData1)
{
	// A replica's UI calling the DLL (outside the core's own work) must not queue what the server's auto-answer queue never saw.
	if (m_diplo && !IsReplicaUiCall()) m_diplo->Push(ePlayer, eDiploUIState, szLeaderMessage, eAction, iData1);
	// The message is the seat's own only (a replayed deal of another human is not news for this UI).
	if (m_uiRep && m_uiRep->Active() && GetActivePlayer() == m_uiRep->Seat())
	{
		RtUiEv ev(GC_UI_EV_DIPLO_AI_LEADER_MESSAGE);
		ev.A((int)ePlayer).A((int)eDiploUIState).A((int)eAction).A(iData1).T(szLeaderMessage);
		m_uiRep->Send(ev);
	}
}

// Network: exe 0x36ef20: all humans sent NetTurnComplete && HasReceivedTurnAllCompleteFromAllPlayers && barrier turn == game turn
// (HstLoopback.cpp CanAdvanceTurn, turn_model exe).
bool RtEngine::CanAdvanceTurn()
{
	if (!m_network) return true;
	return AllHumansTurnComplete() && HasReceivedTurnAllCompleteFromAllPlayers() && m_barrierTurn == TurnNow();
}

const char* RtEngine::GetCacheFolderPath() { return EngineUtility::GetCacheFolderPath(); }

}  // namespace lekmod_portable
