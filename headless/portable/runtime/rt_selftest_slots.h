/* Scenario entry points of the b2/slots packet (N3.10 `peace`, N3.12 `mpslots`, N3.13 `mpsave`) that the C harness
 * (headless/tools/clang/runtime/harness.c) calls inside the gamecore archive. Kept apart from rt_selftest.h so that the packets that edit that header merge cleanly. */
#ifndef LEKMOD_RT_SELFTEST_SLOTS_H
#define LEKMOD_RT_SELFTEST_SLOTS_H

#include <stddef.h>

#include "gamecore_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Scenario `peace` (N3.10): a network game with `humans` humans played by the scenario (END_TURN_TIMEOUT ends their turns), mode "simultaneous" or "hybrid":
 * Q_DIPLO, DECLARE_WAR of human 0 on human 1, MAKE_PEACE refused (no war, a locked war, bad arguments) and accepted, the treaty's game length (rt_selftest_peace.cpp).
 * 0 = all checks passed. Creates the one gc_instance of the process. */
int rt_selftest_peace(const gc_host* host, unsigned seed, unsigned turns, unsigned humans, unsigned ai, const char* mode, const char* world_size, char* report, size_t report_len);

/* Scenario `mpslots` (N3.12): a network game with `humans` test-bot humans and `ai` AI; at safe points DISCONNECT (the turn waits) and CONNECT of one human, DISCONNECT and
 * KICK of another (the AI plays the slot), CONNECT with a new nick onto the kicked human's slot and onto an original AI slot; refusals; "mpslots-digest|turn|hex" lines for a comparison of two
 * runs (rt_selftest_mpslots.cpp). 0 = all checks passed. Creates the one gc_instance of the process. */
int rt_selftest_mpslots(const gc_host* host, unsigned seed, unsigned turns, unsigned humans, unsigned ai, const char* mode, const char* world_size, char* report, size_t report_len);

/* Scenario `mpsave` (N3.13), one phase per process: phase "save" plays a network game with `humans` test-bot humans and `ai` AI (a kick and a claim before the save), saves at a safe point
 * at game turn >= save_at into save_file, writes save_file ".digests" and plays `cont` more turns; phase "load" loads it in a fresh process, checks the digests and the Q_GAME / Q_TURN picture, does the
 * host's CONNECT for every human, plays `cont` turns and compares the per-turn digests (strict: a difference fails). "mpsave-turn|phase|turn|digests" lines (rt_selftest_mpsave.cpp). 0 = all checks passed. */
int rt_selftest_mpsave(const gc_host* host, const char* phase, const char* save_file, unsigned seed, unsigned humans, unsigned ai, unsigned save_at, unsigned cont, unsigned strict, const char* world_size,
	char* report, size_t report_len);

/* Scenario `replicaui` (B4 M2 WO-1): one instance plays a network game as the server (`turns` turns, 2 test-bot humans and `ai` AI), then as a replica of seat 0 with its own Lua state as the UI:
 * gc_lua_attach_ui, gc_ui_setup_graphical (one "created" per living unit and city), the journal batch by batch with Lua getters in between (the digest equals the server's after every batch), gc_ui_op,
 * gc_resync + gc_lua_invalidate_ui (rt_selftest_replica_ui.cpp). 0 = all checks passed. Creates the one gc_instance of the process. */
int rt_selftest_replica_ui(const gc_host* host, unsigned seed, unsigned turns, unsigned ai, const char* world_size, char* report, size_t report_len);

/* Scenario `diplo` (B4 fix-deal-crash): a network game with `humans` humans (default 1) and `ai` AI, mode "hybrid" (default) or "simultaneous": the trade and discussion screens' messages of
 * human 0 to an AI through gc_net_submit on the server: discussion events, embassy / open borders / research agreement deals, a demand, declare war, negotiate peace refused and accepted
 * (DoTradeScreenOpened on the server, the crash of game 925), the peace deal, a turn after it (rt_selftest_diplo.cpp). 0 = all checks passed. Creates the one gc_instance of the process. */
int rt_selftest_diplo(const gc_host* host, unsigned seed, unsigned turns, unsigned humans, unsigned ai, const char* mode, const char* world_size, char* report, size_t report_len);

#ifdef __cplusplus
}
#endif

#endif
