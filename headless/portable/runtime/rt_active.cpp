// See rt_active.h.
#include "CvGameCoreDLLPCH.h"

#include "rt_active.h"

#include "CvPreGame.h"
#include "lekmod_portable/host_log.h"

namespace lekmod_portable {

namespace {
PlayerTypes g_serverSeat = NO_PLAYER;
PlayerTypes g_restPlayer = NO_PLAYER;  // B4 M2 WO-1: a replica's seat (see rt_active.h)
}

PlayerTypes ServerSeat() { return g_serverSeat; }
void SetServerSeat(PlayerTypes seat) { g_serverSeat = seat; }
PlayerTypes RestPlayer() { return g_restPlayer; }
void SetRestPlayer(PlayerTypes seat)
{
	const PlayerTypes before = g_restPlayer;
	g_restPlayer = seat;
	// At rest now: the UI's player is the active one (every guarded entry switches to the server seat and back).
	if (seat != NO_PLAYER)
		CvPreGame::setActivePlayer(seat);
	else if (before != NO_PLAYER && g_serverSeat != NO_PLAYER)
		CvPreGame::setActivePlayer(g_serverSeat);
}

ActiveAs::ActiveAs(PlayerTypes player) : m_previous(CvPreGame::activePlayer()), m_changed(player != m_previous)
{
	if (m_changed) CvPreGame::setActivePlayer(player);
}

ActiveAs::ActiveAs(PlayerTypes player, bool apply) : m_previous(CvPreGame::activePlayer()), m_changed(apply && player != m_previous)
{
	if (m_changed) CvPreGame::setActivePlayer(player);
}

ActiveAs::~ActiveAs()
{
	if (m_changed) CvPreGame::setActivePlayer(m_previous);
}

bool IsSeatedHuman(int player)
{
	if (player < 0 || player >= MAX_MAJOR_CIVS) return false;
	const CvPlayer& p = GET_PLAYER((PlayerTypes)player);
	return p.isAlive() && p.isHuman() && !p.isObserver();
}

ActiveRuleGuard::ActiveRuleGuard(const char* entry, bool armed) : m_entry(entry), m_armed(armed)
{
	if (armed && g_restPlayer != NO_PLAYER && g_serverSeat != NO_PLAYER && CvPreGame::activePlayer() != g_serverSeat) CvPreGame::setActivePlayer(g_serverSeat);
}

ActiveRuleGuard::~ActiveRuleGuard()
{
	if (m_armed && g_restPlayer != NO_PLAYER)
	{
		CvPreGame::setActivePlayer(g_restPlayer);
		return;
	}
	if (!m_armed || g_serverSeat == NO_PLAYER) return;
	if (CvPreGame::activePlayer() == g_serverSeat) return;
	Log(LOG_ERROR, "active-player rule broken: gc_%s returned with active player %d, the server seat is %d (repaired)", m_entry, (int)CvPreGame::activePlayer(), (int)g_serverSeat);
	CvPreGame::setActivePlayer(g_serverSeat);
}

}  // namespace lekmod_portable
