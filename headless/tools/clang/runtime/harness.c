/* Scenario harness over gamecore_abi.h (C, no Lua): headless/tools/clang/runtime/run.sh.
 *   harness --scenario boot|db|load|lua|vfs|gameinfo|setup|newgame|observer|dirtybench|humanturn|autochoice|autochoice2|autoplay|saveload|spike-net|mp|mphybrid|timer|dirty|purity|s2|mp8|mapscripts|peace|mpslots|mpsave|mpmsg|replicaui|diplo|wbmapread|wbmap [--game DIR] [--lua-dir DIR] [--seed N] [--turns N] [--ai N] [--report FILE]
 *           [--humans N] [--mode simultaneous|hybrid] [--world-size WORLDSIZE_*] (spike-net) [--noise 0|1|2] (purity)
 *           [--maps DIR] [--map maps/REL ...] [--scenario-data 0|1] (wbmapread, wbmap: read_file of "maps/REL" comes from DIR/REL)
 * Files the gamecore asks for through read_file come from DIR (the game-data layout). On wasm DIR and the
 * report directory are WASI preopens. The report is JSON; "not_implemented" lists what belongs to later tasks. */
#if defined(__linux__)
#define _GNU_SOURCE /* backtrace, write, _exit under -std=c99 */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>
#endif

#include "gamecore_abi.h"
#include "rt_selftest.h"

static const char* g_game_dir = NULL;
static const char* g_lua_dir = NULL;
static const char* g_maps_dir = NULL; /* P14: game-data paths "maps/<rel>" are read from <g_maps_dir>/<rel> (the .Civ5Map files of the install) */
static int g_errors = 0;
static int g_events = 0;

static void host_log(void* user, int32_t level, const char* msg)
{
	static const char* names[] = {"error", "warn", "info", "debug", "trace"};
	(void)user;
	if (level == GC_LOG_ERROR) ++g_errors;
	if (level <= GC_LOG_INFO) printf("[%s] %s\n", (level >= 0 && level <= 4) ? names[level] : "?", msg);
}

static void release_malloc(gc_buf* b) { free(b->data); }

static int32_t host_read_file(void* user, const char* path, gc_buf* out)
{
	char full[1024];
	FILE* f;
	long n;
	uint8_t* data;
	(void)user;
	if (g_maps_dir && !strncmp(path, "maps/", 5)) {
		if (snprintf(full, sizeof(full), "%s/%s", g_maps_dir, path + 5) >= (int)sizeof(full)) return GC_ERR_INVALID_ARG;
	} else {
		if (!g_game_dir) return GC_ERR_NOT_FOUND;
		if (snprintf(full, sizeof(full), "%s/%s", g_game_dir, path) >= (int)sizeof(full)) return GC_ERR_INVALID_ARG;
	}
	f = fopen(full, "rb");
	if (!f) return GC_ERR_NOT_FOUND;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	data = (uint8_t*)malloc(n > 0 ? (size_t)n : 1);
	if (!data || (n > 0 && fread(data, 1, (size_t)n, f) != (size_t)n)) { free(data); fclose(f); return GC_ERR_INTERNAL; }
	fclose(f);
	out->data = data;
	out->len = (size_t)n;
	out->release = release_malloc;
	out->owner = NULL;
	return GC_OK;
}

static void host_event(void* user, const gc_event* ev)
{
	(void)user;
	(void)ev;
	++g_events;
}

/* Scenario boot: the gamecore_abi.h v1 lifecycle on the real data (needs --game DIR): create, new_game({"seed":N}), pump until the
 * human is up (WAITING_HUMAN) or the game turn changes, state digest, destroy. --turns is unused here (the turn loop has `observer`). */
static int scenario_boot(unsigned seed, const char* report)
{
	gc_host host = {NULL, host_log, host_read_file, host_event};
	gc_instance* inst;
	gc_error err;
	gc_pump_result pr;
	uint64_t digest = 0;
	char setup[64];
	int32_t rc_new = GC_ERR_NOT_STARTED, rc_pump = GC_OK, rc_digest = GC_ERR_NOT_STARTED;
	int start_turn = -1, calls = 0, total_slices = 0, ok;

	if (!g_game_dir) { printf("boot: --game DIR (game-data layout) is required\n"); return 2; }
	memset(&pr, 0, sizeof(pr));
	memset(&err, 0, sizeof(err));
	inst = gc_create(&host, &err);
	if (!inst) { printf("boot: gc_create failed: %d %s\n", err.code, err.message); return 1; }
	snprintf(setup, sizeof(setup), "{\"game_type\":\"single\",\"seed\":%u}", seed);
	rc_new = gc_new_game(inst, setup, &err);
	if (rc_new != GC_OK) printf("boot: gc_new_game failed: %d %s\n", err.code, err.message);
	if (rc_new == GC_OK) {
		rc_pump = gc_pump(inst, 1, &pr, &err);
		start_turn = pr.turn;
		while (rc_pump == GC_OK && pr.status == GC_PUMP_AI_RUNNING && pr.turn == start_turn && calls < 2000) {
			total_slices += pr.slices_used;
			rc_pump = gc_pump(inst, 50, &pr, &err);
			++calls;
		}
		total_slices += pr.slices_used;
		if (rc_pump != GC_OK) printf("boot: gc_pump failed: %d %s\n", err.code, err.message);
		rc_digest = gc_state_digest(inst, -1, &digest, &err);
	}
	printf("boot: new_game=%d pump=%d turn %d -> %d status=%d blocker=%d slices=%d digest=%016llx\n", rc_new, rc_pump, start_turn, pr.turn, pr.status,
		pr.blocker, total_slices, (unsigned long long)digest);
	gc_destroy(inst);

	ok = rc_new == GC_OK && rc_pump == GC_OK && rc_digest == GC_OK && (pr.status == GC_PUMP_WAITING_HUMAN || pr.turn != start_turn) && g_errors == 0;
	if (report) {
		FILE* out = fopen(report, "wb");
		if (!out) { printf("boot: cannot write report %s\n", report); return 1; }
		fprintf(out,
			"{\"scenario\":\"boot\",\"abi_version\":%u,\"seed\":%u,\"start_turn\":%d,\"turn\":%d,\"status\":%d,"
			"\"new_game\":%d,\"pump\":%d,\"digest\":\"%016llx\",\"events\":%d,\"log_errors\":%d,\"ok\":%s}\n",
			gc_abi_version(), seed, start_turn, pr.turn, pr.status, rc_new, rc_pump, (unsigned long long)digest, g_events, g_errors, ok ? "true" : "false");
		fclose(out);
	}
	printf("boot: %s\n", ok ? "OK" : "FAILED");
	return ok ? 0 : 1;
}

