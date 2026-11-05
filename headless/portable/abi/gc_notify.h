// Q_NOTIFICATIONS (task F7): the live notifications of one player as the runtime captured them (RtUserInterface).
#pragma once

#include <stdint.h>
#include <string>

#include "gamecore_abi.h"

namespace lekmod_portable {

// args = {player}. GC_OK, or GC_ERR_INVALID_ARG (argc != 1 or player outside 0..MAX_PLAYERS-1) with msg set.
int32_t QueryNotifications(const int32_t* args, uint32_t argc, gc_buf* out, std::string& msg);

}  // namespace lekmod_portable
