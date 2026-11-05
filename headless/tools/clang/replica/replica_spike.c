/* G0 spike of B4 gc-replica: a server game that journals its inputs and snapshots every turn, and a replica that
 * loads a snapshot, replays the journal and resyncs to the next snapshot. Over gamecore_abi.h only (the replica part of it).
 *
 *   replica_spike --mode server  --game DIR --out DIR --turns N [--seed S] [--humans H] [--ai A] [--world-size W] [--keep-from T] [--keep-every K]
 *   replica_spike --mode replica --game DIR --snap FILE [--journal FILE] [--next FILE] [--report FILE] [--dump FILE]
 *
 * server: a network game, H test-bot humans (autoplay_humans) and A AI. After every gc_pump the journal is taken (with a CHECK record while the
 *   humans move) and appended to DIR/journal-<turn>.bin as u32 length + GCJ1 bytes; the file of turn T runs from the human phase of T to the
 *   human phase of T+1. At every human phase the game is saved (gc_save); the snapshot is kept as DIR/snap-<turn>.gcs1 when turn >= keep-from or
 *   turn % keep-every == 0. DIR/server.tsv: one line per turn (sizes, times, the replica digest at the save).
 * replica: gc_create, gc_load_game(snap), GC_ROLE_REPLICA, every journal batch through gc_journal_apply (stops at the first divergence), then
 *   gc_resync(next). Prints one JSON line (and writes it to --report); --dump saves the replica after the journal. */
#if defined(__linux__)
#define _GNU_SOURCE
#else
#define _POSIX_C_SOURCE 200809L /* clock_gettime in wasi-libc */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gamecore_abi.h"

static const char* g_game_dir = NULL;
static int g_log_errors = 0;
static int g_quiet = 1;

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* Resident memory in KiB (Linux: VmRSS / VmHWM; wasm: the linear memory size). */
static void memory_kib(long* rss, long* peak)
{
	*rss = -1;
	*peak = -1;
#if defined(__wasm__)
	*rss = (long)(__builtin_wasm_memory_size(0) * 64);
	*peak = *rss;
#else
	FILE* f = fopen("/proc/self/status", "r");
	char line[256];
	if (!f) return;
	while (fgets(line, sizeof(line), f)) {
		if (!strncmp(line, "VmRSS:", 6)) *rss = atol(line + 6);
		if (!strncmp(line, "VmHWM:", 6)) *peak = atol(line + 6);
	}
	fclose(f);
#endif
}

static void host_log(void* user, int32_t level, const char* msg)
{
	(void)user;
	if (level == GC_LOG_ERROR) ++g_log_errors;
	if (level == GC_LOG_ERROR || (!g_quiet && level <= GC_LOG_INFO)) fprintf(stderr, "[%d] %s\n", (int)level, msg);
}

static void release_malloc(gc_buf* b) { free(b->data); }

static int read_whole(const char* path, uint8_t** data, long* n)
{
	FILE* f = fopen(path, "rb");
	if (!f) return 0;
	fseek(f, 0, SEEK_END);
	*n = ftell(f);
	fseek(f, 0, SEEK_SET);
	*data = (uint8_t*)malloc(*n > 0 ? (size_t)*n : 1);
	if (!*data || (*n > 0 && fread(*data, 1, (size_t)*n, f) != (size_t)*n)) {
		fclose(f);
		free(*data);
		return 0;
	}
	fclose(f);
	return 1;
}

static int32_t host_read_file(void* user, const char* path, gc_buf* out)
{
	char full[1024];
	uint8_t* data = NULL;
	long n = 0;
	(void)user;
	if (snprintf(full, sizeof(full), "%s/%s", g_game_dir, path) >= (int)sizeof(full)) return GC_ERR_INVALID_ARG;
	if (!read_whole(full, &data, &n)) return GC_ERR_NOT_FOUND;
	out->data = data;
	out->len = (size_t)n;
	out->release = release_malloc;
	out->owner = NULL;
	return GC_OK;
}

static int g_events = 0;
static void host_event(void* user, const gc_event* ev)
{
	(void)user;
	(void)ev;
	++g_events;
}

static int write_file(const char* path, const uint8_t* p, size_t n)
{
	FILE* f = fopen(path, "wb");
	if (!f) return 0;
	if (n && fwrite(p, 1, n, f) != n) {
		fclose(f);
		return 0;
	}
	fclose(f);
	return 1;
}

/* Q_GAME phase (GC_PHASE_*), -1 on error. */
static int game_phase(gc_instance* inst)
{
	gc_buf b = {0, 0, 0, 0};
	gc_error err;
	int phase = -1;
	if (gc_query(inst, GC_Q_GAME, NULL, 0, &b, &err) != GC_OK) return -1;
	if (b.len >= sizeof(gcq_header) + sizeof(gcq_game)) {
		gcq_game g;
		memcpy(&g, b.data + sizeof(gcq_header), sizeof(g));
		phase = g.phase;
	}
	gc_buf_release(&b);
	return phase;
}