/* Scenario db: rt_db + rt_fs checks inside the gamecore archive (rt_selftest.cpp), needs --game DIR. */
static int scenario_db(const char* report)
{
	static char json[16384];
	gc_host host = {NULL, host_log, host_read_file, host_event};
	int rc, ok;
	if (!g_game_dir) { printf("db: --game DIR (game-data layout: db/gameplay.db, db/loc-en_us.db) is required\n"); return 2; }
	json[0] = '\0';
	rc = rt_selftest_db(&host, json, sizeof(json));
	ok = rc == 0 && g_errors == 0;
	if (report) {
		FILE* out = fopen(report, "wb");
		if (!out) { printf("db: cannot write report %s\n", report); return 1; }
		fprintf(out, "{\"scenario\":\"db\",\"ok\":%s,\"log_errors\":%d,\"result\":%s}\n", ok ? "true" : "false", g_errors, json[0] ? json : "null");
		fclose(out);
	}
	printf("db: %s\n", ok ? "OK" : "FAILED");
	return ok ? 0 : 1;
}

/* Scenario load (M1): rt_session boot I1-I8 on the real game DB, key-table counts vs civhost (rt_selftest_load.cpp). */
static int scenario_load(const char* report)
{
	static char json[65536];
	gc_host host = {NULL, host_log, host_read_file, host_event};
	int rc, ok;
	if (!g_game_dir) { printf("load: --game DIR (game-data layout: db/gameplay.db, db/loc-en_us.db) is required\n"); return 2; }
	json[0] = '\0';
	rc = rt_selftest_load(&host, json, sizeof(json));
	ok = rc == 0 && g_errors == 0;
	if (report) {
		FILE* out = fopen(report, "wb");
		if (!out) { printf("load: cannot write report %s\n", report); return 1; }
		fprintf(out, "{\"scenario\":\"load\",\"milestone\":\"M1\",\"ok\":%s,\"log_errors\":%d,\"result\":%s}\n", ok ? "true" : "false", g_errors,
			json[0] ? json : "null");
		fclose(out);
	}
	printf("load: %s (log errors: %d)\n", ok ? "OK" : "FAILED", g_errors);
	return ok ? 0 : 1;
}

#if defined(__linux__)
/* Native crash report: raw return addresses (harness(+0xOFFSET)); resolve with nm / addr2line on the binary. */
static void on_fatal_signal(int sig)
{
	void* frames[64];
	int n = backtrace(frames, 64);
	static const char head[] = "harness: fatal signal, backtrace:\n";
	(void)sig;
	if (write(2, head, sizeof(head) - 1) < 0) {}
	backtrace_symbols_fd(frames, n, 2);
	_exit(128 + sig);
}
#endif

/* Scenario lua: rt_lua + exe-Lua checks (rt_lua_selftest.cpp); needs --lua-dir (run.sh passes it). */
static int scenario_lua(const char* report)
{
	static char json[16384];
	int rc, ok;
	if (!g_lua_dir) { printf("lua: --lua-dir DIR (headless/tools/clang/runtime/lua) is required\n"); return 2; }
	json[0] = '\0';
	rc = rt_selftest_lua(g_lua_dir, json, sizeof(json));
	ok = rc == 0 && g_errors == 0;
	if (report) {
		FILE* out = fopen(report, "wb");
		if (!out) { printf("lua: cannot write report %s\n", report); return 1; }
		fprintf(out, "{\"scenario\":\"lua\",\"ok\":%s,\"log_errors\":%d,\"result\":%s}\n", ok ? "true" : "false", g_errors, json[0] ? json : "null");
		fclose(out);
	}
	printf("lua: %s\n", ok ? "OK" : "FAILED");
	return ok ? 0 : 1;
}

/* Scenario setup: rt_setup parse / choice / pregame readback (rt_setup_selftest.cpp); --game DIR adds the session part. */
static int scenario_setup(const char* report)
{
	static char json[32768];
	gc_host host = {NULL, host_log, host_read_file, host_event};
	int rc, ok;
	if (!g_game_dir) host.read_file = NULL;
	json[0] = '\0';
	rc = rt_selftest_setup(&host, json, sizeof(json));
	ok = rc == 0 && g_errors == 0;
	if (report) {
		FILE* out = fopen(report, "wb");
		if (!out) { printf("setup: cannot write report %s\n", report); return 1; }
		fprintf(out, "{\"scenario\":\"setup\",\"ok\":%s,\"log_errors\":%d,\"result\":%s}\n", ok ? "true" : "false", g_errors, json[0] ? json : "null");
		fclose(out);
	}
	printf("setup: %s\n", ok ? "OK" : "FAILED");
	return ok ? 0 : 1;
}

