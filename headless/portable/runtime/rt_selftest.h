/* Scenario entry points the C harness (headless/tools/clang/runtime/harness.c) calls inside the gamecore archive. */
#ifndef LEKMOD_RT_SELFTEST_H
#define LEKMOD_RT_SELFTEST_H

#include <stddef.h>

#include "gamecore_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Scenario `db`: loads <game>/db/gameplay.db and <game>/db/loc-en_us.db through host->read_file into rt_db,
 * counts tables, looks up localized text, and round-trips the saved-game DB through rt_fs the way
 * CvGame::reset / Write / Read do. Writes a JSON object into report (NUL-terminated); returns 0 when all checks pass. */
int rt_selftest_db(const gc_host* host, char* report, size_t report_len);

/* Scenario `load` (M1): rt_session boot (D1-D2, B1-B4, I1-I8: ctx->Init, Flush / PostProcessing / CacheGameDatabaseData)
 * on <game>/db/gameplay.db + db/loc-en_us.db, then the cached info counts of key tables against the DB and civhost.
 * Writes a JSON object into report; returns 0 when all checks pass. Boots the gamecore singleton: once per process. */
int rt_selftest_load(const gc_host* host, char* report, size_t report_len);

/* Scenario `lua`: rt_lua (exe-Lua, ICvEngineScriptSystem1) runs lua_dir/test_main.lua; include() looks up lua_dir
 * recursively (rt_vfs, task A6, replaces that lookup). Writes a JSON object into report; returns 0 when all checks pass. */
int rt_selftest_lua(const char* lua_dir, char* report, size_t report_len);

/* Scenario `vfs`: rt_vfs (RtVfs + RtJson). Always runs the in-memory fixture (base-name conflicts between roots, files before
 * subfolders, non-recursive first root, known_missing, BOM, include() through Lua); when host->read_file serves vfs-index.json
 * (--game DIR) it also loads that export, resolves includes and the map script and compiles every listed Lua file.
 * Writes a JSON object into report; returns 0 when all checks pass. */
int rt_selftest_vfs(const gc_host* host, char* report, size_t report_len);

/* Scenario `gameinfo` (A5, Lua half of M1): rt_session boot on <game>/db, rt_lua with GameInfo / DB.Query over the game DB,
 * then lua_dir/gameinfo/gameinfo_probe.lua (50 reference queries + semantics) compared with lua_dir/gameinfo/
 * civhost_reference.txt. Writes a JSON object into report; returns 0 when all checks pass. Boots the gamecore: once per process. */
int rt_selftest_gameinfo(const gc_host* host, const char* lua_dir, char* report, size_t report_len);

/* Scenario `setup`: rt_setup. Parses a JSON with every key of ABI 4.2, unknown / mistyped keys, and (with host->read_file, --game DIR)
 * RtSession::Load, deterministic AI / city-state choice from the seed, G1-G9 applied and the PreGame values read back. The game is not
 * started. Writes a JSON object into report; returns 0 when all checks pass. Boots the gamecore singleton: once per process. */
int rt_selftest_setup(const gc_host* host, char* report, size_t report_len);

/* Scenario `newgame` (A8, milestone M2): RtSession::Load + NewGame({"seed":seed}): pregame, Init2 with the Lekmap Pangaea map script,
 * LaterInit, the 37 gameplay contexts, DoGameStarted, Events.SequenceGameInitComplete; 0 Lua errors; the map digest (RtSession::MapDigest)
 * also recomputed through lua_dir/newgame/map_probe.lua. Prints the canonical map text as "newgame-map|" lines. Writes a JSON object
 * (with "map_digest") into report; returns 0 when all checks pass. Boots the gamecore singleton: once per process. */
int rt_selftest_newgame(const gc_host* host, const char* lua_dir, unsigned seed, char* report, size_t report_len);

/* Scenario `observer` (A9, milestone M3 / measurement M6): RtSession::Load + NewGame({"seed":seed,"observer":1,"ai_count":ai}) (all AI, an
 * SS_OBSERVER slot is the active player), then `turns` turns through RtSession::Pump with the loopback. 0 Lua errors, no STUCK; per-turn wall
 * time (median, max), Load / Init / Init2 times; "observer-digest|turn|hex" lines (RtSession::StateDigest) for cross-process comparison; the
 * final digest is "state_digest" in the report. Boots the gamecore singleton: once per process. */
int rt_selftest_observer(const gc_host* host, unsigned seed, unsigned turns, unsigned ai, char* report, size_t report_len);

