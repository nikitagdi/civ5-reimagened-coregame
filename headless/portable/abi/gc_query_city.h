// G1a queries: Q_CITIES, Q_TOP_BAR, Q_CITY_BUILDS, also used by the state digest (gc_digest.cpp).
// Read-only over the gamecore objects; one buffer per call, owned by the caller (gc_buf release frees it). C++03.
#pragma once

#include <stdint.h>
#include <string>

#include "gamecore_abi.h"

namespace lekmod_portable {

// Answers GC_Q_CITIES, GC_Q_TOP_BAR or GC_Q_CITY_BUILDS into out. GC_OK or GC_ERR_INVALID_ARG (bad argc / range, a player that is not the
// human, a city that does not exist, any other kind); on error msg says why and out is untouched.
int32_t RunCityQuery(int32_t kind, const int32_t* args, uint32_t argc, gc_buf* out, std::string& msg);

// The Q_TOP_BAR record of any living player (the query itself only serves the human; the digest needs the viewer team's player).
int32_t BuildTopBar(int player, gc_buf* out, std::string& msg);

}  // namespace lekmod_portable