/* Scenario vfs: rt_vfs checks (rt_vfs_selftest.cpp); --game DIR adds the real export (vfs-index.json + lua files). */
static int scenario_vfs(const char* report)
{
	static char json[16384];
	gc_host host = {NULL, host_log, host_read_file, host_event};
	int rc, ok;
	json[0] = '\0';
	rc = rt_selftest_vfs(&host, json, sizeof(json));
	ok = rc == 0 && g_errors == 0;
	if (report) {
		FILE* out = fopen(report, "wb");
		if (!out) { printf("vfs: cannot write report %s\n", report); return 1; }
		fprintf(out, "{\"scenario\":\"vfs\",\"ok\":%s,\"log_errors\":%d,\"result\":%s}\n", ok ? "true" : "false", g_errors, json[0] ? json : "null");
		fclose(out);
	}
	printf("vfs: %s\n", ok ? "OK" : "FAILED");
	return ok ? 0 : 1;
}

/* Scenario gameinfo (A5): GameInfo / DB.Query on the real DB vs civhost (rt_selftest_gameinfo.cpp); needs --game and --lua-dir. */
static int scenario_gameinfo(const char* report)
{
	static char json[65536];
	gc_host host = {NULL, host_log, host_read_file, host_event};
	int rc, ok;
	if (!g_game_dir || !g_lua_dir) { printf("gameinfo: --game DIR and --lua-dir DIR are required\n"); return 2; }
	json[0] = '\0';
	rc = rt_selftest_gameinfo(&host, g_lua_dir, json, sizeof(json));
	ok = rc == 0 && g_errors == 0;
	if (report) {
		FILE* out = fopen(report, "wb");
		if (!out) { printf("gameinfo: cannot write report %s\n", report); return 1; }
		fprintf(out, "{\"scenario\":\"gameinfo\",\"milestone\":\"M1\",\"ok\":%s,\"log_errors\":%d,\"result\":%s}\n", ok ? "true" : "false", g_errors,
			json[0] ? json : "null");
		fclose(out);
	}
	printf("gameinfo: %s (log errors: %d)\n", ok ? "OK" : "FAILED", g_errors);
	return ok ? 0 : 1;
}

/* Scenario newgame (A8, M2): new game on the real data (rt_selftest_newgame.cpp); needs --game and --lua-dir; --seed sets the setup seed. */
static int scenario_newgame(unsigned seed, const char* report)
{
	static char json[65536];
	gc_host host = {NULL, host_log, host_read_file, host_event};
	int rc, ok;
	if (!g_game_dir || !g_lua_dir) { printf("newgame: --game DIR and --lua-dir DIR are required\n"); return 2; }
	json[0] = '\0';
	rc = rt_selftest_newgame(&host, g_lua_dir, seed, json, sizeof(json));
	ok = rc == 0 && g_errors == 0;
	if (report) {
		FILE* out = fopen(report, "wb");
		if (!out) { printf("newgame: cannot write report %s\n", report); return 1; }
		fprintf(out, "{\"scenario\":\"newgame\",\"milestone\":\"M2\",\"ok\":%s,\"log_errors\":%d,\"result\":%s}\n", ok ? "true" : "false", g_errors,
			json[0] ? json : "null");
		fclose(out);
	}
	printf("newgame: %s (log errors: %d)\n", ok ? "OK" : "FAILED", g_errors);
	return ok ? 0 : 1;
}

/* Scenario observer (A9, M3 / M6): N AI-only turns through the turn loop (rt_selftest_observer.cpp); needs --game; --turns (run.sh
 * defaults it to 100 for this scenario), --seed, --ai (AI civilizations besides the first, default 1). */
static int scenario_observer(unsigned seed, unsigned turns, unsigned ai, const char* report)
{
	static char json[262144];
	gc_host host = {NULL, host_log, host_read_file, host_event};
	int rc, ok;
	if (!g_game_dir) { printf("observer: --game DIR is required\n"); return 2; }
	json[0] = '\0';
	rc = rt_selftest_observer(&host, seed, turns, ai, json, sizeof(json));
	ok = rc == 0 && g_errors == 0;
	if (report) {
		FILE* out = fopen(report, "wb");
		if (!out) { printf("observer: cannot write report %s\n", report); return 1; }
		fprintf(out, "{\"scenario\":\"observer\",\"milestone\":\"M3\",\"ok\":%s,\"log_errors\":%d,\"result\":%s}\n", ok ? "true" : "false", g_errors,
			json[0] ? json : "null");
		fclose(out);
	}
	printf("observer: %s (log errors: %d)\n", ok ? "OK" : "FAILED", g_errors);
	return ok ? 0 : 1;
}

/* Scenario dirtybench (N1.3): cost of dirty plots on a Huge map (rt_selftest_dirtybench.cpp); needs --game; use --ai 7 (8 civilizations), --turns (default 30). */
static int scenario_dirtybench(unsigned seed, unsigned turns, unsigned ai, const char* report)
{
	static char json[262144];
	gc_host host = {NULL, host_log, host_read_file, host_event};
	int rc, ok;
	if (!g_game_dir) { printf("dirtybench: --game DIR is required\n"); return 2; }
	json[0] = '\0';
	rc = rt_selftest_dirtybench(&host, seed, turns, ai, json, sizeof(json));
	ok = rc == 0 && g_errors == 0;
	if (report) {
		FILE* out = fopen(report, "wb");
		if (!out) { printf("dirtybench: cannot write report %s\n", report); return 1; }
		fprintf(out, "{\"scenario\":\"dirtybench\",\"task\":\"N1.3\",\"ok\":%s,\"log_errors\":%d,\"result\":%s}\n", ok ? "true" : "false", g_errors,
			json[0] ? json : "null");
		fclose(out);
	}
	printf("dirtybench: %s (log errors: %d)\n", ok ? "OK" : "FAILED", g_errors);
	return ok ? 0 : 1;
}

/* Scenario humanturn (F1, M4a): human turns with end-turn blockers (rt_selftest_humanturn.cpp); needs --game; --turns (run.sh defaults
 * it to 5 for this scenario), --seed, --ai, --game-type single|network (N3.4). */
