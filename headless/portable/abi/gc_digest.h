// gc_state_digest: the viewer-filtered digest, xxh64 over a canonical serialization of Q_GAME, Q_PLOTS, Q_UNITS,
// Q_CITIES and Q_TOP_BAR as the viewer sees them. C++03.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

#include "../runtime/rt_session.h"
#include "gamecore_abi.h"

namespace lekmod_portable {

// XXH64 (Yann Collet's xxHash, the published algorithm; BSD-2 reference behavior, written here from the specification, no third-party code).
uint64_t Xxh64(const void* data, size_t len, uint64_t seed);

// The digest for viewer_team (-1 = the whole truth). Canonical stream, hashed once with seed 0:
//   for each part in the order Q_GAME(1), Q_PLOTS(2, viewer), Q_UNITS(3, viewer), Q_CITIES(4, viewer), Q_TOP_BAR(6, bar player):
//     u8 query kind, u32 little-endian byte length of the GCQ1 reply, the reply bytes exactly as gc_query returns them
//     (header, records with zeroed padding, deduplicated string table in first-use order).
// bar player = the living human when viewer_team is -1 or the human's team, otherwise the first living major civ of the viewer's team; a
// viewer team without one contributes the Q_TOP_BAR part with length 0. Every part is a pure function of the game state: the same state
// gives the same digest on every target and on every call. lastPumpStatus / blocking / target are the inputs of Q_GAME (see RunGameQuery).
int32_t ViewerDigest(int32_t viewerTeam, int32_t lastPumpStatus, int32_t blocking, const RtSession::BlockTarget& target, uint64_t* out, std::string& msg);

// viewer_team -3 (B4 gc-replica): the replica digest. xxh64 over: the pregame's write (part 0x7C), the simulation part of
// viewer -2 (0x7E: one xxh64 per serialized game object) and the xxh64 of the runtime section a save would carry now plus one byte per slot on
// line (0x7D). It has no input of the ABI layer (no Q_GAME: the last pump status and the bots of a server differ on a replica), so a server
// and its replicas compare it. GC_REPLICA_DIGEST_DUMP=<file> writes the objects of each call there (u32 length + bytes), to find a divergence.
int32_t ReplicaDigest(const RtSession& session, uint64_t* out, std::string& msg);

}  // namespace lekmod_portable