/* Scenario `dirtybench` (N1.3): like `observer` on WORLDSIZE_HUGE (use --ai 7: 8 civilizations), plus after every turn
 * (outside the turn timing) per team: (a) a full Q_PLOTS, (b) a snapshot of every gcq_plot record plus basis fields compared with the previous
 * turn's, (c) the changed-plot count, (d) the AI turn time. "dirtybench-table|..." lines are p50 / p95 / max; "dirtybench-decision|..." applies the
 * rule b_total > 3 ms -> conditional task D5. Boots the gamecore singleton: once per process. */
int rt_selftest_dirtybench(const gc_host* host, unsigned seed, unsigned turns, unsigned ai, char* report, size_t report_len);

/* Scenario `humanturn` (F1, milestone M4a): RtSession::Load + NewGame({"seed":seed,"ai_count":ai}) (the human in slot 0), then `turns`
 * human turns: Pump to WAITING_HUMAN, "end turn" while blocked is REJECTED_BLOCKED with the reported blocker, the scenario resolves the
 * blockers with the human's commands (research, production, unit orders; FORCE_END_TURN for a UNITS blocker on odd turns), "end turn"
 * without a blocker is ACCEPTED and the next WAITING_HUMAN is at turn + 1. "humanturn-block|..." lines are the observed blocker table.
 * game_type "single" (default) or "network" (N3.4: one human in slot 0 of a network game with the server seat as active player; every action of the
 * human runs as that human, ActiveAs; plus the GC_REASON_* checks of HumanMayAct and CANCEL_END_TURN).
 * Boots the gamecore singleton: once per process. */
int rt_selftest_humanturn(const gc_host* host, unsigned seed, unsigned turns, unsigned ai, const char* game_type, char* report, size_t report_len);

/* Scenario `autochoice` (SIMP-002): NewGame({"seed":seed,"ai_count":ai,"auto_choices":{"research":0}}); the first research
 * blocker must stop the turn (research auto off), then research auto is switched on and clears it; `turns` human turns in all must never
 * wait on RESEARCH / FREE_TECH / POLICY / FREE_POLICY / CHOOSE_IDEOLOGY; then the human's team is set to ERA_MODERN and the ideology must be
 * chosen automatically within 5 turns. "autochoice-event|..." lines are the AUTO_CHOICE events. Boots the gamecore singleton: once per process. */
int rt_selftest_autochoice(const gc_host* host, unsigned seed, unsigned turns, unsigned ai, char* report, size_t report_len);

/* Scenario `autochoice2` (F4): rt_autochoice part 2 (promotion, free / faith / Maya great person, pantheon, religion, enhancement, reformation,
 * captured city). NewGame with religion / promotion / great_person / captured_city auto-choices off; for each kind the state is set up by
 * scenario-only gamecore calls, the flag-off pump must stop on the blocker (captured city: the city stays undecided), then the flag is switched
 * on and the next pump must choose with an AUTO_CHOICE event. `turns` is the turn budget. "autochoice2-event|..." lines are the events.
 * Boots the gamecore singleton: once per process. */
int rt_selftest_autochoice2(const gc_host* host, unsigned seed, unsigned turns, unsigned ai, char* report, size_t report_len);

/* Scenarios `autochoice` (part 1) and `autochoice2` (part 2) in the network game type (N3.14; rt_selftest_acnet.cpp): `humans` test-bot humans and `ai` AI in a Pitboss
 * game; rt_autochoice for each human (flag off: every human waits on his own blocker; flag on: one event of his own; the states per human), the AI's requests to the humans, the attempt
 * counters per human. 0 = all passed. Boots the gamecore singleton: once per process. */
int rt_selftest_autochoice_net(const gc_host* host, unsigned seed, unsigned turns, unsigned humans, unsigned ai, int part, char* report, size_t report_len);

/* Scenario `autoplay` (F8, milestone M4): one game through the C ABI only (gc_create, gc_new_game with "autoplay_human": 1, gc_pump), the
 * test bot rt_autoplay plays the human through gc_apply_command for `turns` turns. 0 STUCK / GAME_OVER / gc_pump errors, 0 Lua and log
 * errors, every AI diplomacy message answered; the report counts the diplomacy requests (the matrix in run.sh needs at least one) and holds
 * per-turn wall time and gc_state_digest(-1) ("autoplay-turn|turn|ms|digest" lines). Creates the one gc_instance of the process. */