static gc_instance* boot(double* ms)
{
	gc_host host = {NULL, host_log, host_read_file, host_event};
	gc_error err;
	double t0 = now_ms();
	gc_instance* inst = gc_create(&host, &err);
	*ms = now_ms() - t0;
	if (!inst) fprintf(stderr, "gc_create failed: %d %s\n", err.code, err.message);
	return inst;
}

static int run_server(const char* out_dir, int turns, unsigned seed, int humans, int ai, const char* world_size, int keep_from, int keep_every)
{
	char setup[8192], players[6000], slots[256], path[1024];
	gc_error err;
	gc_pump_result pr;
	double create_ms, t0;
	gc_instance* inst;
	FILE* tsv;
	FILE* journal = NULL;
	int snap_turn = -1, start_turn = -1, i, rc, pumps_in_turn = 0, batches_in_turn = 0, checks_in_turn = 0;
	long journal_bytes_turn = 0, rss, peak;
	double turn_t0 = 0, check_ms_turn = 0, human_ms_turn = 0;
	players[0] = slots[0] = '\0';
	for (i = 0; i < humans; ++i) {
		char one[128];
		snprintf(one, sizeof(one), "%s{\"kind\":\"human\",\"civ\":\"%s\",\"nick\":\"late%d\"}", i ? "," : "", i ? "random" : "CIVILIZATION_ROME", i);
		strcat(players, one);
		snprintf(one, sizeof(one), "%s%d", i ? "," : "", i);
		strcat(slots, one);
	}
	for (i = 0; i < ai; ++i) strcat(players, (humans || i) ? ",{\"kind\":\"ai\",\"civ\":\"random\"}" : "{\"kind\":\"ai\",\"civ\":\"random\"}");
	snprintf(setup, sizeof(setup),
		"{\"seed\":%u,\"map_seed\":%u,\"game_type\":\"network\",\"players\":[%s],\"turn_mode\":\"simultaneous\",\"autoplay_humans\":[%s],\"handicap\":\"HANDICAP_CHIEFTAIN\",\"world_size\":\"%s\"}",
		seed, seed, players, slots, world_size);
	inst = boot(&create_ms);
	if (!inst) return 1;
	t0 = now_ms();
	rc = gc_new_game(inst, setup, &err);
	if (rc != GC_OK) {
		fprintf(stderr, "gc_new_game: %d %s\n", rc, err.message);
		return 1;
	}
	fprintf(stderr, "server: new game in %.0f ms (create %.0f ms): %s\n", now_ms() - t0, create_ms, setup);
	if (gc_set_role(inst, GC_ROLE_SERVER, -1, &err) != GC_OK) {
		fprintf(stderr, "gc_set_role: %s\n", err.message);
		return 1;
	}
	snprintf(path, sizeof(path), "%s/server.tsv", out_dir);
	tsv = fopen(path, "w");
	if (!tsv) return 1;
	fprintf(tsv, "turn\tturn_ms\thuman_ms\tpumps\tbatches\tchecks\tcheck_ms\tjournal_bytes\tsave_ms\tsnap_bytes\tdigest3\tdigest3_ms\trss_kib\tkept\n");
	turn_t0 = now_ms();
	for (;;) {
		double tp = now_ms();
		gc_buf jb = {0, 0, 0, 0};
		int phase;
		rc = gc_pump(inst, 64, &pr, &err);
		if (rc != GC_OK) {
			fprintf(stderr, "server: gc_pump: %d %s\n", rc, err.message);
			break;
		}
		if (pr.status == GC_PUMP_GAME_OVER) {
			fprintf(stderr, "server: game over at turn %d\n", pr.turn);
			break;
		}
		++pumps_in_turn;
		phase = game_phase(inst);
		if (phase == GC_PHASE_HUMANS) human_ms_turn += now_ms() - tp;
		{
			double tc = now_ms();
			const uint32_t flags = phase == GC_PHASE_HUMANS ? GC_JOURNAL_TAKE_CHECK : 0u;
			if (gc_journal_take(inst, flags, &jb, &err) != GC_OK) {
				fprintf(stderr, "server: gc_journal_take: %s\n", err.message);
				return 1;
			}
			if (flags) {
				check_ms_turn += now_ms() - tc;
				++checks_in_turn;
			}
		}
		if (journal && jb.len > 16) {
			uint32_t len = (uint32_t)jb.len;
			fwrite(&len, 4, 1, journal);
			fwrite(jb.data, 1, jb.len, journal);
			journal_bytes_turn += 4 + (long)jb.len;
			++batches_in_turn;
		}
		gc_buf_release(&jb);
		if (pr.turn != snap_turn) {
			gc_buf snap = {0, 0, 0, 0};
			double ts = now_ms(), save_ms, dig_ms;
			uint64_t d3 = 0;
			int kept;
			if (gc_save(inst, &snap, &err) != GC_OK) continue; /* not at the human phase yet */
			save_ms = now_ms() - ts;
			ts = now_ms();
			gc_state_digest(inst, -3, &d3, &err);
			dig_ms = now_ms() - ts;
			if (start_turn < 0) start_turn = pr.turn;
			kept = pr.turn >= keep_from || (keep_every > 0 && pr.turn % keep_every <= 1); /* pairs T, T+1 for the patch sizes */
			if (kept) {
				snprintf(path, sizeof(path), "%s/snap-%03d.gcs1", out_dir, pr.turn);
				write_file(path, snap.data, snap.len);
			}
			memory_kib(&rss, &peak);
			if (snap_turn >= 0) {
				/* the line of the previous turn closes here */
			}
			fprintf(tsv, "%d\t%.0f\t%.0f\t%d\t%d\t%d\t%.1f\t%ld\t%.0f\t%lu\t%016llx\t%.1f\t%ld\t%d\n", pr.turn, now_ms() - turn_t0, human_ms_turn, pumps_in_turn,
				batches_in_turn, checks_in_turn, check_ms_turn, journal_bytes_turn, save_ms, (unsigned long)snap.len, (unsigned long long)d3, dig_ms, rss, kept);
			fflush(tsv);
			fprintf(stderr, "server: turn %d: %lu bytes (save %.0f ms, digest %.0f ms), turn %.0f ms, rss %ld KiB\n", pr.turn, (unsigned long)snap.len, save_ms, dig_ms,
				now_ms() - turn_t0, rss);
			gc_buf_release(&snap);
			if (journal) fclose(journal);
			journal = NULL;
			snap_turn = pr.turn;
			if (pr.turn >= start_turn + turns) break;
			snprintf(path, sizeof(path), "%s/journal-%03d.bin", out_dir, pr.turn);
			journal = fopen(path, "wb");
			pumps_in_turn = batches_in_turn = checks_in_turn = 0;
			journal_bytes_turn = 0;
			check_ms_turn = human_ms_turn = 0;
			turn_t0 = now_ms();
		}
	}
	if (journal) fclose(journal);
	fclose(tsv);
	fprintf(stderr, "server: done, %d log error(s)\n", g_log_errors);
	gc_destroy(inst);
	return 0;
}