static int scenario_humanturn(unsigned seed, unsigned turns, unsigned ai, const char* game_type, const char* report)
{
	static char json[262144];
	gc_host host = {NULL, host_log, host_read_file, host_event};
	int rc, ok;
	if (!g_game_dir) { printf("humanturn: --game DIR is required\n"); return 2; }
	json[0] = '\0';
	rc = rt_selftest_humanturn(&host, seed, turns, ai, game_type, json, sizeof(json));
	ok = rc == 0 && g_errors == 0;
	if (report) {
		FILE* out = fopen(report, "wb");
		if (!out) { printf("humanturn: cannot write report %s\n", report); return 1; }
		fprintf(out, "{\"scenario\":\"humanturn\",\"milestone\":\"M4a\",\"ok\":%s,\"log_errors\":%d,\"result\":%s}\n", ok ? "true" : "false", g_errors,
			json[0] ? json : "null");
		fclose(out);
	}
	printf("humanturn: %s (game type %s, log errors: %d)\n", ok ? "OK" : "FAILED", game_type, g_errors);
	return ok ? 0 : 1;
}

/* Scenario autochoice (F3): rt_autochoice part 1 (rt_selftest_autochoice.cpp); needs --game; --turns (run.sh defaults it to 60), --seed, --ai.
 * --game-type network (N3.14): the humans of a network game (rt_selftest_acnet.cpp), --humans N. */
static int scenario_autochoice(unsigned seed, unsigned turns, unsigned ai, const char* game_type, unsigned humans, const char* report)
{
	static char json[1048576];
	gc_host host = {NULL, host_log, host_read_file, host_event};
	int rc, ok;
	if (!g_game_dir) { printf("autochoice: --game DIR is required\n"); return 2; }
	json[0] = '\0';
	rc = !strcmp(game_type, "network") ? rt_selftest_autochoice_net(&host, seed, turns, humans, ai, 1, json, sizeof(json)) : rt_selftest_autochoice(&host, seed, turns, ai, json, sizeof(json));
	ok = rc == 0 && g_errors == 0;
	if (report) {
		FILE* out = fopen(report, "wb");
		if (!out) { printf("autochoice: cannot write report %s\n", report); return 1; }
		fprintf(out, "{\"scenario\":\"autochoice\",\"task\":\"F3\",\"ok\":%s,\"log_errors\":%d,\"result\":%s}\n", ok ? "true" : "false", g_errors,
			json[0] ? json : "null");
		fclose(out);
	}
	printf("autochoice: %s (log errors: %d)\n", ok ? "OK" : "FAILED", g_errors);
	return ok ? 0 : 1;
}

/* Scenario autochoice2 (F4): rt_autochoice part 2 (rt_selftest_autochoice2.cpp); needs --game; --turns = turn budget (run.sh defaults it to 40), --seed, --ai. */
static int scenario_autochoice2(unsigned seed, unsigned turns, unsigned ai, const char* game_type, unsigned humans, const char* report)
{
	static char json[1048576];
	gc_host host = {NULL, host_log, host_read_file, host_event};
	int rc, ok;
	if (!g_game_dir) { printf("autochoice2: --game DIR is required\n"); return 2; }
	json[0] = '\0';
	rc = !strcmp(game_type, "network") ? rt_selftest_autochoice_net(&host, seed, turns, humans, ai, 2, json, sizeof(json)) : rt_selftest_autochoice2(&host, seed, turns, ai, json, sizeof(json));
	ok = rc == 0 && g_errors == 0;
	if (report) {
		FILE* out = fopen(report, "wb");
		if (!out) { printf("autochoice2: cannot write report %s\n", report); return 1; }
		fprintf(out, "{\"scenario\":\"autochoice2\",\"task\":\"F4\",\"ok\":%s,\"log_errors\":%d,\"result\":%s}\n", ok ? "true" : "false", g_errors,
			json[0] ? json : "null");
		fclose(out);
	}
	printf("autochoice2: %s (log errors: %d)\n", ok ? "OK" : "FAILED", g_errors);
	return ok ? 0 : 1;
}

/* Scenario autoplay (F8, M4): one run of the test bot in the human's seat through the C ABI (rt_selftest_autoplay.cpp); needs --game;
 * --turns (run.sh defaults it to 100), --seed, --ai. run.sh runs the seed x AI matrix as one process per run. */
static int scenario_autoplay(unsigned seed, unsigned turns, unsigned ai, const char* report)
{
	static char json[262144];
	gc_host host = {NULL, host_log, host_read_file, host_event};
	int rc, ok;
	if (!g_game_dir) { printf("autoplay: --game DIR is required\n"); return 2; }
	json[0] = '\0';
	rc = rt_selftest_autoplay(&host, seed, turns, ai, json, sizeof(json));
	ok = rc == 0 && g_errors == 0;
	if (report) {
		FILE* out = fopen(report, "wb");
		if (!out) { printf("autoplay: cannot write report %s\n", report); return 1; }
		fprintf(out, "{\"scenario\":\"autoplay\",\"task\":\"F8\",\"milestone\":\"M4\",\"ok\":%s,\"log_errors\":%d,\"result\":%s}\n", ok ? "true" : "false", g_errors,
			json[0] ? json : "null");
		fclose(out);
	}
	printf("autoplay-run: %s (seed %u, ai %u, log errors: %d)\n", ok ? "OK" : "FAILED", seed, ai, g_errors);
	return ok ? 0 : 1;
}

/* Scenario saveload (G4a, M5): one phase of the save / load pair (rt_selftest_saveload.cpp); needs --game, --phase save|load, --save-file;
 * --save-at (game turn), --continue (turns after the save), --seed, --ai. run.sh runs the save phase and then the load phase as two processes. */
