// Q_DIPLO (task N3.10): war and peace of one player with every other living major civ, for the buttons "declare war" / "make peace".
#pragma once

#include <stdint.h>
#include <string>

#include "gamecore_abi.h"

namespace lekmod_portable {

// args {player}: a living major civ that is not the server seat. One gcq_diplo per other living major civ, by id, not the seat. Reads the core, changes nothing.
int32_t QueryDiplo(const int32_t* args, uint32_t argc, gc_buf* out, std::string& msg);

}  // namespace lekmod_portable