static int run_replica(const char* snap_path, const char* journal_path, const char* next_path, const char* report, const char* dump)
{
	uint8_t *snap = NULL, *jr = NULL, *next = NULL;
	long snap_n = 0, jr_n = 0, next_n = 0, rss0, peak0, rss1, peak1, rss2, peak2, rss3 = -1, peak3 = -1;
	double create_ms, load_ms, apply_ms = 0, apply_max = 0, resync_ms = -1, dig_ms = 0;
	gc_error err;
	gc_instance* inst;
	uint64_t d_load = 0, d_end = 0, d_resync = 0;
	int batches = 0, applied = 0, diverged = -1, rc;
	char diverge_msg[300] = "";
	char json[4096];
	FILE* f;
	if (!read_whole(snap_path, &snap, &snap_n)) {
		fprintf(stderr, "replica: cannot read %s\n", snap_path);
		return 2;
	}
	memory_kib(&rss0, &peak0);
	inst = boot(&create_ms);
	if (!inst) return 1;
	memory_kib(&rss1, &peak1);
	{
		double t = now_ms();
		rc = gc_load_game(inst, snap, (uint32_t)snap_n, &err);
		load_ms = now_ms() - t;
	}
	if (rc != GC_OK) {
		fprintf(stderr, "replica: gc_load_game: %d %s\n", rc, err.message);
		return 1;
	}
	memory_kib(&rss2, &peak2);
	if (gc_set_role(inst, GC_ROLE_REPLICA, -1, &err) != GC_OK) { /* before the digest: the replica takes the server's slots on line from the save */
		fprintf(stderr, "replica: gc_set_role: %s\n", err.message);
		return 1;
	}
	{
		double t = now_ms();
		gc_state_digest(inst, -3, &d_load, &err);
		dig_ms = now_ms() - t;
	}
	if (journal_path && read_whole(journal_path, &jr, &jr_n)) {
		long pos = 0;
		while (pos + 4 <= jr_n) {
			uint32_t len;
			double t;
			memcpy(&len, jr + pos, 4);
			pos += 4;
			if (pos + (long)len > jr_n) break;
			++batches;
			if (diverged < 0) {
				t = now_ms();
				rc = gc_journal_apply(inst, jr + pos, len, &err);
				t = now_ms() - t;
				apply_ms += t;
				if (t > apply_max) apply_max = t;
				if (rc == GC_OK)
					++applied;
				else {
					diverged = batches - 1;
					snprintf(diverge_msg, sizeof(diverge_msg), "%d: %s", rc, err.message);
					for (char* c = diverge_msg; *c; ++c)
						if (*c == '"' || *c == '\\') *c = '\'';
				}
			}
			pos += len;
		}
		gc_state_digest(inst, -3, &d_end, &err);
		if (dump) { /* the replica's own save after the journal, to compare with the server's next snapshot by section */
			gc_buf b = {0, 0, 0, 0};
			if (gc_save(inst, &b, &err) == GC_OK)
				write_file(dump, b.data, b.len);
			else
				fprintf(stderr, "replica: gc_save for --dump: %s\n", err.message);
			gc_buf_release(&b);
		}
	}
	if (next_path && read_whole(next_path, &next, &next_n)) {
		double t = now_ms();
		rc = gc_resync(inst, next, (uint32_t)next_n, &err);
		resync_ms = now_ms() - t;
		if (rc != GC_OK) fprintf(stderr, "replica: gc_resync: %d %s\n", rc, err.message);
		gc_state_digest(inst, -3, &d_resync, &err);
		memory_kib(&rss3, &peak3);
	}
	snprintf(json, sizeof(json),
		"{\"snap\":\"%s\",\"snap_bytes\":%ld,\"create_ms\":%.0f,\"load_ms\":%.0f,\"digest_ms\":%.1f,\"digest_load\":\"%016llx\",\"batches\":%d,\"applied\":%d,"
		"\"diverged_at\":%d,\"diverge\":\"%s\",\"apply_ms\":%.1f,\"apply_max_ms\":%.1f,\"digest_end\":\"%016llx\",\"resync_ms\":%.0f,\"digest_resync\":\"%016llx\","
		"\"rss_start_kib\":%ld,\"rss_created_kib\":%ld,\"rss_loaded_kib\":%ld,\"peak_loaded_kib\":%ld,\"rss_resynced_kib\":%ld,\"peak_resynced_kib\":%ld,\"log_errors\":%d,\"events\":%d}",
		snap_path, snap_n, create_ms, load_ms, dig_ms, (unsigned long long)d_load, batches, applied, diverged, diverge_msg, apply_ms, apply_max,
		(unsigned long long)d_end, resync_ms, (unsigned long long)d_resync, rss0, rss1, rss2, peak2, rss3, peak3, g_log_errors, g_events);
	printf("%s\n", json);
	if (report && (f = fopen(report, "w"))) {
		fprintf(f, "%s\n", json);
		fclose(f);
	}
	return diverged >= 0 ? 3 : 0;
}

