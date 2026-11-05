/* Scenario glue of the b2/slots packet (included by harness.c right before main; uses its host_log / host_read_file / host_event, g_game_dir and g_errors):
 *  peace    (N3.10)  war and white peace between humans, rt_selftest_peace.cpp
 *  mpslots  (9.2, N3.12)                 disconnect, kick, return and takeover of slots, rt_selftest_mpslots.cpp
 *  mpsave   (9.2, N3.13)                 save and load of a network game with several humans, rt_selftest_mpsave.cpp (two processes: phase save, phase load)
 *  diplo    (B4 fix-deal-crash)          the diplomacy screens' messages to an AI on the server, rt_selftest_diplo.cpp
 *  replicaui (B4 M2 WO-1)                a replica serving a Lua UI (attach, graphical setup, events, getters between batches, ui ops, resync and invalidation), rt_selftest_replica_ui.cpp
 */
#include "rt_selftest_slots.h"

/* Writes the scenario's report file {"scenario":..,"task":..,"ok":..,"log_errors":..,"result":..} and prints the result line; returns the process exit code. */
static int slots_finish(const char* name, const char* task, int rc, const char* json, const char* report, const char* detail)
{
	int ok = rc == 0 && g_errors == 0;
	if (report) {
		FILE* out = fopen(report, "wb");
		if (!out) { printf("%s: cannot write report %s\n", name, report); return 1; }
		fprintf(out, "{\"scenario\":\"%s\",\"task\":\"%s\",\"ok\":%s,\"log_errors\":%d,\"result\":%s}\n", name, task, ok ? "true" : "false", g_errors, json[0] ? json : "null");
		fclose(out);
	}
	printf("%s: %s (%s, log errors: %d)\n", name, ok ? "OK" : "FAILED", detail, g_errors);
	return ok ? 0 : 1;
}

/* Scenario peace (N3.10): a network game with --humans N humans played by the scenario (rt_selftest_peace.cpp); needs --game; --mode, --turns, --seed, --ai, --world-size. */
static int scenario_peace(unsigned seed, unsigned turns, unsigned humans, unsigned ai, const char* mode, const char* world_size, const char* report)
{
	static char json[262144];
	gc_host host = {NULL, host_log, host_read_file, host_event};
	char detail[128];
	int rc;
	if (!g_game_dir) { printf("peace: --game DIR is required\n"); return 2; }
	json[0] = '\0';
	rc = rt_selftest_peace(&host, seed, turns, humans, ai, mode, world_size, json, sizeof(json));
	sprintf(detail, "%s, seed %u, humans %u, ai %u", mode, seed, humans, ai);
	return slots_finish("peace", "N3.10", rc, json, report, detail);
}

/* Scenario mpsave (N3.13): phase "save" or "load", one process each (rt_selftest_mpsave.cpp); needs --game and --save-file; --seed, --humans, --ai, --save-at, --continue, --strict-continuation. */
static int scenario_mpsave(const char* phase, const char* save_file, unsigned seed, unsigned humans, unsigned ai, unsigned save_at, unsigned cont, unsigned strict, const char* world_size, const char* report)
{
	static char json[262144];
	gc_host host = {NULL, host_log, host_read_file, host_event};
	char detail[160];
	int rc;
	if (!g_game_dir) { printf("mpsave: --game DIR is required\n"); return 2; }
	if (!phase || !save_file) { printf("mpsave: --phase save|load and --save-file FILE are required\n"); return 2; }
	json[0] = '\0';
	rc = rt_selftest_mpsave(&host, phase, save_file, seed, humans, ai, save_at, cont, strict, world_size, json, sizeof(json));
	sprintf(detail, "phase %s, seed %u, humans %u, ai %u", phase, seed, humans, ai);
	return slots_finish("mpsave", "N3.13", rc, json, report, detail);
}

/* Scenario mpslots (N3.12): a network game with --humans N test-bot humans and --ai A AI (rt_selftest_mpslots.cpp); needs --game; --turns, --seed, --mode, --world-size. */
static int scenario_mpslots(unsigned seed, unsigned turns, unsigned humans, unsigned ai, const char* mode, const char* world_size, const char* report)
{
	static char json[262144];
	gc_host host = {NULL, host_log, host_read_file, host_event};
	char detail[128];
	int rc;
	if (!g_game_dir) { printf("mpslots: --game DIR is required\n"); return 2; }
	json[0] = '\0';
	rc = rt_selftest_mpslots(&host, seed, turns, humans, ai, mode, world_size, json, sizeof(json));
	sprintf(detail, "%s, seed %u, humans %u, ai %u", mode, seed, humans, ai);
	return slots_finish("mpslots", "N3.12", rc, json, report, detail);
}

/* Scenario replicaui (B4 M2 WO-1, rt_selftest_replica_ui.cpp): needs --game; --turns (default 6; 30 or more also requires GameEvents.PlayerAdoptPolicy), --seed, --ai, --world-size. */
static int scenario_replicaui(unsigned seed, unsigned turns, unsigned ai, const char* world_size, const char* report)
{
	static char json[262144];
	gc_host host = {NULL, host_log, host_read_file, host_event};
	char detail[128];
	int rc;
	if (!g_game_dir) { printf("replicaui: --game DIR is required\n"); return 2; }
	json[0] = '\0';
	rc = rt_selftest_replica_ui(&host, seed, turns, ai, world_size, json, sizeof(json));
	sprintf(detail, "seed %u, turns %u, ai %u", seed, turns, ai);
	return slots_finish("replicaui", "B4-M2-WO1", rc, json, report, detail);
}

/* Scenario diplo (B4 fix-deal-crash, rt_selftest_diplo.cpp): needs --game; --humans (default 1), --ai, --mode (default hybrid), --seed, --world-size. */
static int scenario_diplo(unsigned seed, unsigned turns, unsigned humans, unsigned ai, const char* mode, const char* world_size, const char* report)
{
	static char json[262144];
	gc_host host = {NULL, host_log, host_read_file, host_event};
	char detail[128];
	int rc;
	if (!g_game_dir) { printf("diplo: --game DIR is required\n"); return 2; }
	json[0] = '\0';
	rc = rt_selftest_diplo(&host, seed, turns, humans, ai, mode, world_size, json, sizeof(json));
	sprintf(detail, "%s, seed %u, humans %u, ai %u", mode, seed, humans, ai);
	return slots_finish("diplo", "B4-fix-deal-crash", rc, json, report, detail);
}