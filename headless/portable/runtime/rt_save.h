// rt_save: save and load of a game as one GCS1 buffer ( rt_save; task G4a, milestone M5).
// Ported from civhost's checkpoint (headless/host/src/game/HstCheckpoint.h, control/HstSession.cpp SaveGameImpl / LoadGameImpl):
// the DLL's own serialization, one section per Write call, in the order the exe's loader reads (Resync_LoadGameFromStreamImpl
// 0x2b6230 -> 0x31bfe0), plus the runtime's own state. RtSession::SaveGame / CheckSave / LoadGame are defined in rt_save.cpp.
//
// SIMPLIFIED(SIMP-019): GCS1 is our own container; the exe writes .Civ5Save (its header with game version / DLC / mods, then one zlib stream).
// GCS1 (little-endian):
//   offset 0  "GCS1"
//          4  u32 format version (kGcsFormatVersion)
//          8  u32 fixed header size (32)
//         12  u32 number of sections
//         16  u64 body length (bytes after the fixed header)
//         24  u64 xxh64 (seed 0) of the body
//         32  body: per section u16 name length, name (ASCII), u32 size, bytes
// Sections, in this order:
//   "header"     key=value lines: format, build_id (must equal RtSession::BuildId() of the loading core), dll_version, turn, abi
//   "setup"      the setup JSON of gc_new_game (re-parsed on load: auto_choices, autoplay_human, ...; the pregame itself comes from "pregame")
//   "runtime"    host state of the runtime (RtSaveAccess in rt_save.cpp): virtual clock, loopback handshake flags and queue, war-state
//                memory, the network turn model (per-human NetTurnAllComplete turns, the all-AI-processed and barrier turns, un-ready flags, kicks),
//                UI flags (canEndTurn per human) and the notification registry, pending ruin pop-ups, diplomacy queue and counters, auto-choice flags
//                and per-turn attempts, Lua math.random / os.clock state, the turn's slice count, the turn whose human phase was announced.
//                The connection of a human to its slot is not saved: after a load nobody is on line until the host's CONNECT.
//   "abi"        the ABI layer's bytes (gc_abi.cpp: last pump status, the test bot of a single game, the test bots of the humans of a network game)
//   "pregame"    ICvPreGame1::write
//   "game"       ICvGame1::Write
//   "map"        ICvMap1::Write
//   "team00".."team63"     ICvTeam1::Write
//   "player00".."player63" ICvPlayer1::Write
//   "supporting" ICvGame1::WriteSupportingClassData
// Import of a .Civ5Save (golden scenes): sections "header", "setup", "pregame" (the CvPreGame::write block of the save) and
// "body" (its inflated zlib stream: game, map, teams, players, supporting data, then zero padding); no "runtime", "abi" or split DLL
// sections and no build id. The DLL Reads take the body in turn, as the exe's loader does (civhost --import-save); the runtime and the ABI
// layer start fresh. Only saves of the DLL's own mod version load. The exe writes GameMapTypes of writeArchive as 1 byte, this build's
// CvEnumSerialization reads 4: the host widens that field of the pregame before the import (golden `world.rs`).
// Build id: "lekmod-portable/<LEKMOD_PORTABLE_BUILD_TAG>/dll <GetDLLVersion>/guid <GetDLLGUID>/data <xxh64 of db/gameplay.db>". The tag
// defaults to "dev"; a release build can pass -DLEKMOD_PORTABLE_BUILD_TAG=\"...\" to make saves of other builds incompatible.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "rt_bytes.h"

namespace lekmod_portable {

class RtAutoplay;

static const uint32_t kGcsFormatVersion = 2;  // 2 (N3.13): the runtime section carries the network handshake, kicks and per-human flags; the abi section the test bots of every human
static const uint32_t kGcsFixedHeader = 32;

// abi/gc_digest.cpp.
uint64_t Xxh64(const void* data, size_t len, uint64_t seed);

class RtSession;
// The "runtime" section a save would carry now (B4 gc-replica: part of the replica digest).
void WriteRuntimeState(const RtSession& s, std::vector<uint8_t>& out);

// The test bot's state (rt_autoplay), for the ABI layer's "abi" section.
void WriteAutoplayState(const RtAutoplay& bot, RtByteWriter& w);
bool ReadAutoplayState(RtAutoplay& bot, RtByteReader& r);

}  // namespace lekmod_portable