static int scenario_saveload(const char* phase, const char* save_file, unsigned seed, unsigned ai, unsigned save_at, unsigned cont, unsigned strict, const char* report)
{
	static char json[262144];
	gc_host host = {NULL, host_log, host_read_file, host_event};
	int rc, ok;
	if (!g_game_dir || !phase || !save_file) { printf("saveload: --game DIR, --phase save|load and --save-file FILE are required\n"); return 2; }
	json[0] = '\0';
	rc = rt_selftest_saveload(&host, phase, save_file, seed, ai, save_at, cont, strict, json, sizeof(json));
	ok = rc == 0 && g_errors == 0;
	if (report) {
		FILE* out = fopen(report, "wb");
		if (!out) { printf("saveload: cannot write report %s\n", report); return 1; }
		fprintf(out, "{\"scenario\":\"saveload\",\"task\":\"G4a\",\"milestone\":\"M5\",\"ok\":%s,\"log_errors\":%d,\"result\":%s}\n", ok ? "true" : "false", g_errors,
			json[0] ? json : "null");
		fclose(out);
	}
	printf("saveload-run: %s (phase %s, seed %u, ai %u, log errors: %d)\n", ok ? "OK" : "FAILED", phase, seed, ai, g_errors);
	return ok ? 0 : 1;
}

/* Scenario spike-net (S1): a network game with --humans N test-bot humans and --ai A AI (rt_selftest_spikenet.cpp); needs --game;
 * --mode simultaneous|hybrid, --world-size WORLDSIZE_* (default: the setup default), --turns, --seed. */
static int scenario_spikenet(unsigned seed, unsigned turns, unsigned humans, unsigned ai, const char* mode, const char* world_size, const char* report)
{
	static char json[262144];
	gc_host host = {NULL, host_log, host_read_file, host_event};
	int rc, ok;
	if (!g_game_dir) { printf("spike-net: --game DIR is required\n"); return 2; }
	json[0] = '\0';
	rc = rt_selftest_spikenet(&host, seed, turns, humans, ai, mode, world_size, json, sizeof(json));
	ok = rc == 0 && g_errors == 0;
	if (report) {
		FILE* out = fopen(report, "wb");
		if (!out) { printf("spike-net: cannot write report %s\n", report); return 1; }
		fprintf(out, "{\"scenario\":\"spike-net\",\"task\":\"N1.1\",\"ok\":%s,\"log_errors\":%d,\"result\":%s}\n", ok ? "true" : "false", g_errors, json[0] ? json : "null");
		fclose(out);
	}
	printf("spike-net: %s (mode %s, seed %u, humans %u, ai %u, log errors: %d)\n", ok ? "OK" : "FAILED", mode, seed, humans, ai, g_errors);
	return ok ? 0 : 1;
}

/* Scenarios mp / mphybrid (N3.5): a network game with --humans N test-bot humans and --ai A AI through the C ABI (rt_selftest_mp.cpp); needs --game;
 * --turns, --seed, --world-size. mp = simultaneous, mphybrid = hybrid. */
static int scenario_mp(const char* name, unsigned seed, unsigned turns, unsigned humans, unsigned ai, const char* world_size, const char* report)
{
	static char json[262144];
	gc_host host = {NULL, host_log, host_read_file, host_event};
	int rc, ok;
	if (!g_game_dir) { printf("%s: --game DIR is required\n", name); return 2; }
	json[0] = '\0';
	rc = rt_selftest_mp(&host, seed, turns, humans, ai, !strcmp(name, "mphybrid") ? "hybrid" : "simultaneous", world_size, json, sizeof(json));
	ok = rc == 0 && g_errors == 0;
	if (report) {
		FILE* out = fopen(report, "wb");
		if (!out) { printf("%s: cannot write report %s\n", name, report); return 1; }
		fprintf(out, "{\"scenario\":\"%s\",\"task\":\"N3.5\",\"ok\":%s,\"log_errors\":%d,\"result\":%s}\n", name, ok ? "true" : "false", g_errors, json[0] ? json : "null");
		fclose(out);
	}
	printf("%s: %s (seed %u, humans %u, ai %u, log errors: %d)\n", name, ok ? "OK" : "FAILED", seed, humans, ai, g_errors);
	return ok ? 0 : 1;
}

/* Scenario s2 (S2, N3.16): what the core's phases lose, counted per human (rt_selftest_s2.cpp); --game-type network|single, --humans, --ai, --turns, --seed, --world-size. */
static int scenario_s2(unsigned seed, unsigned turns, unsigned humans, unsigned ai, const char* game_type, const char* world_size, const char* report)
{
	static char json[262144];
	gc_host host = {NULL, host_log, host_read_file, host_event};
	int rc, ok;
	if (!g_game_dir) { printf("s2: --game DIR is required\n"); return 2; }
	json[0] = '\0';
	rc = rt_selftest_s2(&host, seed, turns, humans, ai, game_type, world_size, json, sizeof(json));
	ok = rc == 0 && g_errors == 0;
	if (report) {
		FILE* out = fopen(report, "wb");
		if (!out) { printf("s2: cannot write report %s\n", report); return 1; }
		fprintf(out, "{\"scenario\":\"s2\",\"task\":\"N3.16\",\"ok\":%s,\"log_errors\":%d,\"result\":%s}\n", ok ? "true" : "false", g_errors, json[0] ? json : "null");
		fclose(out);
	}
	printf("s2: %s (game type %s, seed %u, humans %u, ai %u, log errors: %d)\n", ok ? "OK" : "FAILED", game_type, seed, humans, ai, g_errors);
	return ok ? 0 : 1;
}

