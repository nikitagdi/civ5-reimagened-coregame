// rt_slots: who plays a slot (package P8, task N3.12): connect, disconnect, kick, and the takeover of an AI slot by a new human, in the
// running game of a network game. Done through the core's own paths CvPlayer::disconnected / reconnected (CvPlayer.cpp:31092-31337) under the server seat (so the
// player is not the local one, CvPlayer.cpp:31095, :31290), with the exe's connection state (RtEngine::SlotConnected / SlotKicked) answering IsPlayerConnected /
// IsPlayerKicked to them. The safe-point rule (SIMP-026) is the ABI layer's (gc_slot_control, gc_abi.cpp), which knows the pump and the command history.
#pragma once

#include <stdint.h>
#include <string>

#include "gamecore_abi.h"

namespace lekmod_portable {

class RtSession;

// What a slot operation did, for the ABI layer (test bots follow the slot) and for the event.
struct RtSlotChange
{
	int slotBefore, slotAfter;  // SlotStatus of the core
	bool connected;             // on line afterwards
	bool kicked;                // the kick flag afterwards
	bool tookAiSlot;            // CONNECT onto an AI slot: a new human sits in it now
	bool returned;              // CONNECT of the human the slot already had
	bool turnMarkedEnded;       // the taken slot's turn was not active (its AI had moved): its end of turn was marked so the turn does not wait for it
	RtSlotChange() : slotBefore(-1), slotAfter(-1), connected(false), kicked(false), tookAiSlot(false), returned(false), turnMarkedEnded(false) {}
};

// GC_SLOT_CONNECT / DISCONNECT / KICK (gamecore_abi.h) for a major slot. GC_OK; GC_ERR_REJECTED with GC_REASON_SLOT_STATE (the operation does not fit the slot: not a
// living major civ, the server seat, ...); GC_ERR_INVALID_ARG (CONNECT onto an AI slot without a nick). msg says why; *reason gets the GC_REASON_*. Pushes GC_EVENT_SLOT_CHANGED
// on success. The caller has checked the safe point.
int ControlSlot(RtSession& session, int player, int op, const char* nick, RtSlotChange& out, std::string& msg, int32_t* reason);

}  // namespace lekmod_portable
