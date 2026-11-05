// rt_active: the active-player rule of spike S1 (package P3). While the brain runs an action of human P, the active
// (local) player of the gamecore is P; otherwise it is the server seat (an SS_OBSERVER slot, decision 3.3). Only the pregame field is
// switched (CvPreGame::setActivePlayer, a plain field: CvPreGame.cpp:2436-2439), never CvGame::setActivePlayer (UI events, hotseat logic).
#pragma once

#include "CvGameCoreDLLPCH.h"

namespace lekmod_portable {

// The server seat of a network game (NO_PLAYER in a single-player game). rt_setup sets it; ActiveAs restores to whatever was active.
PlayerTypes ServerSeat();
void SetServerSeat(PlayerTypes seat);

// RAII: the active player is `player` for the object's life; the previous one comes back on destruction (also on an exception).
class ActiveAs
{
public:
	explicit ActiveAs(PlayerTypes player);
	// `apply` false: nothing is switched (a conditional scope without a heap object).
	ActiveAs(PlayerTypes player, bool apply);
	~ActiveAs();

private:
	ActiveAs(const ActiveAs&);
	ActiveAs& operator=(const ActiveAs&);
	PlayerTypes m_previous;
	bool m_changed;
};

// B4 M2 WO-1: a replica's UI reads the game as its seat (Game.GetActivePlayer() is the seat), so between two gc_* calls the active player of a replica is its seat
// (SetRestPlayer; NO_PLAYER = the rule above, every other role). Every guarded entry runs as the server seat, as the server does, and the guard puts the seat back.
PlayerTypes RestPlayer();
void SetRestPlayer(PlayerTypes seat);

// A human a command or query may be made for: a living major civ whose slot is human and that is not the server seat (an SS_TAKEN slot of a
// network game; the one human of a single-player game). Kicked slots are SS_COMPUTER, so they are not humans any more.
bool IsSeatedHuman(int player);

// after every gc_* entry the active player is the server seat again. The guard is made at the top of an entry; its
// destructor checks the rule (only when `armed`, i.e. a game is running in a network game), logs an error and repairs the field when broken,
// so a leak shows up as a log error in every scenario instead of as a wrong game later.
class ActiveRuleGuard
{
public:
	ActiveRuleGuard(const char* entry, bool armed);
	~ActiveRuleGuard();

private:
	ActiveRuleGuard(const ActiveRuleGuard&);
	ActiveRuleGuard& operator=(const ActiveRuleGuard&);
	const char* m_entry;
	bool m_armed;
};

}  // namespace lekmod_portable
