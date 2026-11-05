// See rt_slots.h.
#include "CvGameCoreDLLPCH.h"

#include "rt_active.h"
#include "rt_session.h"
#include "rt_slots.h"
#include "CvPreGame.h"
#include "lekmod_portable/host_log.h"

namespace lekmod_portable {

namespace {

int32_t Refuse(std::string& msg, int32_t* reason, const char* text, int32_t why = GC_REASON_SLOT_STATE)
{
	msg = text;
	if (reason) *reason = why;
	return GC_ERR_REJECTED;
}

}  // namespace

// table of operations. Ported from the lobby's own paths (CvPlayer::disconnected / reconnected), which the exe calls from its net code when a
// connection drops, a Pitboss host kicks, or a player joins a slot; here the host (gc_slot_control) decides and the brain only carries out the core's side.
int ControlSlot(RtSession& s, int player, int op, const char* nick, RtSlotChange& out, std::string& msg, int32_t* reason)
{
	if (reason) *reason = GC_REASON_NONE;
	out = RtSlotChange();
	RtEngine& e = s.Engine();
	if (!e.NetworkGame()) return Refuse(msg, reason, "gc_slot_control: only the network game type has slots to change");
	if (player < 0 || player >= MAX_MAJOR_CIVS) return Refuse(msg, reason, "gc_slot_control: not a major civilization slot");
	const PlayerTypes who = (PlayerTypes)player;
	CvPlayer& p = GET_PLAYER(who);
	const SlotStatus before = CvPreGame::slotStatus(who);
	out.slotBefore = (int)before;
	if (who == ServerSeat() || before == SS_OBSERVER) return Refuse(msg, reason, "gc_slot_control: the server seat is not a slot of a player");
	if (!p.isAlive() || p.isMinorCiv() || p.isBarbarian()) return Refuse(msg, reason, "gc_slot_control: the slot is dead or not a major civilization");
	if (before != SS_TAKEN && before != SS_COMPUTER) return Refuse(msg, reason, "gc_slot_control: the slot is closed");

	// 4.8: everything under the server seat, so that the player is not the local one (the core's disconnected / reconnected need it).
	ActiveAs as(ServerSeat());

	switch (op)
	{
	case GC_SLOT_DISCONNECT:
		if (before != SS_TAKEN) return Refuse(msg, reason, "gc_slot_control DISCONNECT: the slot is not a human's");
		// The connection is lost: the slot stays human and the turn waits for it (Pitboss without a kick, CvPlayer.cpp:31255-31273: Lekmod's autopause flag
		// when the turn is active). Nothing ends the turn for it (N-D7).
		e.SetSlotConnected(who, false);
		p.disconnected();
		break;

	case GC_SLOT_KICK:
		if (before != SS_TAKEN) return Refuse(msg, reason, "gc_slot_control KICK: the slot is not a human's");
		// The room host kicked the player: CvPlayer::disconnected sees IsPlayerKicked and gives the slot to the AI (SS_COMPUTER, VerifyHandicap, the AI's handicap;
		// CvPlayer.cpp:31127-31161); the AI plays the rest of the turn (updateMoves treats the turn-active non-human as an AI) and holds no end of turn.
		e.SetSlotKicked(who, true);
		e.SetSlotConnected(who, false);
		p.disconnected();
		if (CvPreGame::slotStatus(who) != SS_COMPUTER)
		{
			Log(LOG_ERROR, "slots: KICK of player %d: the core left the slot human (slot status %d)", player, (int)CvPreGame::slotStatus(who));
			e.SetSlotKicked(who, false);
			return Refuse(msg, reason, "gc_slot_control KICK: the core did not hand the slot to the AI");
		}
		break;

	case GC_SLOT_CONNECT:
		if (before == SS_TAKEN)
		{
			// The human comes back to its slot: on line again, the autopause flag goes (CvPlayer::reconnected); the nick may change.
			if (nick && *nick) CvPreGame::setNickname(who, CvString(nick));
			const bool wasOn = e.SlotConnected(who);
			// A human that is off line only because of the load keeps the saved state (it was on line when saved): no reconnected(), which would add PLAYER_CONNECTING
			// notifications to the server seat and make the loaded game differ from the saved one. A real reconnect (after a DISCONNECT) runs it.
			const bool byLoad = e.SlotOfflineByLoad(who);
			e.SetSlotConnected(who, true);
			if (!wasOn && !byLoad) p.reconnected();
			out.returned = true;
			break;
		}
		else
		{
			// A new human takes a living AI major's slot (a former human's or an original AI's): the nick, SS_TAKEN and the handicap of the humans of this game; no kick,
			// on line. The slot's turn is not active when this is called at a safe point (the AI civs have moved): the end of its turn is marked so the turn does not hang;
			// the human plays from the next turn (SIMP-056).
			if (!nick || !*nick)
			{
				msg = "gc_slot_control CONNECT onto an AI slot needs a nick";
				return GC_ERR_INVALID_ARG;
			}
			CvPreGame::setNickname(who, CvString(nick));
			e.SetSlotKicked(who, false);
			e.SetSlotConnected(who, true);
			p.reconnected();  // SS_TAKEN and VerifyHandicap (the AI's handicap -> the last human one or the multiplayer one)
			const int human = GC.getInfoTypeForString(s.Setup().handicap.c_str(), true);
			if (human >= 0) CvPreGame::setHandicap(who, (HandicapTypes)human);
			out.tookAiSlot = true;
			// SIMPLIFIED(SIMP-056): the slot is taken over for the next turn, not the one that runs; original: not known (re-world RW-26 decides whether the exe activates the
			// slot at once).
			if (!p.isTurnActive() && s.HumanPhase())
			{
				e.MarkTurnEnded(who);
				out.turnMarkedEnded = true;
			}
		}
		break;

	default:
		msg = "gc_slot_control: unknown op";
		return GC_ERR_INVALID_ARG;
	}

	out.slotAfter = (int)CvPreGame::slotStatus(who);
	out.connected = e.SlotConnected(who);
	out.kicked = e.SlotKicked(who);
	s.Events().Push(GC_EVENT_SLOT_CHANGED, 4, player, out.slotAfter, out.connected ? 1 : 0, out.kicked ? 1 : 0, 0, 0, CvPreGame::nickname(who).c_str());
	Log(LOG_INFO, "slots: player %d op %d: slot %d -> %d, connected %d, kicked %d, nick '%s'%s", player, op, out.slotBefore, out.slotAfter, out.connected ? 1 : 0, out.kicked ? 1 : 0,
		CvPreGame::nickname(who).c_str(), out.turnMarkedEnded ? ", end of turn marked" : "");
	return GC_OK;
}

}  // namespace lekmod_portable