int rt_selftest_autoplay(const gc_host* host, unsigned seed, unsigned turns, unsigned ai, char* report, size_t report_len);

/* Scenario `saveload` (G4a, milestone M5), one phase per process (run.sh runs both):
 *  - phase "save": new game with the test bot ("autoplay_human": 1), gc_pump to game turn save_at, gc_save at the first save point there
 *    (writes save_file and save_file".digests": gc_state_digest of -1 and every living major team right after the save, then -1 and the
 *    human's team after every turn change for `cont` more turns); checks: gc_save refused away from a save point, the save does not change
 *    the digests, two saves give the same bytes, gc_load_game on this instance is ALREADY_EXISTS;
 *  - phase "load": on a fresh instance, broken saves (garbage, truncated, one flipped byte, another format version, another build id) are
 *    GC_ERR_BAD_DATA and leave the slot free; then gc_load_game of save_file, every viewer's digest = the saved one, a second load / new game is
 *    ALREADY_EXISTS, and `cont` turns without STUCK; their digests are compared with the save phase's turn by turn (a check with strict != 0,
 *    otherwise reported only). Both: 0 STUCK, 0 Lua and log errors. */
int rt_selftest_saveload(const gc_host* host, const char* phase, const char* save_file, unsigned seed, unsigned ai, unsigned save_at, unsigned cont,
	unsigned strict, char* report, size_t report_len);

/* Scenario `spike-net` (spike S1): RtSession::Load + NewGame({"game_type":"network","humans":humans,"ai_count":ai,"turn_mode":mode})
 * (Pitboss, the server seat as active player), `turns` turns through RtSession::Pump with the exe turn model; the test bot plays every waiting human
 * under ActiveAs. mode "hybrid": human 0 declares war on human 1 at game turn 5 and the two must not be turn-active together afterwards. The report
 * holds the S1 measurements (exe calls, dropped messages, diplomacy, automated workers, AI turn time). Boots the gamecore singleton: once per process. */
int rt_selftest_spikenet(const gc_host* host, unsigned seed, unsigned turns, unsigned humans, unsigned ai, const char* mode, const char* world_size, char* report, size_t report_len);

/* Scenarios `mp` / `mphybrid` (task N3.5): a network game with `humans` test-bot humans (setup "autoplay_humans") and `ai` AI through the C ABI only
 * (gc_create, gc_new_game, gc_pump, gc_apply_command, gc_query), `turns` turns. mode "simultaneous" (`mp`): the humans end every turn, humans_left follows, END_TURN then
 * CANCEL_END_TURN, and at the end the game is over when every human is dead (GC_PUMP_GAME_OVER). mode "hybrid" (`mphybrid`): human 0 declares war on human 1 at turn 5, the two
 * are never turn-active together, a third human moves with one of them, NOT_YOUR_TURN for the one waiting, peace brings the simultaneous turn back. "mp-digest|turn|hex" lines
 * (RtSession::StateDigest) let two runs of one seed be compared turn by turn. Creates the one gc_instance of the process. */
int rt_selftest_mp(const gc_host* host, unsigned seed, unsigned turns, unsigned humans, unsigned ai, const char* mode, const char* world_size, char* report, size_t report_len);
/* Scenario timer (N3.6): a network game (simultaneous, TURNTIMER_MEDIUM) with --humans N humans played by the scenario through the C ABI: Q_GAME v2 / Q_TURN,
 * END_TURN refused with a blocker, END_TURN_TIMEOUT, CANCEL_END_TURN, the safe point, events 2 / 12 / 14 (rt_selftest_timer.cpp). 0 = all checks passed. */
int rt_selftest_timer(const gc_host* host, unsigned seed, unsigned turns, unsigned humans, unsigned ai, const char* world_size, char* report, size_t report_len);

/* Scenario `dirty` (task N3.8): the `mp` game (a network game, `humans` test-bot humans, `ai` AI, simultaneous turns) through the C ABI; after every gc_pump call
 * Q_PLOTS_DIRTY, then the full Q_PLOTS of every human's team and the full Q_PLOT_BASIS are compared with the previous step's: no changed plot may be missing from the dirty set. Also the
 * first call, the reset by the call, the index forms, the basis against Q_PLOTS, and scenario-only edits of a plot (rt_selftest_dirty.cpp). The report holds the cost of the call. 0 = all passed. */
int rt_selftest_dirty(const gc_host* host, unsigned seed, unsigned turns, unsigned humans, unsigned ai, const char* world_size, char* report, size_t report_len);

