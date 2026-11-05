// GCQ1 query writer for gc_query: Q_GAME, Q_PLOTS, Q_PLAYERS, Q_CATALOG.
// Read-only over the gamecore objects; one buffer per call, owned by the caller (gc_buf release frees it).
#pragma once

#include <stdint.h>
#include <string>

#include "../runtime/rt_session.h"
#include "gamecore_abi.h"
#include "gc_query_dirty.h"

namespace lekmod_portable {

// Answers query kind (GC_Q_*) into out. Returns GC_OK, GC_ERR_INVALID_ARG (bad argc / range, unknown kind) or
// GC_ERR_NOT_IMPLEMENTED (known kind not built yet); on error msg says why and out is untouched.
// blocking / target: RtSession::CurrentBlocking of the human (the one place that knows what holds the turn).
// lastPumpStatus is the status of the last finished gc_pump (GC_PUMP_*), -1 before the first pump.
// extra (v2, task N3.6): what Q_GAME and Q_TURN need beyond the game objects, or NULL (the scenario bot's calls): the runtime session (humans, handshake, blockers per
// human) and the ABI layer's safe-point verdict (GC_UNSAFE_*; the layer knows the pump and command history).
struct GameQueryExtra
{
	RtSession* session;
	int32_t unsafeReason;
	PlotDirtyTracker* dirty;  // N3.8: the snapshot behind Q_PLOTS_DIRTY (the instance's); NULL = that query is not answered
	GameQueryExtra() : session(0), unsafeReason(0), dirty(0) {}
};
int32_t RunGameQuery(int32_t kind, const int32_t* args, uint32_t argc, int32_t lastPumpStatus, int32_t blocking, const RtSession::BlockTarget& target, gc_buf* out, std::string& msg,
	const GameQueryExtra* extra = 0);

}  // namespace lekmod_portable