/* Scenario purity (N3.15): one run of the query purity test (rt_selftest_purity.cpp), --noise 0|1|2, --humans, --ai, --turns, --seed; run.sh runs the three and compares. */
static int scenario_purity(unsigned seed, unsigned turns, unsigned humans, unsigned ai, int noise, const char* noise_kinds, const char* world_size, const char* report)
{
	static char json[262144];
	gc_host host = {NULL, host_log, host_read_file, host_event};
	int rc, ok;
	if (!g_game_dir) { printf("purity: --game DIR is required\n"); return 2; }
	json[0] = '\0';
	rc = rt_selftest_purity(&host, seed, turns, humans, ai, noise, noise_kinds, world_size, json, sizeof(json));
	ok = rc == 0 && g_errors == 0;
	if (report) {
		FILE* out = fopen(report, "wb");
		if (!out) { printf("purity: cannot write report %s\n", report); return 1; }
		fprintf(out, "{\"scenario\":\"purity\",\"task\":\"N3.15\",\"ok\":%s,\"log_errors\":%d,\"result\":%s}\n", ok ? "true" : "false", g_errors, json[0] ? json : "null");
		fclose(out);
	}
	printf("purity: %s (noise %d, seed %u, humans %u, ai %u, log errors: %d)\n", ok ? "OK" : "FAILED", noise, seed, humans, ai, g_errors);
	return ok ? 0 : 1;
}

/* Scenario timer (N3.6): a network game with --humans N humans played by the scenario (rt_selftest_timer.cpp); needs --game; --turns, --seed, --ai, --world-size. */
static int scenario_timer(unsigned seed, unsigned turns, unsigned humans, unsigned ai, const char* world_size, const char* report)
{
	static char json[262144];
	gc_host host = {NULL, host_log, host_read_file, host_event};
	int rc, ok;
	if (!g_game_dir) { printf("timer: --game DIR is required\n"); return 2; }
	json[0] = '\0';
	rc = rt_selftest_timer(&host, seed, turns, humans, ai, world_size, json, sizeof(json));
	ok = rc == 0 && g_errors == 0;
	if (report) {
		FILE* out = fopen(report, "wb");
		if (!out) { printf("timer: cannot write report %s\n", report); return 1; }
		fprintf(out, "{\"scenario\":\"timer\",\"task\":\"N3.6\",\"ok\":%s,\"log_errors\":%d,\"result\":%s}\n", ok ? "true" : "false", g_errors, json[0] ? json : "null");
		fclose(out);
	}
	printf("timer: %s (seed %u, humans %u, ai %u, log errors: %d)\n", ok ? "OK" : "FAILED", seed, humans, ai, g_errors);
	return ok ? 0 : 1;
}

/* Scenario dirty (N3.8): Q_PLOTS_DIRTY / Q_PLOT_BASIS / Q_PLOTS indices on a network game with --humans N bots (rt_selftest_dirty.cpp); needs --game; --turns, --seed, --ai, --world-size. */
static int scenario_dirty(unsigned seed, unsigned turns, unsigned humans, unsigned ai, const char* world_size, const char* report)
{
	static char json[262144];
	gc_host host = {NULL, host_log, host_read_file, host_event};
	int rc, ok;
	if (!g_game_dir) { printf("dirty: --game DIR is required\n"); return 2; }
	json[0] = '\0';
	rc = rt_selftest_dirty(&host, seed, turns, humans, ai, world_size, json, sizeof(json));
	ok = rc == 0 && g_errors == 0;
	if (report) {
		FILE* out = fopen(report, "wb");
		if (!out) { printf("dirty: cannot write report %s\n", report); return 1; }
		fprintf(out, "{\"scenario\":\"dirty\",\"task\":\"N3.8\",\"ok\":%s,\"log_errors\":%d,\"result\":%s}\n", ok ? "true" : "false", g_errors, json[0] ? json : "null");
		fclose(out);
	}
	printf("dirty: %s (seed %u, humans %u, ai %u, log errors: %d)\n", ok ? "OK" : "FAILED", seed, humans, ai, g_errors);
	return ok ? 0 : 1;
}

/* Scenario wbmapread (P14): every --map through CvWorldBuilderMap::Load against an independent parser (rt_selftest_wbmap.cpp); needs --game and --maps. */
static int scenario_wbmapread(const char* const* maps, int count, const char* report)
{
	static char json[1048576];
	gc_host host = {NULL, host_log, host_read_file, host_event};
	int rc, ok;
	if (!g_game_dir || !g_maps_dir || count <= 0) { printf("wbmapread: --game DIR, --maps DIR and at least one --map maps/REL are required\n"); return 2; }
	json[0] = '\0';
	rc = rt_selftest_wbmapread(&host, maps, count, json, sizeof(json));
	ok = rc == 0 && g_errors == 0;
	if (report) {
		FILE* out = fopen(report, "wb");
		if (!out) { printf("wbmapread: cannot write report %s\n", report); return 1; }
		fprintf(out, "{\"scenario\":\"wbmapread\",\"task\":\"N3.20\",\"ok\":%s,\"log_errors\":%d,\"result\":%s}\n", ok ? "true" : "false", g_errors, json[0] ? json : "null");
		fclose(out);
	}
	printf("wbmapread: %s (%d map(s), log errors: %d)\n", ok ? "OK" : "FAILED", count, g_errors);
	return ok ? 0 : 1;
}

/* Scenario wbmap (P14): a network game on one --map (with --scenario-data 1 its scenario), --turns, --seed, --ai (map only) (rt_selftest_wbmap.cpp). */
static int scenario_wbmap(const char* map, unsigned scenario_data, unsigned seed, unsigned turns, unsigned ai, const char* report)
{
	static char json[262144];
	gc_host host = {NULL, host_log, host_read_file, host_event};
	int rc, ok;
	if (!g_game_dir || !g_maps_dir || !map) { printf("wbmap: --game DIR, --maps DIR and --map maps/REL are required\n"); return 2; }
	json[0] = '\0';
	rc = rt_selftest_wbmap(&host, map, scenario_data, seed, turns, ai, json, sizeof(json));
	ok = rc == 0 && g_errors == 0;
	if (report) {
		FILE* out = fopen(report, "wb");
		if (!out) { printf("wbmap: cannot write report %s\n", report); return 1; }
		fprintf(out, "{\"scenario\":\"wbmap\",\"task\":\"N3.20\",\"ok\":%s,\"log_errors\":%d,\"result\":%s}\n", ok ? "true" : "false", g_errors, json[0] ? json : "null");
		fclose(out);
	}
	printf("wbmap: %s (%s, scenario %u, seed %u, log errors: %d)\n", ok ? "OK" : "FAILED", map, scenario_data, seed, g_errors);
	return ok ? 0 : 1;
}