/* Scenario `wbmapread` (package P14): RtSession::Load, then each of the `count` .Civ5Map files (game-data paths read through
 * host->read_file) through CvWorldBuilderMap::Load with the game's type tables; an independent parser of the same file must consume it exactly and agree
 * plot by plot (map and scenario data), player by player, city by city and unit by unit. A missing file must fail. Boots the gamecore singleton. */
int rt_selftest_wbmapread(const gc_host* host, const char* const* maps, int count, char* report, size_t report_len);

/* Scenario `wbmap` (P14): a network game on the WorldBuilder map `map` ("world_builder_map"; with scenario != 0 also "world_builder_scenario": the map's
 * players, 1 human played by the test bot), `turns` turns. The game map must match the file (terrain, features, resources, rivers, improvements, routes,
 * cities); the report ends with "state_digest" for a cross-process comparison. Boots the gamecore singleton: once per process. */
int rt_selftest_wbmap(const gc_host* host, const char* map, unsigned scenario, unsigned seed, unsigned turns, unsigned ai, char* report, size_t report_len);

/* Scenario `purity` (AC-N1.4, N3.15; rt_selftest_purity.cpp): a network game like `mp` with `humans` bots and `ai` AI for `turns` turns; noise 0 nothing, 1 = 10 000 random
 * queries Q_* 1..21 between the gc_pump calls, 2 = 5 000 random commands that must be refused; noise_kinds "5,6" (or "") limits the queries to those kinds (bisection). Prints "purity-digest|turn|hex" (the simulation digest after every turn): run.sh compares the runs
 * turn by turn. Boots the gamecore singleton: once per process. */
int rt_selftest_purity(const gc_host* host, unsigned seed, unsigned turns, unsigned humans, unsigned ai, int noise, const char* noise_kinds, const char* world_size, char* report, size_t report_len);

/* Scenario `s2` (S2, N3.16; rt_selftest_s2.cpp): counts per human what the core's phases lose with the server seat as the active player (audit F-01, F-02, F-03, F-11):
 * research left unchosen, turns re-opened for units, cities / ruin rewards, resource notifications, AI messages, messages and popups. game_type "network" with `humans` bots, or "single" (B1, one human)
 * as the reference. Boots the gamecore singleton: once per process. */
int rt_selftest_s2(const gc_host* host, unsigned seed, unsigned turns, unsigned humans, unsigned ai, const char* game_type, const char* world_size, char* report, size_t report_len);

/* Scenario `mp8` (S4, N3.17; rt_selftest_mp8.cpp; Linux): the speed of a network game with `humans` test-bot humans (default 8) and `ai` AI on world_size (default Huge) for `turns` turns:
 * the cost of one command (gc_apply_command + the step that carries it), what a server step needs on top (Q_PLOTS_DIRTY, Q_PLOTS of the dirty plots, Q_UNITS, Q_CITIES, Q_PLAYERS per team, Q_TOP_BAR, Q_NOTIFICATIONS
 * per human), the AI turn and the human phase per turn window. The numbers are printed ("mp8-..." lines) and go into the report; the checks fail only on errors. Creates the one gc_instance of the process. */
int rt_selftest_mp8(const gc_host* host, unsigned seed, unsigned turns, unsigned humans, unsigned ai, const char* world_size, char* report, size_t report_len);

/* Scenario `mapscripts` (N3.18; rt_selftest_mapscripts.cpp): one game on `map_script` (a path of vfs-index.json "map_scripts") and `world_size` (default Duel): a network game with a bot human and `ai` AI,
 * `turns` turns; passes without STUCK, Lua errors and logged errors. One game per process; run.sh loops over the scripts and sizes and writes map-scripts.json. 0 = the script passed. */
int rt_selftest_mapscripts(const gc_host* host, unsigned seed, unsigned turns, unsigned ai, const char* map_script, const char* world_size, char* report, size_t report_len);

/* Scenario `mpmsg` (N3.21, HYG-0013; rt_selftest_mpmsg.cpp): the core's message lines for the owner reach every human: a fight between two humans, a fight AI -> human and a technology line;
 * expects the lines for the passive human when the flag is compiled in and none when not; prints "mpmsg-digest|label|hex" for the comparison between the two builds. 0 = all checks passed. */
int rt_selftest_mpmsg(const gc_host* host, unsigned seed, unsigned turns, unsigned humans, unsigned ai, char* report, size_t report_len);

#ifdef __cplusplus
}
#endif

#endif