int main(int argc, char** argv)
{
	const char *mode = NULL, *out = NULL, *snap = NULL, *journal = NULL, *next = NULL, *report = NULL, *dump = NULL, *world = "WORLDSIZE_HUGE";
	int turns = 10, humans = 2, ai = 10, keep_from = 1 << 30, keep_every = 0, i;
	unsigned seed = 11;
	for (i = 1; i + 1 < argc; i += 2) {
		const char* k = argv[i];
		const char* v = argv[i + 1];
		if (!strcmp(k, "--mode")) mode = v;
		else if (!strcmp(k, "--game")) g_game_dir = v;
		else if (!strcmp(k, "--out")) out = v;
		else if (!strcmp(k, "--turns")) turns = atoi(v);
		else if (!strcmp(k, "--seed")) seed = (unsigned)atoi(v);
		else if (!strcmp(k, "--humans")) humans = atoi(v);
		else if (!strcmp(k, "--ai")) ai = atoi(v);
		else if (!strcmp(k, "--world-size")) world = v;
		else if (!strcmp(k, "--keep-from")) keep_from = atoi(v);
		else if (!strcmp(k, "--keep-every")) keep_every = atoi(v);
		else if (!strcmp(k, "--snap")) snap = v;
		else if (!strcmp(k, "--journal")) journal = v;
		else if (!strcmp(k, "--next")) next = v;
		else if (!strcmp(k, "--report")) report = v;
		else if (!strcmp(k, "--dump")) dump = v;
		else if (!strcmp(k, "--verbose")) g_quiet = !atoi(v);
		else {
			fprintf(stderr, "unknown argument %s\n", k);
			return 2;
		}
	}
	if (!mode || !g_game_dir) {
		fprintf(stderr, "usage: see the head of replica_spike.c\n");
		return 2;
	}
	if (!strcmp(mode, "server")) return out ? run_server(out, turns, seed, humans, ai, world, keep_from, keep_every) : 2;
	if (!strcmp(mode, "replica")) return snap ? run_replica(snap, journal, next, report, dump) : 2;
	fprintf(stderr, "unknown mode %s\n", mode);
	return 2;
}