/* Scenario mp8 (S4, N3.17): speed of a network game with --humans N (default 8) bot humans on --world-size (default Huge); rt_selftest_mp8.cpp; needs --game. */
static int scenario_mp8(unsigned seed, unsigned turns, unsigned humans, unsigned ai, const char* world_size, const char* report)
{
	static char json[262144];
	gc_host host = {NULL, host_log, host_read_file, host_event};
	int rc, ok;
	if (!g_game_dir) { printf("mp8: --game DIR is required\n"); return 2; }
	json[0] = '\0';
	rc = rt_selftest_mp8(&host, seed, turns, humans, ai, world_size, json, sizeof(json));
	ok = rc == 0 && g_errors == 0;
	if (report) {
		FILE* out = fopen(report, "wb");
		if (!out) { printf("mp8: cannot write report %s\n", report); return 1; }
		fprintf(out, "{\"scenario\":\"mp8\",\"task\":\"N3.17\",\"ok\":%s,\"log_errors\":%d,\"result\":%s}\n", ok ? "true" : "false", g_errors, json[0] ? json : "null");
		fclose(out);
	}
	printf("mp8: %s (seed %u, humans %u, ai %u, log errors: %d)\n", ok ? "OK" : "FAILED", seed, humans, ai, g_errors);
	return ok ? 0 : 1;
}

/* Scenario mapscripts (N3.18): one game on --map-script (a vfs-index.json path) and --world-size (rt_selftest_mapscripts.cpp); needs --game. */
static int scenario_mapscripts(unsigned seed, unsigned turns, unsigned ai, const char* map_script, const char* world_size, const char* report)
{
	static char json[65536];
	gc_host host = {NULL, host_log, host_read_file, host_event};
	int rc, ok;
	if (!g_game_dir || !map_script || !map_script[0]) { printf("mapscripts: --game DIR and --map-script PATH are required\n"); return 2; }
	json[0] = '\0';
	rc = rt_selftest_mapscripts(&host, seed, turns, ai, map_script, world_size, json, sizeof(json));
	ok = rc == 0 && g_errors == 0;
	if (report) {
		FILE* out = fopen(report, "wb");
		if (!out) { printf("mapscripts: cannot write report %s\n", report); return 1; }
		fprintf(out, "{\"scenario\":\"mapscripts\",\"task\":\"N3.18\",\"ok\":%s,\"log_errors\":%d,\"result\":%s}\n", ok ? "true" : "false", g_errors, json[0] ? json : "null");
		fclose(out);
	}
	printf("mapscripts: %s (%s, %s, log errors: %d)\n", ok ? "OK" : "FAILED", map_script, world_size && world_size[0] ? world_size : "WORLDSIZE_DUEL", g_errors);
	return ok ? 0 : 1;
}

/* Scenario mpmsg (N3.21, HYG-0013): the core's message lines for every human (rt_selftest_mpmsg.cpp); --humans (>= 2), --ai (>= 1), --turns, --seed; needs --game. */
static int scenario_mpmsg(unsigned seed, unsigned turns, unsigned humans, unsigned ai, const char* report)
{
	static char json[65536];
	gc_host host = {NULL, host_log, host_read_file, host_event};
	int rc, ok;
	if (!g_game_dir) { printf("mpmsg: --game DIR is required\n"); return 2; }
	json[0] = '\0';
	rc = rt_selftest_mpmsg(&host, seed, turns, humans, ai, json, sizeof(json));
	ok = rc == 0 && g_errors == 0;
	if (report) {
		FILE* out = fopen(report, "wb");
		if (!out) { printf("mpmsg: cannot write report %s\n", report); return 1; }
		fprintf(out, "{\"scenario\":\"mpmsg\",\"task\":\"N3.21\",\"ok\":%s,\"log_errors\":%d,\"result\":%s}\n", ok ? "true" : "false", g_errors, json[0] ? json : "null");
		fclose(out);
	}
	printf("mpmsg: %s (seed %u, humans %u, ai %u, log errors: %d)\n", ok ? "OK" : "FAILED", seed, humans, ai, g_errors);
	return ok ? 0 : 1;
}
#include "harness_slots.h" /* N3.10 peace, N3.12 mpslots, N3.13 mpsave */

