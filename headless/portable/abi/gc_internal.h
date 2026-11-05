// In-archive access to a gc_instance for the scenario harness (runtime/rt_selftest_*.cpp). Not part of gamecore_abi.h: hosts never use it.
#pragma once

#include "gamecore_abi.h"

namespace lekmod_portable {

class RtSession;
class RtAutoplay;

// The instance's runtime session and test bot (rt_autoplay, active with the setup's autoplay_human); NULL for a NULL instance.
RtSession* InstanceSession(gc_instance* inst);
const RtAutoplay* InstanceAutoplay(gc_instance* inst);
// The test bot of one human slot of a network game (N3.5: setup "autoplay_humans"); NULL for a bad slot or when the instance has no bots.
const RtAutoplay* InstanceBot(gc_instance* inst, int player);
// B4 M2 WO-1: the instance's GC_ROLE_*.
int32_t InstanceRole(gc_instance* inst);

}  // namespace lekmod_portable