int main(int argc, char** argv)
{
	static const char* maps[512];
	int map_count = 0;
	unsigned scenario_data = 0;
	const char* scenario = "boot";
	const char* report = NULL;
	const char* phase = NULL;
	const char* save_file = NULL;
	const char* mode = "simultaneous";
	const char* world_size = "";
	const char* game_type = "single";
	const char* map_script = NULL;
	unsigned seed = 1, turns = 3, ai = 1, save_at = 30, cont = 20, strict = 0, humans = 2;
	int noise = 0;
	const char* noise_kinds = "";
	int i;
	setvbuf(stdout, NULL, _IOLBF, 0); /* keep stdout ordered with stderr and complete if the module traps */
#if defined(__linux__)
	signal(SIGSEGV, on_fatal_signal);
	signal(SIGBUS, on_fatal_signal);
	signal(SIGFPE, on_fatal_signal);
	signal(SIGABRT, on_fatal_signal);
#endif
	for (i = 1; i < argc; ++i) {
		const char* a = argv[i];
		const char* v = i + 1 < argc ? argv[i + 1] : NULL;
		if (!strcmp(a, "--scenario") && v) { scenario = v; ++i; }
		else if (!strcmp(a, "--game") && v) { g_game_dir = v; ++i; }
		else if (!strcmp(a, "--lua-dir") && v) { g_lua_dir = v; ++i; }
		else if (!strcmp(a, "--seed") && v) { seed = (unsigned)strtoul(v, NULL, 0); ++i; }
		else if (!strcmp(a, "--turns") && v) { turns = (unsigned)strtoul(v, NULL, 0); ++i; }
		else if (!strcmp(a, "--ai") && v) { ai = (unsigned)strtoul(v, NULL, 0); ++i; }
		else if (!strcmp(a, "--report") && v) { report = v; ++i; }
		else if (!strcmp(a, "--phase") && v) { phase = v; ++i; }
		else if (!strcmp(a, "--save-file") && v) { save_file = v; ++i; }
		else if (!strcmp(a, "--save-at") && v) { save_at = (unsigned)strtoul(v, NULL, 0); ++i; }
		else if (!strcmp(a, "--continue") && v) { cont = (unsigned)strtoul(v, NULL, 0); ++i; }
		else if (!strcmp(a, "--strict") && v) { strict = (unsigned)strtoul(v, NULL, 0); ++i; }
		else if (!strcmp(a, "--noise-kinds") && v) { noise_kinds = v; ++i; }
		else if (!strcmp(a, "--noise") && v) { noise = (int)strtol(v, NULL, 0); ++i; }
		else if (!strcmp(a, "--humans") && v) { humans = (unsigned)strtoul(v, NULL, 0); ++i; }
		else if (!strcmp(a, "--mode") && v) { mode = v; ++i; }
			else if (!strcmp(a, "--world-size") && v) { world_size = v; ++i; }
		else if (!strcmp(a, "--game-type") && v) { game_type = v; ++i; }
		else if (!strcmp(a, "--map-script") && v) { map_script = v; ++i; }
		else if (!strcmp(a, "--maps") && v) { g_maps_dir = v; ++i; }
		else if (!strcmp(a, "--map") && v) { if (map_count < (int)(sizeof(maps) / sizeof(maps[0]))) maps[map_count++] = v; ++i; }
		else if (!strcmp(a, "--scenario-data") && v) { scenario_data = (unsigned)strtoul(v, NULL, 0); ++i; }
		else { fprintf(stderr, "harness: unknown argument %s\n", a); return 2; }
	}
	if (!strcmp(scenario, "boot")) return scenario_boot(seed, report);
	if (!strcmp(scenario, "db")) return scenario_db(report);
	if (!strcmp(scenario, "load")) return scenario_load(report);
	if (!strcmp(scenario, "lua")) return scenario_lua(report);
	if (!strcmp(scenario, "vfs")) return scenario_vfs(report);
	if (!strcmp(scenario, "gameinfo")) return scenario_gameinfo(report);
	if (!strcmp(scenario, "setup")) return scenario_setup(report);
	if (!strcmp(scenario, "newgame")) return scenario_newgame(seed, report);
	if (!strcmp(scenario, "observer")) return scenario_observer(seed, turns, ai, report);
	if (!strcmp(scenario, "dirtybench")) return scenario_dirtybench(seed, turns, ai, report);
	if (!strcmp(scenario, "humanturn")) return scenario_humanturn(seed, turns, ai, game_type, report);
	if (!strcmp(scenario, "autochoice")) return scenario_autochoice(seed, turns, ai, game_type, humans, report);
	if (!strcmp(scenario, "autochoice2")) return scenario_autochoice2(seed, turns, ai, game_type, humans, report);
	if (!strcmp(scenario, "autoplay")) return scenario_autoplay(seed, turns, ai, report);
	if (!strcmp(scenario, "saveload")) return scenario_saveload(phase, save_file, seed, ai, save_at, cont, strict, report);
	if (!strcmp(scenario, "spike-net")) return scenario_spikenet(seed, turns, humans, ai, mode, world_size, report);
	if (!strcmp(scenario, "mp") || !strcmp(scenario, "mphybrid")) return scenario_mp(scenario, seed, turns, humans, ai, world_size, report);
	if (!strcmp(scenario, "s2")) return scenario_s2(seed, turns, humans, ai, game_type, world_size, report);
	if (!strcmp(scenario, "purity")) return scenario_purity(seed, turns, humans, ai, noise, noise_kinds, world_size, report);
	if (!strcmp(scenario, "timer")) return scenario_timer(seed, turns, humans, ai, world_size, report);
	if (!strcmp(scenario, "dirty")) return scenario_dirty(seed, turns, humans, ai, world_size, report);
	if (!strcmp(scenario, "mp8")) return scenario_mp8(seed, turns, humans, ai, world_size, report);
	if (!strcmp(scenario, "mapscripts")) return scenario_mapscripts(seed, turns, ai, map_script, world_size, report);
	if (!strcmp(scenario, "mpmsg")) return scenario_mpmsg(seed, turns, humans, ai, report);
	if (!strcmp(scenario, "peace")) return scenario_peace(seed, turns, humans, ai, mode, world_size, report);
	if (!strcmp(scenario, "diplo")) return scenario_diplo(seed, turns, humans, ai, mode, world_size, report);
	if (!strcmp(scenario, "mpslots")) return scenario_mpslots(seed, turns, humans, ai, mode, world_size, report);
	if (!strcmp(scenario, "mpsave")) return scenario_mpsave(phase, save_file, seed, humans, ai, save_at, cont, strict, world_size, report);
	if (!strcmp(scenario, "replicaui")) return scenario_replicaui(seed, turns, ai, world_size, report);
	if (!strcmp(scenario, "wbmapread")) return scenario_wbmapread(maps, map_count, report);
	if (!strcmp(scenario, "wbmap")) return scenario_wbmap(map_count > 0 ? maps[0] : NULL, scenario_data, seed, turns, ai, report);
	printf("scenario %s: not implemented (later B1 tasks)\n", scenario);
	return 3;
}
