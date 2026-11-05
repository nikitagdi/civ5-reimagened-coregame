/* End-to-end smoke test of gamecore_abi.h v2 over the real libgamecore.a (headless/tools/clang/smoke/smoke.sh).
 * Lifecycle on a real game: create -> new_game (setup JSON, game data read through read_file from --game DIR) -> pump until the
 * human is up or the turn changes -> state digest -> destroy; plus argument errors, the one-instance / one-game rules, buffers and
 * the queries (GCQ1: Q_GAME, Q_PLOTS with the fog filter and river bits, Q_PLAYERS, Q_CATALOG) and the staged entries (load_game, save, apply_command, lua_register). Usage: smoke --game GAMEDATA_DIR */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gamecore_abi.h"

static int g_failures = 0;
static const char* g_game_dir = NULL;
static int g_log_errors = 0;
static int g_turn_events = 0;
static int g_last_turn_event = -1;
static int g_game_over_events = 0;
static int g_other_events = 0;

/* v2 record and struct sizes (the header is copied to gamecore-sys; a size drift breaks the Rust side). */
#define SIZE_CHECK(type, size) typedef char smoke_size_check_##type[(sizeof(type) == (size)) ? 1 : -1]
SIZE_CHECK(gc_error, 264);
SIZE_CHECK(gc_pump_result, 24);
SIZE_CHECK(gcq_game, 56);
SIZE_CHECK(gcq_player, 48);
SIZE_CHECK(gcq_path_node, 12);
SIZE_CHECK(gcq_catalog_timer, 24);
SIZE_CHECK(gcq_turn, 24);
SIZE_CHECK(gcq_diplo, 8);
SIZE_CHECK(gcq_tree_node, 16);
SIZE_CHECK(gcq_plot_basis, 16);
SIZE_CHECK(gcq_combat_preview, 20);
SIZE_CHECK(gcq_plot_dirty, 4);

#define CHECK(cond, what)                                              \
	do {                                                               \
		if (cond) printf("  ok   %s\n", what);                        \
		else { printf("  FAIL %s (%s:%d)\n", what, __FILE__, __LINE__); ++g_failures; } \
	} while (0)

static void host_log(void* user, int32_t level, const char* msg)
{
	static const char* names[] = {"error", "warn", "info", "debug", "trace"};
	(void)user;
	if (level == GC_LOG_ERROR) ++g_log_errors;
	if (level <= GC_LOG_WARN) printf("  [%s] %s\n", (level >= 0 && level <= 4) ? names[level] : "?", msg);
}

static void release_malloc(gc_buf* b) { free(b->data); }

static int32_t host_read_file(void* user, const char* path, gc_buf* out)
{
	char full[1024];
	FILE* f;
	long n;
	uint8_t* data;
	(void)user;
	if (!g_game_dir) return GC_ERR_NOT_FOUND;
	if (snprintf(full, sizeof(full), "%s/%s", g_game_dir, path) >= (int)sizeof(full)) return GC_ERR_INVALID_ARG;
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

/* F7: a copy of the events 2-10 (the pointers are valid only during the callback). */
#define EV_MAX_LIVE 512
static int g_ev_count[GC_EVENT_PLAYER_TURN_STATUS + 1];
static int g_ev_live[EV_MAX_LIVE]; /* ids of the notifications added and not yet removed, by the events */
static int g_ev_live_n = 0;
static int g_ev_first_player = -1;
static int g_ev_bad = 0; /* events that broke the contract (argc, NULL args, text on an event without text, ...) */
static char g_ev_first_text[512], g_ev_first_text2[512];
static int g_ev_first_id = -1;
static int g_ev_text_ok = 0; /* NOTIFICATION_ADDED events with non-empty text and text2 */
static int g_ev_popup_text_null = 0;
static int g_ac_bad = 0;       /* AUTO_CHOICE events without kind 0..4, a player, an item >= 0 or a text */
static char g_ac_first_text[256];

static void ev_record(const gc_event* ev)
{
	int i;
	if (ev->kind >= 1 && ev->kind <= GC_EVENT_PLAYER_TURN_STATUS) ++g_ev_count[ev->kind];
	else ++g_ev_bad;
	switch (ev->kind) {
	case GC_EVENT_PLAYER_TURN_STARTED:
		if (ev->argc != 1 || !ev->args) ++g_ev_bad;
		else if (g_ev_first_player < 0) g_ev_first_player = ev->args[0];
		break;
	case GC_EVENT_NOTIFICATION_ADDED:
		if (ev->argc != 7 || !ev->args) { ++g_ev_bad; break; } /* v2: args end with the player */
		if (ev->text && ev->text[0] && ev->text2 && ev->text2[0]) ++g_ev_text_ok;
		if (g_ev_first_id < 0) {
			g_ev_first_id = ev->args[0];
			snprintf(g_ev_first_text, sizeof(g_ev_first_text), "%s", ev->text ? ev->text : "");
			snprintf(g_ev_first_text2, sizeof(g_ev_first_text2), "%s", ev->text2 ? ev->text2 : "");
		}
		if (g_ev_live_n < EV_MAX_LIVE) g_ev_live[g_ev_live_n++] = ev->args[0];
		break;
	case GC_EVENT_NOTIFICATION_REMOVED:
		if (ev->argc != 2 || !ev->args) { ++g_ev_bad; break; } /* v2: id, player */
		for (i = 0; i < g_ev_live_n; ++i)
			if (g_ev_live[i] == ev->args[0]) { g_ev_live[i] = g_ev_live[--g_ev_live_n]; break; }
		break;
	case GC_EVENT_AUTO_CHOICE:
		if (ev->argc != 3 || !ev->args || ev->args[0] < 0 || ev->args[0] > 4 || ev->args[1] < 0 || ev->args[2] < 0 || !ev->text || !ev->text[0] || ev->text2) ++g_ac_bad;
		else if (g_ac_first_text[0] == '\0') snprintf(g_ac_first_text, sizeof(g_ac_first_text), "%s", ev->text);
		break;
	case GC_EVENT_POPUP_INFO:
		if (ev->argc != 5 || !ev->args) ++g_ev_bad; /* v2: ..., player */
		if (!ev->text) ++g_ev_popup_text_null;
		break;
	case GC_EVENT_DIPLO_AUTO_DECLINED:
		if (ev->argc != 3 || !ev->args || !ev->text || !ev->text2) ++g_ev_bad;
		break;
	case GC_EVENT_WAR_STATE:
		if (ev->argc != 3 || !ev->args) ++g_ev_bad;
		break;
	case GC_EVENT_LUA_ERROR:
		if (!ev->text) ++g_ev_bad;
		break;
	case GC_EVENT_PLAYER_TURN_STATUS:
		if (ev->argc != 3 || !ev->args || ev->args[0] < 0 || ev->args[0] > 1) ++g_ev_bad;
		break;
	case GC_EVENT_MESSAGE:
		if (ev->argc != 6 || !ev->args || ev->args[4] < 0 || ev->args[4] > 2 || !ev->text || !ev->text[0] || ev->text2) ++g_ev_bad;
		break;
	default: break;
	}
}

static void host_event(void* user, const gc_event* ev)
{
	(void)user;
	ev_record(ev);
	if (ev->kind == GC_EVENT_TURN_STARTED && ev->argc == 1 && ev->args) { ++g_turn_events; g_last_turn_event = ev->args[0]; }
	else if (ev->kind == GC_EVENT_GAME_OVER) ++g_game_over_events;
	else ++g_other_events;
}

static int32_t host_lua_fn(void* user, struct lua_State* L) { (void)user; (void)L; return 0; }

static void release_static(gc_buf* b) { b->owner = (void*)1; }

/* ---- GCQ1 reply checks ---- */
static const int k_dir_dx[6] = {0, 1, 1, 0, -1, -1}; /* hex-space steps per DirectionType: NE E SE SW W NW */
static const int k_dir_dy[6] = {1, 0, -1, -1, 0, 1};

/* Header, sizes and string table of a reply; returns 1 when the structure is sound. */
static int gcq_check(const gc_buf* b, int kind, uint32_t min_rec, const char* what, gcq_header* h)
{
	char msg[200];
	int ok = 1;
	memset(h, 0, sizeof(*h));
	if (!b->data || b->len < sizeof(gcq_header)) { printf("  FAIL %s: reply shorter than the header\n", what); ++g_failures; return 0; }
	memcpy(h, b->data, sizeof(*h));
	if (memcmp(h->magic, "GCQ1", 4) != 0) ok = 0;
	if (h->format != 1 || h->kind != (uint16_t)kind) ok = 0;
	if (h->record_size < min_rec || (h->record_size & 3u) != 0) ok = 0;
	if ((uint64_t)h->strings_offset != (uint64_t)sizeof(gcq_header) + (uint64_t)h->count * h->record_size) ok = 0;
	if ((uint64_t)h->strings_offset + h->strings_size != (uint64_t)b->len) ok = 0;
	if (ok && h->strings_size > 0 && b->data[b->len - 1] != 0) ok = 0;
	sprintf(msg, "%s: GCQ1 header (magic, format 1, kind %d, record_size %u >= %u, offsets and length consistent)", what, kind, (unsigned)h->record_size, (unsigned)min_rec);
	CHECK(ok, msg);
	return ok;
}

static void gcq_rec(const gc_buf* b, const gcq_header* h, uint32_t i, void* dst, size_t n)
{
	memset(dst, 0, n);
	memcpy(dst, b->data + sizeof(gcq_header) + (size_t)i * h->record_size, n < h->record_size ? n : h->record_size);
}

/* A string reference is GCQ_NO_STRING or points at a NUL-terminated string inside the table. */
static int gcq_str_ok(const gc_buf* b, const gcq_header* h, uint32_t off)
{
	if (off == GCQ_NO_STRING) return 1;
	if (off >= h->strings_size) return 0;
	return memchr(b->data + h->strings_offset + off, 0, h->strings_size - off) != NULL;
}

static const char* gcq_str(const gc_buf* b, const gcq_header* h, uint32_t off)
{
	return off == GCQ_NO_STRING ? "" : (const char*)(b->data + h->strings_offset + off);
}

/* The neighbour of (x, y) towards direction d on a w x h map (Civ5 hex storage: odd rows shifted), or 0 when off the map. */
static int gcq_neighbour(int w, int h, int wrap_x, int wrap_y, int x, int y, int d, int* nx, int* ny)
{
	int hx = x - (y >> 1);
	int yy = y + k_dir_dy[d];
	int xx;
	hx += k_dir_dx[d];
	if (yy < 0 || yy >= h) { if (!wrap_y) return 0; yy = (yy + h) % h; }
	xx = hx + (yy >> 1);
	if (xx < 0 || xx >= w) { if (!wrap_x) return 0; xx = ((xx % w) + w) % w; }
	*nx = xx;
	*ny = yy;
	return 1;
}

static void run_query_checks(gc_instance* inst, const gc_pump_result* pr)
{
	static const uint8_t zeros[sizeof(gcq_plot)] = {0};
	gc_error err;
	gc_buf bg = {0, 0, 0, 0}, bp = {0, 0, 0, 0}, ba = {0, 0, 0, 0}, bm = {0, 0, 0, 0}, bc = {0, 0, 0, 0}, bc2 = {0, 0, 0, 0};
	gcq_header hg, hp, ha, hm, hc, hc2;
	gcq_game g;
	gcq_plot pa, pb;
	gcq_player pl, nxt;
	gcq_catalog ce;
	int32_t viewer[1], args[1];
	uint32_t i, k, revealed, visible, total, rivers, river_bad, unrev_bad, unrev_n, fog_bad, order_bad, vis_flag_bad;
	int w, h, wrap_x, wrap_y, nx, ny, d, humans, majors, barbs, met_n, unknown_bad, id_bad, str_bad, colors_bad;
	int terrain_max, terrain_catalog_max;

	printf("-- queries\n");
	memset(&err, 0, sizeof(err));

	/* argument errors leave the buffer alone */
	bg.data = (uint8_t*)&err;
	CHECK(gc_query(inst, 999, NULL, 0, &bg, &err) == GC_ERR_INVALID_ARG && err.message[0] != '\0' && bg.data == (uint8_t*)&err, "unknown query kind -> INVALID_ARG with a message, buffer untouched");
	bg.data = NULL;
	{
		int32_t two[2] = {0, 0};
		CHECK(gc_query(inst, GC_Q_GAME, two, 2, &bg, &err) == GC_ERR_INVALID_ARG, "Q_GAME with two arguments -> INVALID_ARG");
		args[0] = -2;
		CHECK(gc_query(inst, GC_Q_GAME, args, 1, &bg, &err) == GC_ERR_INVALID_ARG, "Q_GAME player -2 -> INVALID_ARG");
		CHECK(gc_query(inst, GC_Q_TURN, two, 2, &bg, &err) == GC_ERR_INVALID_ARG, "Q_TURN with arguments -> INVALID_ARG");
	}
	CHECK(gc_query(inst, GC_Q_PLOTS, NULL, 0, &bg, &err) == GC_ERR_INVALID_ARG, "Q_PLOTS without viewer_team -> INVALID_ARG");
	args[0] = -2;
	CHECK(gc_query(inst, GC_Q_PLOTS, args, 1, &bg, &err) == GC_ERR_INVALID_ARG, "Q_PLOTS viewer -2 -> INVALID_ARG");
	args[0] = 100000;
	CHECK(gc_query(inst, GC_Q_PLAYERS, args, 1, &bg, &err) == GC_ERR_INVALID_ARG, "Q_PLAYERS viewer 100000 -> INVALID_ARG");
	args[0] = GC_CATALOG_TABLE_COUNT_V4;
	CHECK(gc_query(inst, GC_Q_CATALOG, args, 1, &bg, &err) == GC_ERR_INVALID_ARG, "Q_CATALOG table 25 -> INVALID_ARG");
	args[0] = GC_CATALOG_TURN_TIMERS;
	{
		gcq_header htt;
		gcq_catalog_timer tr;
		int n_t, has_medium = 0;
		CHECK(gc_query(inst, GC_Q_CATALOG, args, 1, &bg, &err) == GC_OK, "Q_CATALOG table 15 (TurnTimers) answers");
		if (gcq_check(&bg, GC_Q_CATALOG, sizeof(gcq_catalog_timer), "Q_CATALOG(TurnTimers)", &htt)) {
			for (n_t = 0; n_t < (int)htt.count; ++n_t) {
				gcq_rec(&bg, &htt, (uint32_t)n_t, &tr, sizeof(tr));
				if (strcmp(gcq_str(&bg, &htt, tr.type), "TURNTIMER_MEDIUM") == 0 && tr.base_time > 0) has_medium = 1;
			}
			printf("  TurnTimers: %u row(s)\n", (unsigned)htt.count);
			CHECK(htt.count > 0 && has_medium, "Q_CATALOG(TurnTimers): rows with TURNTIMER_MEDIUM and a base time");
		}
		gc_buf_release(&bg);
	}
	args[0] = GC_CATALOG_WORLD_SIZES;
	{
		gcq_header hw;
		gcq_catalog_world_size wr;
		int n_w, has_duel = 0, has_huge = 0;
		CHECK(gc_query(inst, GC_Q_CATALOG, args, 1, &bg, &err) == GC_OK, "Q_CATALOG table 17 (Worlds) answers");
		if (gcq_check(&bg, GC_Q_CATALOG, sizeof(gcq_catalog_world_size), "Q_CATALOG(Worlds)", &hw)) {
			for (n_w = 0; n_w < (int)hw.count; ++n_w) {
				const char* t;
				gcq_rec(&bg, &hw, (uint32_t)n_w, &wr, sizeof(wr));
				t = gcq_str(&bg, &hw, wr.type);
				if (strcmp(t, "WORLDSIZE_DUEL") == 0 && wr.width == 40 && wr.height == 24 && wr.default_players == 2) has_duel = 1;
				if (strcmp(t, "WORLDSIZE_HUGE") == 0 && wr.width == 128 && wr.height == 80) has_huge = 1;
			}
			printf("  Worlds: %u row(s)\n", (unsigned)hw.count);
			CHECK(hw.count >= 2 && has_duel && has_huge, "Q_CATALOG(Worlds): DUEL 40x24 and HUGE 128x80 with player counts");
		}
		gc_buf_release(&bg);
	}
	args[0] = GC_CATALOG_GAME_SPEEDS;
	{
		gcq_header hs;
		gcq_catalog sr;
		int n_s, has_standard = 0;
		CHECK(gc_query(inst, GC_Q_CATALOG, args, 1, &bg, &err) == GC_OK, "Q_CATALOG table 18 (GameSpeeds) answers");
		if (gcq_check(&bg, GC_Q_CATALOG, sizeof(gcq_catalog), "Q_CATALOG(GameSpeeds)", &hs)) {
			for (n_s = 0; n_s < (int)hs.count; ++n_s) {
				gcq_rec(&bg, &hs, (uint32_t)n_s, &sr, sizeof(sr));
				if (strcmp(gcq_str(&bg, &hs, sr.type), "GAMESPEED_STANDARD") == 0) has_standard = 1;
			}
			printf("  GameSpeeds: %u row(s)\n", (unsigned)hs.count);
			CHECK(hs.count > 0 && has_standard, "Q_CATALOG(GameSpeeds): rows with GAMESPEED_STANDARD");
		}
		gc_buf_release(&bg);
	}
	{
		gc_buf bid = {0, 0, 0, 0};
		CHECK(gc_build_id(inst, &bid, &err) == GC_OK && bid.len > 16 && memcmp(bid.data, "lekmod-portable/", 16) == 0, "gc_build_id: lekmod-portable/... string");
		if (bid.data) printf("  build id: %.*s\n", (int)bid.len, (const char*)bid.data);
		gc_buf_release(&bid);
		CHECK(gc_build_id(NULL, &bid, &err) == GC_ERR_INVALID_ARG, "gc_build_id(NULL) -> INVALID_ARG");
	}
	args[0] = 0;
	CHECK(gc_query(inst, GC_Q_CITIES, NULL, 0, &bg, &err) == GC_ERR_INVALID_ARG && bg.data == NULL, "Q_CITIES without viewer_team -> INVALID_ARG");
	CHECK(gc_query(inst, GC_Q_UNITS, NULL, 0, &bg, &err) == GC_ERR_INVALID_ARG, "Q_UNITS without viewer_team -> INVALID_ARG");
	args[0] = 100000;
	CHECK(gc_query(inst, GC_Q_UNITS, args, 1, &bg, &err) == GC_ERR_INVALID_ARG, "Q_UNITS viewer 100000 -> INVALID_ARG");
	args[0] = 0;

	/* Q_GAME */
	CHECK(gc_query(inst, GC_Q_GAME, NULL, 0, &bg, &err) == GC_OK && err.code == GC_OK, "Q_GAME");
	if (!gcq_check(&bg, GC_Q_GAME, sizeof(gcq_game), "Q_GAME", &hg) || hg.count != 1) { CHECK(0, "Q_GAME has one record"); return; }
	gcq_rec(&bg, &hg, 0, &g, sizeof(g));
	printf("  game: turn %d year %d map %dx%d wrap %d/%d human %d team %d status %d blocker %d (unit %d city %d at %d,%d) difficulty %d winner %d\n", (int)g.turn,
		(int)g.year, (int)g.map_width, (int)g.map_height, (int)g.wrap_x, (int)g.wrap_y, (int)g.human_player, (int)g.human_team, (int)g.status, (int)g.blocker,
		(int)g.blocker_unit, (int)g.blocker_city, (int)g.blocker_x, (int)g.blocker_y, (int)g.difficulty, (int)g.winner);
	CHECK(hg.strings_size == 0, "Q_GAME has an empty string table");
	CHECK(g.turn == pr->turn && g.status == pr->status && g.active_player == pr->active_player, "Q_GAME turn / status / active player match the last gc_pump");
	CHECK(g.blocker == pr->blocker, "Q_GAME blocker matches the pump's blocker");
	CHECK(hg.record_size == sizeof(gcq_game) && sizeof(gcq_game) == 56, "Q_GAME record is the 56-byte v2 record (v2 fields: game_type, turn_mode, phase, humans, humans_left, timer, safe point)");
	CHECK(g.map_width > 0 && g.map_height > 0 && g.human_player >= 0 && g.human_team >= 0 && g.winner == -1, "Q_GAME: map size, human slot and team set, no winner");
	CHECK(g.blocker_x < g.map_width && g.blocker_y < g.map_height && g.blocker_x >= -1 && g.blocker_y >= -1, "Q_GAME blocker plot is -1 or on the map");
	CHECK(g.game_type == 0 && g.turn_mode == 0 && g.turn_len_s == 0 && g.turn_base_s == 0, "Q_GAME v2: single game type, no turn mode, no timer");
	CHECK(g.humans_left == (pr->status == GC_PUMP_WAITING_HUMAN ? 1 : 0) && (g.safe_point == 0 || g.safe_point == 1) && (g.safe_point == 1) == (g.unsafe_reason == GC_UNSAFE_NONE), "Q_GAME v2: humans_left and the safe point verdict are consistent");
	{
		gc_buf bt = {0, 0, 0, 0};
		gcq_header ht;
		CHECK(gc_query(inst, GC_Q_TURN, NULL, 0, &bt, &err) == GC_OK, "Q_TURN");
		CHECK(gcq_check(&bt, GC_Q_TURN, sizeof(gcq_turn), "Q_TURN", &ht) && ht.record_size == 24, "Q_TURN: GCQ1 reply with 24-byte records");
		gc_buf_release(&bt);
	}
	w = g.map_width;
	h = g.map_height;
	wrap_x = g.wrap_x;
	wrap_y = g.wrap_y;

	/* Q_PLOTS for the human's team, and the whole truth */
	viewer[0] = g.human_team;
	CHECK(gc_query(inst, GC_Q_PLOTS, viewer, 1, &bp, &err) == GC_OK, "Q_PLOTS(human team)");
	if (!gcq_check(&bp, GC_Q_PLOTS, sizeof(gcq_plot), "Q_PLOTS(human team)", &hp)) return;
	total = (uint32_t)w * (uint32_t)h;
	CHECK(hp.count == total, "Q_PLOTS count = map width x height");
	CHECK(hp.strings_size == 0, "Q_PLOTS has an empty string table");
	viewer[0] = -1;
	CHECK(gc_query(inst, GC_Q_PLOTS, viewer, 1, &ba, &err) == GC_OK, "Q_PLOTS(-1)");
	if (!gcq_check(&ba, GC_Q_PLOTS, sizeof(gcq_plot), "Q_PLOTS(-1)", &ha) || ha.count != total || hp.count != total) { CHECK(0, "Q_PLOTS(-1) count"); return; }

	revealed = visible = unrev_bad = unrev_n = fog_bad = order_bad = vis_flag_bad = 0;
	terrain_max = 0;
	for (i = 0; i < total; ++i) {
		gcq_rec(&bp, &hp, i, &pa, sizeof(pa));
		gcq_rec(&ba, &ha, i, &pb, sizeof(pb));
		if (pa.x != (int)(i % (uint32_t)w) || pa.y != (int)(i / (uint32_t)w) || pb.x != pa.x || pb.y != pa.y) ++order_bad;
		if (!(pb.flags & GC_PLOT_F_REVEALED) || !(pb.flags & GC_PLOT_F_VISIBLE)) ++vis_flag_bad;
		if (pb.terrain > terrain_max) terrain_max = pb.terrain;
		if (pa.flags & GC_PLOT_F_REVEALED) {
			++revealed;
			if (pa.flags & GC_PLOT_F_VISIBLE) ++visible;
			/* a revealed plot shows the same terrain as the truth */
			if (pa.terrain != pb.terrain || pa.feature != pb.feature || pa.plot_type != pb.plot_type || pa.continent_art != pb.continent_art) ++fog_bad;
			if ((pa.flags & GC_PLOT_F_VISIBLE) == 0) {
				/* fogged: no live data */
				int j, y_nonzero = 0;
				for (j = 0; j < 6; ++j) if (pa.yields[j] != 0) y_nonzero = 1;
				if ((pa.flags & GC_PLOT_F_PILLAGED) || y_nonzero) ++fog_bad;
			} else if (pa.flags != pb.flags) {
				++fog_bad; /* a visible plot reads like the truth */
			}
		} else {
			++unrev_n;
			/* nothing but x and y */
			if (pa.flags != 0 || memcmp((const uint8_t*)&pa + 4, zeros + 4, sizeof(pa) - 4) != 0) ++unrev_bad;
		}
	}
	printf("  plots %u: revealed %u (visible %u), unrevealed %u for team %d\n", (unsigned)total, (unsigned)revealed, (unsigned)visible, (unsigned)unrev_n, (int)g.human_team);
	CHECK(order_bad == 0, "Q_PLOTS records come by rows y, then x, with x,y in both replies");
	CHECK(revealed > 0 && revealed < total, "fog filter: 0 < revealed < total for the human's team");
	CHECK(visible > 0 && visible <= revealed, "fog filter: some plots are visible, all of them revealed");
	CHECK(unrev_n > 0 && unrev_bad == 0, "an unrevealed plot carries no flags, terrain, owner or other data");
	CHECK(fog_bad == 0, "revealed plots read like the truth (terrain, feature, continent art); fogged plots have no live data; visible plots equal the truth");
	CHECK(vis_flag_bad == 0, "viewer -1: every plot revealed and visible");

	/* river bits: edge d of A <=> edge (d+3)%6 of the neighbour, wrap included */
	rivers = river_bad = 0;
	for (i = 0; i < total; ++i) {
		gcq_rec(&ba, &ha, i, &pa, sizeof(pa));
		for (d = 0; d < 6; ++d) {
			int has = (pa.flags >> (GC_PLOT_F_RIVER_SHIFT + d)) & 1;
			if (has) ++rivers;
			if (gcq_neighbour(w, h, wrap_x, wrap_y, pa.x, pa.y, d, &nx, &ny)) {
				gcq_rec(&ba, &ha, (uint32_t)ny * (uint32_t)w + (uint32_t)nx, &pb, sizeof(pb));
				if (has != (int)((pb.flags >> (GC_PLOT_F_RIVER_SHIFT + (d + 3) % 6)) & 1)) ++river_bad;
			}
		}
	}
	printf("  river edge bits set: %u (each river edge is counted from both tiles)\n", (unsigned)rivers);
	CHECK(rivers > 0, "the map has rivers");
	CHECK(river_bad == 0, "river bits are consistent between neighbours (edge d of A <=> edge opposite(d) of the neighbour)");
	{
		uint32_t arts = 0, water = 0, land = 0;
		for (i = 0; i < total; ++i) {
			gcq_rec(&ba, &ha, i, &pa, sizeof(pa));
			if (pa.continent_art > 0) ++arts;
			if (pa.plot_type == GC_PLOT_TYPE_OCEAN) ++water; else ++land;
		}
		printf("  truth: land %u water %u, plots with continent_art > 0: %u\n", (unsigned)land, (unsigned)water, (unsigned)arts);
		CHECK(land > 0 && water > 0, "both land and water plots exist");
	}

	/* Q_PLAYERS */
	viewer[0] = g.human_team;
	CHECK(gc_query(inst, GC_Q_PLAYERS, viewer, 1, &bm, &err) == GC_OK, "Q_PLAYERS(human team)");
	if (gcq_check(&bm, GC_Q_PLAYERS, sizeof(gcq_player), "Q_PLAYERS", &hm)) {
		humans = majors = barbs = met_n = unknown_bad = id_bad = str_bad = colors_bad = 0;
		for (k = 0; k < hm.count; ++k) {
			gcq_rec(&bm, &hm, k, &pl, sizeof(pl));
			if (!gcq_str_ok(&bm, &hm, pl.name) || !gcq_str_ok(&bm, &hm, pl.civ_name) || pl.name == GCQ_NO_STRING) ++str_bad;
			if (pl.flags & GC_PLAYER_F_HUMAN) ++humans;
			if (pl.flags & GC_PLAYER_F_BARBARIAN) ++barbs;
			if (!(pl.flags & (GC_PLAYER_F_BARBARIAN | GC_PLAYER_F_MINOR))) {
				++majors;
				if (pl.color_primary == 0) ++colors_bad;
			}
			if (pl.flags & GC_PLAYER_F_MET) ++met_n;
			else if (pl.civ_type != -1 || pl.leader_type != -1 || strcmp(gcq_str(&bm, &hm, pl.name), "Unknown") != 0) ++unknown_bad;
			if (pl.id == g.human_player && (!(pl.flags & GC_PLAYER_F_MET) || !(pl.flags & GC_PLAYER_F_HUMAN) || pl.team != g.human_team)) ++id_bad;
			if (k + 1 < hm.count) {
				gcq_rec(&bm, &hm, k + 1, &nxt, sizeof(nxt));
				if (nxt.id <= pl.id) ++id_bad;
			}
			printf("  player %d team %d civ %d leader %d flags %04x colors %08x/%08x '%s' / '%s'\n", (int)pl.id, (int)pl.team, (int)pl.civ_type, (int)pl.leader_type, (unsigned)pl.flags,
				(unsigned)pl.color_primary, (unsigned)pl.color_secondary, gcq_str(&bm, &hm, pl.name), gcq_str(&bm, &hm, pl.civ_name));
		}
		CHECK(humans == 1 && barbs == 1, "Q_PLAYERS: one human, one barbarian slot");
		CHECK(majors == 2, "Q_PLAYERS: 2 major civs (human + ai_count 1)");
		CHECK(hm.count > 3, "Q_PLAYERS: city-states are listed too");
		CHECK(id_bad == 0 && str_bad == 0, "Q_PLAYERS: ids ascend, the human is met / human / on the viewer's team, name strings are inside the table");
		CHECK(unknown_bad == 0 && met_n >= 1, "Q_PLAYERS: players the viewer has not met are 'Unknown' with civ and leader -1");
		CHECK(colors_bad == 0, "Q_PLAYERS: major civs have colors");
	}
	viewer[0] = -1;
	CHECK(gc_query(inst, GC_Q_PLAYERS, viewer, 1, &bc, &err) == GC_OK, "Q_PLAYERS(-1)");
	if (gcq_check(&bc, GC_Q_PLAYERS, sizeof(gcq_player), "Q_PLAYERS(-1)", &hc)) {
		unknown_bad = 0;
		for (k = 0; k < hc.count; ++k) {
			gcq_rec(&bc, &hc, k, &pl, sizeof(pl));
			if (!(pl.flags & GC_PLAYER_F_MET) || pl.civ_type < 0) ++unknown_bad;
		}
		CHECK(unknown_bad == 0 && hc.count == hm.count, "Q_PLAYERS(-1): everybody is known; same slots as for the human");
	}
	gc_buf_release(&bc);

	/* Q_CATALOG: every table, ids and types stable between two calls */
	{
		static const char* const names[GC_CATALOG_TABLE_COUNT] = {"Units", "Buildings", "Projects", "Technologies", "Policies", "Terrains", "Features", "Resources",
			"Improvements", "Routes", "Civilizations", "Leaders", "Eras", "Handicaps", "Promotions"};
		char msg[200];
		int t, bad_tables = 0, stable_bad = 0, found_settler = 0, grass = 0;
		uint32_t terrains = 0, with_icon;
		terrain_catalog_max = -1;
		for (t = 0; t < GC_CATALOG_TABLE_COUNT; ++t) {
			int bad = 0, prev = -1;
			uint32_t named = 0;
			with_icon = 0;
			args[0] = t;
			if (gc_query(inst, GC_Q_CATALOG, args, 1, &bc, &err) != GC_OK || gc_query(inst, GC_Q_CATALOG, args, 1, &bc2, &err) != GC_OK) {
				printf("  FAIL catalog %s: %s\n", names[t], err.message);
				++bad_tables;
				gc_buf_release(&bc);
				continue;
			}
			sprintf(msg, "Q_CATALOG(%s)", names[t]);
			if (!gcq_check(&bc, GC_Q_CATALOG, sizeof(gcq_catalog), msg, &hc) || !gcq_check(&bc2, GC_Q_CATALOG, sizeof(gcq_catalog), msg, &hc2)) {
				++bad_tables;
				gc_buf_release(&bc);
				gc_buf_release(&bc2);
				continue;
			}
			if (hc.count == 0) bad = 1;
			if (bc.len != bc2.len || memcmp(bc.data, bc2.data, bc.len) != 0) ++stable_bad;
			for (k = 0; k < hc.count; ++k) {
				gcq_rec(&bc, &hc, k, &ce, sizeof(ce));
				if (ce.id <= prev) bad = 1;
				prev = ce.id;
				if (!gcq_str_ok(&bc, &hc, ce.type) || !gcq_str_ok(&bc, &hc, ce.name) || !gcq_str_ok(&bc, &hc, ce.art_define) || !gcq_str_ok(&bc, &hc, ce.icon_atlas) ||
					!gcq_str_ok(&bc, &hc, ce.flag_atlas) || !gcq_str_ok(&bc, &hc, ce.art_style) || !gcq_str_ok(&bc, &hc, ce.strategic_view)) { bad = 1; continue; }
				if (ce.type == GCQ_NO_STRING || gcq_str(&bc, &hc, ce.type)[0] == 0) bad = 1;
				if (gcq_str(&bc, &hc, ce.name)[0] != 0) ++named;
				if (ce.icon_atlas != GCQ_NO_STRING) ++with_icon;
				if (ce.strategic_view != GCQ_NO_STRING) bad = 1;
				if (t == GC_CATALOG_UNITS && strcmp(gcq_str(&bc, &hc, ce.type), "UNIT_SETTLER") == 0) found_settler = 1;
				if (t == GC_CATALOG_TERRAINS) {
					++terrains;
					if (ce.id > terrain_catalog_max) terrain_catalog_max = ce.id;
					if (ce.id == 0 && strcmp(gcq_str(&bc, &hc, ce.type), "TERRAIN_GRASS") == 0) grass = 1;
				}
			}
			printf("  catalog %-13s %4u entries, %4u named, %4u with an icon atlas\n", names[t], (unsigned)hc.count, (unsigned)named, (unsigned)with_icon);
			if (named == 0) bad = 1;
			if (bad) ++bad_tables;
			gc_buf_release(&bc);
			gc_buf_release(&bc2);
		}
		CHECK(bad_tables == 0, "Q_CATALOG: all 15 tables non-empty, ids ascending, types and strings inside the table, names localized");
		CHECK(stable_bad == 0, "Q_CATALOG: two calls return identical bytes (stable ids)");
		CHECK(found_settler, "Q_CATALOG(Units) lists UNIT_SETTLER");
		CHECK(grass && terrains >= 5, "Q_CATALOG(Terrains) lists TERRAIN_GRASS as id 0");
		CHECK(terrain_max <= terrain_catalog_max, "every terrain id on the map is in the Terrains catalog range");
	}
	gc_buf_release(&bg);
	gc_buf_release(&bp);
	gc_buf_release(&ba);
	gc_buf_release(&bm);
	CHECK(bg.data == NULL && bp.len == 0, "query buffers released");
}

/* ---- commands and unit queries on the live game ---- */

#define MAX_UNITS 128

static int hex_dist(int x1, int y1, int x2, int y2)
{
	int dq = (x2 - (y2 >> 1)) - (x1 - (y1 >> 1)), dr = y2 - y1, ds = dq + dr;
	if (dq < 0) dq = -dq;
	if (dr < 0) dr = -dr;
	if (ds < 0) ds = -ds;
	return dq > dr ? (dq > ds ? dq : ds) : (dr > ds ? dr : ds);
}

/* Q_UNITS(viewer) into us[]; returns the number of units (-1 on failure). */
static int read_units(gc_instance* inst, int viewer, gcq_unit* us, int max, const char* what)
{
	gc_buf b = {0, 0, 0, 0};
	gcq_header h;
	gc_error err;
	int32_t a[1];
	uint32_t i;
	int n;
	a[0] = viewer;
	if (gc_query(inst, GC_Q_UNITS, a, 1, &b, &err) != GC_OK) { printf("  FAIL %s: %s\n", what, err.message); ++g_failures; return -1; }
	if (!gcq_check(&b, GC_Q_UNITS, sizeof(gcq_unit), what, &h)) { gc_buf_release(&b); return -1; }
	n = (int)h.count < max ? (int)h.count : max;
	for (i = 0; i < (uint32_t)n; ++i) gcq_rec(&b, &h, i, &us[i], sizeof(gcq_unit));
	gc_buf_release(&b);
	return n;
}

static const gcq_unit* unit_of(const gcq_unit* us, int n, int owner, int id)
{
	int i;
	for (i = 0; i < n; ++i) if (us[i].owner == owner && us[i].id == id) return &us[i];
	return NULL;
}

/* Pumps until the human is up again (the queued command is delivered and its missions have run). */
static int pump_to_human(gc_instance* inst, gc_pump_result* pr)
{
	gc_error err;
	int calls;
	for (calls = 0; calls < 400; ++calls) {
		if (gc_pump(inst, 50, pr, &err) != GC_OK) { printf("  pump error: %d %s\n", err.code, err.message); return 0; }
		if (pr->status != GC_PUMP_AI_RUNNING) break;
	}
	return pr->status == GC_PUMP_WAITING_HUMAN;
}

static int32_t apply(gc_instance* inst, int kind, int player, int argc, int32_t a0, int32_t a1, int32_t a2, gc_error* err)
{
	int32_t args[3];
	gc_command c;
	args[0] = a0;
	args[1] = a1;
	args[2] = a2;
	memset(&c, 0, sizeof(c));
	c.kind = (uint16_t)kind;
	c.player = (uint8_t)player;
	c.argc = (uint32_t)argc;
	c.args = args;
	memset(err, 0, sizeof(*err));
	return gc_apply_command(inst, &c, err);
}

/* gc_apply_command must refuse with REJECTED and a message that contains `needle`. */
static void expect_rejected(gc_instance* inst, const char* what, int kind, int player, int argc, int32_t a0, int32_t a1, int32_t a2, const char* needle)
{
	gc_error err;
	char msg[300];
	int32_t rc = apply(inst, kind, player, argc, a0, a1, a2, &err);
	printf("  rejected: %s\n", err.message);
	sprintf(msg, "%s -> REJECTED naming \"%s\"", what, needle);
	CHECK(rc == GC_ERR_REJECTED && err.code == GC_ERR_REJECTED && strstr(err.message, needle) != NULL, msg);
}

/* While the AI is moving (before the first pump has finished) the human's commands are refused. */
static void check_not_humans_turn(gc_instance* inst, const char* when)
{
	gcq_unit us[MAX_UNITS];
	gc_buf bg = {0, 0, 0, 0};
	gcq_header hg;
	gcq_game g;
	gc_error err;
	char what[200];
	int n, i;
	if (gc_query(inst, GC_Q_GAME, NULL, 0, &bg, &err) != GC_OK || !gcq_check(&bg, GC_Q_GAME, sizeof(gcq_game), "Q_GAME", &hg)) { CHECK(0, "Q_GAME before the human is up"); return; }
	gcq_rec(&bg, &hg, 0, &g, sizeof(g));
	gc_buf_release(&bg);
	n = read_units(inst, g.human_team, us, MAX_UNITS, "Q_UNITS(human team) while the AI moves");
	for (i = 0; i < n; ++i) if (us[i].owner == g.human_player) break;
	if (i == n) { CHECK(0, "the human owns a unit before the first pump"); return; }
	sprintf(what, "SLEEP of the human's unit %s", when);
	expect_rejected(inst, what, GC_CMD_UNIT_SLEEP, g.human_player, 1, us[i].id, 0, 0, "not the human's turn");
	sprintf(what, "END_TURN %s", when);
	expect_rejected(inst, what, GC_CMD_END_TURN, g.human_player, 0, 0, 0, 0, "refused");
	sprintf(what, "SLEEP %s -> reason AI_RUNNING", when);
	CHECK(apply(inst, GC_CMD_UNIT_SLEEP, g.human_player, 1, us[i].id, 0, 0, &err) == GC_ERR_REJECTED && err.reason == GC_REASON_AI_RUNNING, what);
}

static int unit_type_id(gc_instance* inst, const char* type)
{
	gc_buf b = {0, 0, 0, 0};
	gcq_header h;
	gcq_catalog ce;
	gc_error err;
	int32_t a[1];
	uint32_t k;
	int found = -1;
	a[0] = GC_CATALOG_UNITS;
	if (gc_query(inst, GC_Q_CATALOG, a, 1, &b, &err) != GC_OK) return -1;
	if (gcq_check(&b, GC_Q_CATALOG, sizeof(gcq_catalog), "Q_CATALOG(Units)", &h)) {
		for (k = 0; k < h.count && found < 0; ++k) {
			gcq_rec(&b, &h, k, &ce, sizeof(ce));
			if (strcmp(gcq_str(&b, &h, ce.type), type) == 0) found = ce.id;
		}
	}
	gc_buf_release(&b);
	return found;
}

static void run_command_checks(gc_instance* inst, const gc_pump_result* pr0)
{
	gcq_unit us[MAX_UNITS], all[MAX_UNITS];
	gcq_game g;
	gcq_header hg, hp, ha, hn, hr;
	gcq_plot pl;
	gcq_path_node nd;
	gcq_unit_action ac;
	gc_buf bg = {0, 0, 0, 0}, bp = {0, 0, 0, 0}, ba = {0, 0, 0, 0}, bn = {0, 0, 0, 0}, br = {0, 0, 0, 0};
	gc_error err;
	gc_pump_result pr;
	int32_t a[4];
	char msg[300];
	int n, nall, i, k, human, team, w, h, settler_t, warrior_t, si = -1, wi = -1, sid, wid, wx, wy, sx, sy, tx = -1, ty = -1, path_n = 0, fog_bad, own_bad;
	int ex = -1, ey = -1;
	int found_action = 0, found_enabled = 0, move_enabled = 0, plain_ok = 0, tx_turn = 99, reach_has_target = 0, reach_bad = 0;
	uint32_t j;
	const gcq_unit* u;

	printf("-- commands and unit queries\n");
	if (gc_query(inst, GC_Q_GAME, NULL, 0, &bg, &err) != GC_OK || !gcq_check(&bg, GC_Q_GAME, sizeof(gcq_game), "Q_GAME", &hg)) { CHECK(0, "Q_GAME"); return; }
	gcq_rec(&bg, &hg, 0, &g, sizeof(g));
	gc_buf_release(&bg);
	human = g.human_player;
	team = g.human_team;
	w = g.map_width;
	h = g.map_height;
	pr = *pr0;
	CHECK(pr.status == GC_PUMP_WAITING_HUMAN, "the human is up");

	/* Q_UNITS: the human's units and the fog */
	n = read_units(inst, team, us, MAX_UNITS, "Q_UNITS(human team)");
	nall = read_units(inst, -1, all, MAX_UNITS, "Q_UNITS(-1)");
	if (n < 0 || nall < 0) return;
	settler_t = unit_type_id(inst, "UNIT_SETTLER");
	warrior_t = unit_type_id(inst, "UNIT_WARRIOR");
	for (i = 0; i < n; ++i) {
		printf("  unit %d/%d type %d at (%d,%d) hp %d/%d moves %d/%d str %d rng %d/%d flags %04x\n", us[i].owner, us[i].id, us[i].unit_type, us[i].x, us[i].y, us[i].hp,
			us[i].max_hp, us[i].moves_left, us[i].max_moves, us[i].strength, us[i].ranged, us[i].range, (unsigned)us[i].flags);
		if (us[i].owner == human && us[i].unit_type == settler_t && si < 0) si = i;
		if (us[i].owner == human && us[i].unit_type == warrior_t && wi < 0) wi = i;
	}
	CHECK(si >= 0 && wi >= 0, "Q_UNITS lists the human's settler and warrior");
	if (si < 0 || wi < 0) return;
	sid = us[si].id; sx = us[si].x; sy = us[si].y;
	wid = us[wi].id; wx = us[wi].x; wy = us[wi].y;
	CHECK((us[si].flags & GC_UNIT_F_CIVILIAN) && !(us[wi].flags & GC_UNIT_F_CIVILIAN) && us[wi].strength > 0 && us[wi].max_hp > 0 && us[wi].hp == us[wi].max_hp,
		"settler is a civilian, warrior has strength and full health");
	CHECK(us[wi].max_moves > 0 && us[wi].moves_left == us[wi].max_moves && (us[wi].flags & GC_UNIT_F_CAN_MOVE) && (us[wi].flags & GC_UNIT_F_NEEDS_ORDERS), "warrior has all its moves and needs orders");
	CHECK(nall >= n, "Q_UNITS(-1) lists at least as many units as Q_UNITS(human team)");
	/* fog: every unit of another team in the human's list is on a plot the human sees right now */
	a[0] = team;
	if (gc_query(inst, GC_Q_PLOTS, a, 1, &bp, &err) == GC_OK && gcq_check(&bp, GC_Q_PLOTS, sizeof(gcq_plot), "Q_PLOTS(human team)", &hp)) {
		fog_bad = own_bad = 0;
		for (i = 0; i < n; ++i) {
			if (us[i].owner != human) {
				gcq_rec(&bp, &hp, (uint32_t)us[i].y * (uint32_t)w + (uint32_t)us[i].x, &pl, sizeof(pl));
				if (!(pl.flags & GC_PLOT_F_VISIBLE)) ++fog_bad;
			}
		}
		for (i = 0; i < nall; ++i) {
			if (all[i].owner != human && !unit_of(us, n, all[i].owner, all[i].id)) {
				gcq_rec(&bp, &hp, (uint32_t)all[i].y * (uint32_t)w + (uint32_t)all[i].x, &pl, sizeof(pl));
				if (pl.flags & GC_PLOT_F_VISIBLE) ++own_bad; /* hidden although the plot is visible (not invisible units: none at the start) */
			}
		}
		printf("  units: %d visible to the human, %d in the whole game\n", n, nall);
		CHECK(fog_bad == 0, "Q_UNITS fog: foreign units listed for the human are on visible plots");
		CHECK(own_bad == 0, "Q_UNITS fog: a foreign unit on a visible plot is listed");
		CHECK(nall > n, "Q_UNITS fog: the AI's units out of sight are not in the human's list");
		gc_buf_release(&bp);
	}

	/* Q_UNIT_ACTIONS */
	a[0] = human; a[1] = sid;
	CHECK(gc_query(inst, GC_Q_UNIT_ACTIONS, a, 2, &bn, &err) == GC_OK, "Q_UNIT_ACTIONS(settler)");
	if (gcq_check(&bn, GC_Q_UNIT_ACTIONS, sizeof(gcq_unit_action), "Q_UNIT_ACTIONS(settler)", &hn)) {
		for (j = 0; j < hn.count; ++j) {
			gcq_rec(&bn, &hn, j, &ac, sizeof(ac));
			printf("  settler action %d enabled %d%s%s\n", ac.action, ac.enabled, ac.enabled ? "" : " reason: ", ac.enabled ? "" : gcq_str(&bn, &hn, ac.reason));
			if (ac.action == GC_CMD_UNIT_FOUND_CITY) { found_action = 1; found_enabled = ac.enabled; }
			if (ac.action == GC_CMD_UNIT_RANGE_ATTACK || ac.action == GC_CMD_UNIT_AUTOMATE_BUILD) plain_ok = -1;
			if (!gcq_str_ok(&bn, &hn, ac.reason) || (!ac.enabled && ac.reason == GCQ_NO_STRING)) plain_ok = -1;
		}
		CHECK(found_action && found_enabled, "Q_UNIT_ACTIONS(settler) lists found city, enabled");
		CHECK(plain_ok == 0, "Q_UNIT_ACTIONS(settler): no ranged attack / worker automation, every disabled action has a reason");
	}
	gc_buf_release(&bn);
	a[1] = wid;
	CHECK(gc_query(inst, GC_Q_UNIT_ACTIONS, a, 2, &bn, &err) == GC_OK, "Q_UNIT_ACTIONS(warrior)");
	found_action = 0;
	if (gcq_check(&bn, GC_Q_UNIT_ACTIONS, sizeof(gcq_unit_action), "Q_UNIT_ACTIONS(warrior)", &hn)) {
		for (j = 0; j < hn.count; ++j) {
			gcq_rec(&bn, &hn, j, &ac, sizeof(ac));
			printf("  warrior action %d enabled %d%s%s\n", ac.action, ac.enabled, ac.enabled ? "" : " reason: ", ac.enabled ? "" : gcq_str(&bn, &hn, ac.reason));
			if (ac.action == GC_CMD_UNIT_FOUND_CITY) found_action = 1;
			if (ac.action == GC_CMD_UNIT_MOVE_TO && ac.enabled) move_enabled = 1;
		}
		CHECK(!found_action && move_enabled, "Q_UNIT_ACTIONS(warrior): move is enabled, found city is not offered");
	}
	gc_buf_release(&bn);
	a[0] = human; a[1] = 999999;
	CHECK(gc_query(inst, GC_Q_UNIT_ACTIONS, a, 2, &bn, &err) == GC_ERR_INVALID_ARG && err.message[0] != '\0', "Q_UNIT_ACTIONS of a missing unit -> INVALID_ARG");
	CHECK(gc_query(inst, GC_Q_UNIT_ACTIONS, a, 1, &bn, &err) == GC_ERR_INVALID_ARG, "Q_UNIT_ACTIONS with one argument -> INVALID_ARG");

	/* Q_PATH: the warrior to a land plot two tiles away */
	a[0] = team;
	if (gc_query(inst, GC_Q_PLOTS, a, 1, &bp, &err) != GC_OK || !gcq_check(&bp, GC_Q_PLOTS, sizeof(gcq_plot), "Q_PLOTS(human team)", &hp)) { CHECK(0, "Q_PLOTS"); return; }
	for (k = 0; k < 2 && tx < 0; ++k) {
		for (j = 0; j < hp.count && tx < 0; ++j) {
			gcq_rec(&bp, &hp, j, &pl, sizeof(pl));
			if (!(pl.flags & GC_PLOT_F_REVEALED) || pl.plot_type != GC_PLOT_TYPE_LAND || hex_dist(wx, wy, pl.x, pl.y) != 2) continue;
			a[0] = human; a[1] = wid; a[2] = pl.x; a[3] = pl.y;
			if (gc_query(inst, GC_Q_PATH, a, 4, &bn, &err) != GC_OK) continue;
			if (gcq_check(&bn, GC_Q_PATH, sizeof(gcq_path_node), "Q_PATH(warrior)", &hn) && hn.count >= 3 && (k == 1 || ((gcq_path_node*)(bn.data + sizeof(gcq_header) + (size_t)(hn.count - 1) * hn.record_size))->turn == 0)) {
				gcq_rec(&bn, &hn, hn.count - 1, &nd, sizeof(nd));
				tx = pl.x; ty = pl.y; path_n = (int)hn.count; tx_turn = nd.turn;
				break;
			}
			gc_buf_release(&bn);
		}
	}
	gc_buf_release(&bp);
	CHECK(tx >= 0, "found a land plot two tiles from the warrior with a path");
	if (tx < 0) return;
	printf("  warrior (%d,%d) -> (%d,%d): path of %d nodes:", wx, wy, tx, ty, path_n);
	for (j = 0; j < hn.count; ++j) { gcq_rec(&bn, &hn, j, &nd, sizeof(nd)); printf(" (%d,%d)t%d/m%d", nd.x, nd.y, nd.turn, nd.moves_left); }
	printf("\n");
	gcq_rec(&bn, &hn, 0, &nd, sizeof(nd));
	CHECK(nd.x == wx && nd.y == wy && nd.turn == 0, "Q_PATH starts on the warrior's plot at turn 0");
	gcq_rec(&bn, &hn, hn.count - 1, &nd, sizeof(nd));
	CHECK(nd.x == tx && nd.y == ty, "Q_PATH ends on the target");
	ex = wx; ey = wy;
	for (j = 0; j < hn.count; ++j) { /* where the warrior can stand at the end of this turn */
		gcq_rec(&bn, &hn, j, &nd, sizeof(nd));
		if (nd.turn == 0) { ex = nd.x; ey = nd.y; }
	}
	{
		int bad = 0, prev_t = 0;
		for (j = 1; j < hn.count; ++j) {
			gcq_path_node p0, p1;
			gcq_rec(&bn, &hn, j - 1, &p0, sizeof(p0));
			gcq_rec(&bn, &hn, j, &p1, sizeof(p1));
			if (hex_dist(p0.x, p0.y, p1.x, p1.y) != 1 || p1.turn < prev_t) ++bad;
			prev_t = p1.turn;
		}
		CHECK(bad == 0, "Q_PATH nodes are neighbours with turns that do not decrease");
	}
	gc_buf_release(&bn);
	a[0] = human; a[1] = wid; a[2] = wx; a[3] = wy;
	CHECK(gc_query(inst, GC_Q_PATH, a, 4, &bn, &err) == GC_OK && ((gcq_header*)bn.data)->count == 0, "Q_PATH to the unit's own plot -> zero records");
	gc_buf_release(&bn);
	a[2] = w + 3;
	CHECK(gc_query(inst, GC_Q_PATH, a, 4, &bn, &err) == GC_ERR_INVALID_ARG && err.message[0] != '\0', "Q_PATH off the map -> INVALID_ARG");

	/* Q_REACHABLE */
	a[0] = human; a[1] = wid;
	CHECK(gc_query(inst, GC_Q_REACHABLE, a, 2, &br, &err) == GC_OK, "Q_REACHABLE(warrior)");
	if (gcq_check(&br, GC_Q_REACHABLE, sizeof(gcq_path_node), "Q_REACHABLE(warrior)", &hr)) {
		for (j = 0; j < hr.count; ++j) {
			gcq_rec(&br, &hr, j, &nd, sizeof(nd));
			if (nd.x == tx && nd.y == ty) reach_has_target = 1;
			if (nd.turn != 0 || (nd.x == wx && nd.y == wy) || hex_dist(wx, wy, nd.x, nd.y) > 3) ++reach_bad;
		}
		printf("  reachable by the warrior this turn: %u plots\n", (unsigned)hr.count);
		CHECK(hr.count >= 6 && reach_bad == 0, "Q_REACHABLE: plots around the warrior, none is its own plot, all within moves");
		CHECK(tx_turn != 0 || reach_has_target, "Q_REACHABLE contains the target when Q_PATH reaches it this turn");
	}
	gc_buf_release(&br);

	/* Q_RANGE_TARGETS (G1b): a warrior has no ranged attack, so no targets; argument errors */
	{
		gc_buf bt = {0, 0, 0, 0};
		gcq_header ht;
		a[0] = human; a[1] = wid;
		CHECK(gc_query(inst, GC_Q_RANGE_TARGETS, a, 2, &bt, &err) == GC_OK && gcq_check(&bt, GC_Q_RANGE_TARGETS, sizeof(gcq_range_target), "Q_RANGE_TARGETS(warrior)", &ht) && ht.count == 0,
			"Q_RANGE_TARGETS(warrior) -> zero records (no ranged attack)");
		gc_buf_release(&bt);
		a[1] = 999999;
		CHECK(gc_query(inst, GC_Q_RANGE_TARGETS, a, 2, &bt, &err) == GC_ERR_INVALID_ARG && err.message[0] != '\0' && bt.data == NULL, "Q_RANGE_TARGETS of a missing unit -> INVALID_ARG");
		a[1] = -999999;
		CHECK(gc_query(inst, GC_Q_RANGE_TARGETS, a, 2, &bt, &err) == GC_ERR_INVALID_ARG && err.message[0] != '\0', "Q_RANGE_TARGETS of a missing city -> INVALID_ARG");
		a[1] = wid;
		CHECK(gc_query(inst, GC_Q_RANGE_TARGETS, a, 1, &bt, &err) == GC_ERR_INVALID_ARG, "Q_RANGE_TARGETS with one argument -> INVALID_ARG");
		a[0] = 100000;
		CHECK(gc_query(inst, GC_Q_RANGE_TARGETS, a, 2, &bt, &err) == GC_ERR_INVALID_ARG, "Q_RANGE_TARGETS player 100000 -> INVALID_ARG");
	}

	/* invalid commands */
	expect_rejected(inst, "move to a plot outside the map", GC_CMD_UNIT_MOVE_TO, human, 3, wid, w + 5, 1, "outside the map");
	expect_rejected(inst, "move to a negative plot", GC_CMD_UNIT_MOVE_TO, human, 3, wid, -1, 0, "outside the map");
	expect_rejected(inst, "move to the unit's own plot", GC_CMD_UNIT_MOVE_TO, human, 3, wid, wx, wy, "already");
	expect_rejected(inst, "a unit that does not exist", GC_CMD_UNIT_FORTIFY, human, 1, 999999, 0, 0, "no unit");
	for (i = 0; i < nall; ++i) if (all[i].owner != human) break;
	if (i < nall) {
		sprintf(msg, "a command as player %d (AI) on its unit", (int)all[i].owner);
		expect_rejected(inst, msg, GC_CMD_UNIT_SLEEP, all[i].owner, 1, all[i].id, 0, 0, "not the human");
		/* v2 (N3.4): the refusal carries a code the host can act on */
		CHECK(apply(inst, GC_CMD_UNIT_SLEEP, all[i].owner, 1, all[i].id, 0, 0, &err) == GC_ERR_REJECTED && err.reason == GC_REASON_NOT_HUMAN, "a command as an AI player -> reason NOT_HUMAN");
	}
	CHECK(apply(inst, GC_CMD_UNIT_MOVE_TO, human, 3, wid, w + 5, 1, &err) == GC_ERR_REJECTED && err.reason == GC_REASON_BAD_TARGET, "move outside the map -> reason BAD_TARGET");
	CHECK(apply(inst, GC_CMD_UNIT_FOUND_CITY, human, 1, wid, 0, 0, &err) == GC_ERR_REJECTED && err.reason == GC_REASON_RULE, "found a city with a warrior -> reason RULE");
	expect_rejected(inst, "found a city with a warrior", GC_CMD_UNIT_FOUND_CITY, human, 1, wid, 0, 0, "cannot found");
	expect_rejected(inst, "range attack of a warrior", GC_CMD_UNIT_RANGE_ATTACK, human, 3, wid, tx, ty, "ranged");
	expect_rejected(inst, "automate build of a warrior", GC_CMD_UNIT_AUTOMATE_BUILD, human, 1, wid, 0, 0, "automate");
	expect_rejected(inst, "cancel with no orders", GC_CMD_UNIT_CANCEL, human, 1, wid, 0, 0, "no orders");
	expect_rejected(inst, "wake an awake unit", GC_CMD_UNIT_WAKE, human, 1, wid, 0, 0, "not sleeping");
	CHECK(apply(inst, GC_CMD_UNIT_MOVE_TO, human, 2, wid, tx, 0, &err) == GC_ERR_INVALID_ARG && err.message[0] != '\0', "UNIT_MOVE_TO with two arguments -> INVALID_ARG");
	CHECK(apply(inst, 99, human, 0, 0, 0, 0, &err) == GC_ERR_INVALID_ARG && err.message[0] != '\0', "unknown command kind -> INVALID_ARG");
	CHECK(apply(inst, GC_CMD_CITY_PURCHASE, human, 0, 0, 0, 0, &err) == GC_ERR_INVALID_ARG, "CITY_PURCHASE with no arguments -> INVALID_ARG");
	/* nothing above queued anything */
	n = read_units(inst, team, us, MAX_UNITS, "Q_UNITS(human team) after the refused commands");
	CHECK(n >= 0 && unit_of(us, n, human, wid) && unit_of(us, n, human, wid)->x == wx && unit_of(us, n, human, wid)->y == wy, "refused commands changed nothing");
	/* END_TURN while the turn is held by a blocker */
	if (pr.blocker >= 0) {
		expect_rejected(inst, "END_TURN while blocked", GC_CMD_END_TURN, human, 0, 0, 0, 0, "ENDTURN_BLOCKING");
		if (pr.blocker != 3 /* UNITS */) expect_rejected(inst, "FORCE_END_TURN while blocked by something else than units", GC_CMD_FORCE_END_TURN, human, 0, 0, 0, 0, "ENDTURN_BLOCKING");
		else expect_rejected(inst, "FORCE_END_TURN with two awake units (gamecore limit)", GC_CMD_FORCE_END_TURN, human, 0, 0, 0, 0, "awake units");
	} else
		printf("  (no end-turn blocker at the start: END_TURN rejection is checked after the city is founded)\n");

	CHECK(apply(inst, GC_CMD_UNIT_FORTIFY, human, 1, wid, 0, 0, &err) == GC_OK, "UNIT_FORTIFY accepted");
	CHECK(pump_to_human(inst, &pr), "pump after UNIT_FORTIFY");
	n = read_units(inst, team, us, MAX_UNITS, "Q_UNITS after fortify");
	u = n >= 0 ? unit_of(us, n, human, wid) : NULL;
	CHECK(u && (u->flags & GC_UNIT_F_FORTIFIED) && !(u->flags & GC_UNIT_F_NEEDS_ORDERS), "the warrior is fortified");
	CHECK(apply(inst, GC_CMD_UNIT_WAKE, human, 1, wid, 0, 0, &err) == GC_OK, "UNIT_WAKE accepted");
	CHECK(pump_to_human(inst, &pr), "pump after UNIT_WAKE");
	n = read_units(inst, team, us, MAX_UNITS, "Q_UNITS after wake");
	u = n >= 0 ? unit_of(us, n, human, wid) : NULL;
	CHECK(u && !(u->flags & GC_UNIT_F_FORTIFIED), "the warrior is awake again");
	/* move the warrior */
	CHECK(apply(inst, GC_CMD_UNIT_MOVE_TO, human, 3, wid, tx, ty, &err) == GC_OK && err.code == GC_OK, "UNIT_MOVE_TO accepted");
	CHECK(pump_to_human(inst, &pr) && pr.turn == g.turn, "gc_pump delivers the move and the human is up again in the same turn");
	n = read_units(inst, team, us, MAX_UNITS, "Q_UNITS after the move");
	u = n >= 0 ? unit_of(us, n, human, wid) : NULL;
	if (u) printf("  warrior now at (%d,%d), moves %d/%d\n", u->x, u->y, u->moves_left, u->max_moves);
	CHECK(u && u->x == ex && u->y == ey && (ex != wx || ey != wy) && u->moves_left < u->max_moves, "the warrior moved along the path to where its moves end (or the target) and has spent moves");

	/* fortify / sleep / wake */
	expect_rejected(inst, "sleep a warrior (it fortifies instead)", GC_CMD_UNIT_SLEEP, human, 1, wid, 0, 0, "fortify");
	CHECK(apply(inst, GC_CMD_UNIT_SLEEP, human, 1, sid, 0, 0, &err) == GC_OK, "UNIT_SLEEP of the settler accepted");
	CHECK(pump_to_human(inst, &pr), "pump after UNIT_SLEEP");
	n = read_units(inst, team, us, MAX_UNITS, "Q_UNITS after sleep");
	u = n >= 0 ? unit_of(us, n, human, sid) : NULL;
	CHECK(u && (u->flags & GC_UNIT_F_SLEEPING) && !(u->flags & GC_UNIT_F_NEEDS_ORDERS), "the settler sleeps");
	CHECK(apply(inst, GC_CMD_UNIT_WAKE, human, 1, sid, 0, 0, &err) == GC_OK, "UNIT_WAKE of the settler accepted");
	CHECK(pump_to_human(inst, &pr), "pump after UNIT_WAKE");
	n = read_units(inst, team, us, MAX_UNITS, "Q_UNITS after wake");
	u = n >= 0 ? unit_of(us, n, human, sid) : NULL;
	CHECK(u && !(u->flags & GC_UNIT_F_SLEEPING) && (u->flags & GC_UNIT_F_NEEDS_ORDERS), "the settler is awake again");

	/* found the city */
	CHECK(apply(inst, GC_CMD_UNIT_FOUND_CITY, human, 1, sid, 0, 0, &err) == GC_OK && err.code == GC_OK, "UNIT_FOUND_CITY accepted");
	CHECK(pump_to_human(inst, &pr) && pr.turn == g.turn, "gc_pump delivers the founding and the human is up again");
	a[0] = team;
	if (gc_query(inst, GC_Q_PLOTS, a, 1, &bp, &err) == GC_OK && gcq_check(&bp, GC_Q_PLOTS, sizeof(gcq_plot), "Q_PLOTS after founding", &hp)) {
		gcq_rec(&bp, &hp, (uint32_t)sy * (uint32_t)w + (uint32_t)sx, &pl, sizeof(pl));
		printf("  plot (%d,%d): flags %04x owner %d\n", sx, sy, (unsigned)pl.flags, pl.owner);
		CHECK((pl.flags & GC_PLOT_F_CITY) && pl.owner == human, "the settler's plot has a city owned by the human (Q_PLOTS city flag)");
		gc_buf_release(&bp);
	}
	n = read_units(inst, team, us, MAX_UNITS, "Q_UNITS after founding");
	CHECK(n >= 0 && !unit_of(us, n, human, sid) && unit_of(us, n, human, wid), "the settler is gone, the warrior is still there");
	(void)h;
	/* the founded city (or the research choice) holds the turn: END_TURN is refused and names the blocker */
	if (gc_query(inst, GC_Q_GAME, NULL, 0, &bg, &err) == GC_OK && gcq_check(&bg, GC_Q_GAME, sizeof(gcq_game), "Q_GAME", &hg)) {
		gcq_rec(&bg, &hg, 0, &g, sizeof(g));
		gc_buf_release(&bg);
		printf("  blocker now %d (unit %d city %d at %d,%d)\n", g.blocker, g.blocker_unit, g.blocker_city, g.blocker_x, g.blocker_y);
		CHECK(g.blocker >= 0, "the turn is held by an end-turn blocker");
		expect_rejected(inst, "END_TURN while blocked", GC_CMD_END_TURN, human, 0, 0, 0, 0, "ENDTURN_BLOCKING");
		if (g.blocker != 3) expect_rejected(inst, "FORCE_END_TURN while blocked by something else than units", GC_CMD_FORCE_END_TURN, human, 0, 0, 0, 0, "ENDTURN_BLOCKING");
	}
}

/* ---- G1a: cities, top bar, city production, purchase, city range attack, declare war, viewer digest ---- */

uint64_t lekmod_portable_xxh64(const void* data, size_t len, uint64_t seed); /* abi/gc_digest.cpp: C-linkage test entry of the digest hash */

#define MAX_CITIES 32
#define MAX_BUILDS 600

static int catalog_id(gc_instance* inst, int table, const char* type)
{
	gc_buf b = {0, 0, 0, 0};
	gcq_header h;
	gcq_catalog ce;
	gc_error err;
	int32_t a[1];
	uint32_t k;
	int found = -1;
	a[0] = table;
	if (gc_query(inst, GC_Q_CATALOG, a, 1, &b, &err) != GC_OK) return -1;
	if (gcq_check(&b, GC_Q_CATALOG, sizeof(gcq_catalog), "Q_CATALOG", &h)) {
		for (k = 0; k < h.count && found < 0; ++k) {
			gcq_rec(&b, &h, k, &ce, sizeof(ce));
			if (strcmp(gcq_str(&b, &h, ce.type), type) == 0) found = ce.id;
		}
	}
	gc_buf_release(&b);
	return found;
}

/* Q_CITIES(viewer) into cs[]; returns the number of cities (-1 on failure). */
static int read_cities(gc_instance* inst, int viewer, gcq_city* cs, int max, const char* what)
{
	gc_buf b = {0, 0, 0, 0};
	gcq_header h;
	gc_error err;
	int32_t a[1];
	uint32_t i;
	int n, str_bad = 0;
	a[0] = viewer;
	if (gc_query(inst, GC_Q_CITIES, a, 1, &b, &err) != GC_OK) { printf("  FAIL %s: %s\n", what, err.message); ++g_failures; return -1; }
	if (!gcq_check(&b, GC_Q_CITIES, sizeof(gcq_city), what, &h)) { gc_buf_release(&b); return -1; }
	n = (int)h.count < max ? (int)h.count : max;
	for (i = 0; i < (uint32_t)n; ++i) {
		gcq_rec(&b, &h, i, &cs[i], sizeof(gcq_city));
		if (!gcq_str_ok(&b, &h, cs[i].name) || cs[i].name == GCQ_NO_STRING || gcq_str(&b, &h, cs[i].name)[0] == '\0') ++str_bad;
		/* the name outlives the buffer only as a copy: keep it in the record's reserved bytes is not possible, print here */
		printf("  city %d/%d \"%s\" at (%d,%d) pop %d flags %04x hp %d/%d def %d prod %d:%d stored %d/%d turns %d growth %d yields %d/%d/%d/%d/%d/%d\n", cs[i].owner, cs[i].id,
			gcq_str(&b, &h, cs[i].name), cs[i].x, cs[i].y, cs[i].population, (unsigned)cs[i].flags, cs[i].hp, cs[i].max_hp, cs[i].defense, cs[i].production_order,
			cs[i].production_item, cs[i].production_stored, cs[i].production_needed, cs[i].production_turns, cs[i].growth_turns, cs[i].yields[0], cs[i].yields[1],
			cs[i].yields[2], cs[i].yields[3], cs[i].yields[4], cs[i].yields[5]);
	}
	CHECK(str_bad == 0, "Q_CITIES: every city has a name string inside the table");
	gc_buf_release(&b);
	return n;
}

static int read_top_bar(gc_instance* inst, int player, gcq_top_bar* t)
{
	gc_buf b = {0, 0, 0, 0};
	gcq_header h;
	gc_error err;
	int32_t a[1];
	a[0] = player;
	if (gc_query(inst, GC_Q_TOP_BAR, a, 1, &b, &err) != GC_OK) { printf("  FAIL Q_TOP_BAR: %s\n", err.message); ++g_failures; return 0; }
	if (!gcq_check(&b, GC_Q_TOP_BAR, sizeof(gcq_top_bar), "Q_TOP_BAR", &h) || h.count != 1) { gc_buf_release(&b); CHECK(0, "Q_TOP_BAR has one record"); return 0; }
	gcq_rec(&b, &h, 0, t, sizeof(*t));
	CHECK(h.strings_size == 0, "Q_TOP_BAR has an empty string table");
	gc_buf_release(&b);
	return 1;
}

/* Q_CITY_BUILDS into bs[] (reasons are checked here and copied to rs[] as "" or the text); returns the number of entries (-1 on failure). */
static int read_builds(gc_instance* inst, int player, int city, gcq_city_build* bs, char (*rs)[160], int max)
{
	gc_buf b = {0, 0, 0, 0};
	gcq_header h;
	gc_error err;
	int32_t a[2];
	uint32_t i;
	int n, reason_bad = 0;
	a[0] = player;
	a[1] = city;
	if (gc_query(inst, GC_Q_CITY_BUILDS, a, 2, &b, &err) != GC_OK) { printf("  FAIL Q_CITY_BUILDS: %s\n", err.message); ++g_failures; return -1; }
	if (!gcq_check(&b, GC_Q_CITY_BUILDS, sizeof(gcq_city_build), "Q_CITY_BUILDS", &h)) { gc_buf_release(&b); return -1; }
	n = (int)h.count < max ? (int)h.count : max;
	for (i = 0; i < (uint32_t)n; ++i) {
		gcq_rec(&b, &h, i, &bs[i], sizeof(gcq_city_build));
		rs[i][0] = '\0';
		if (!gcq_str_ok(&b, &h, bs[i].reason)) ++reason_bad;
		else if (bs[i].reason != GCQ_NO_STRING) { strncpy(rs[i], gcq_str(&b, &h, bs[i].reason), 159); rs[i][159] = '\0'; }
		if (((bs[i].flags & GC_BUILD_F_ENABLED) != 0) != (bs[i].reason == GCQ_NO_STRING)) ++reason_bad; /* a reason exactly when disabled */
	}
	CHECK(reason_bad == 0, "Q_CITY_BUILDS: a reason string exactly when an entry is disabled");
	gc_buf_release(&b);
	return n;
}

static const gcq_city_build* build_of(const gcq_city_build* bs, int n, int order, int item)
{
	int i;
	for (i = 0; i < n; ++i) if (bs[i].order_type == order && bs[i].item == item) return &bs[i];
	return NULL;
}

static int blocker_now(gc_instance* inst)
{
	gc_buf b = {0, 0, 0, 0};
	gcq_header h;
	gcq_game g;
	gc_error err;
	int r = -2;
	if (gc_query(inst, GC_Q_GAME, NULL, 0, &b, &err) == GC_OK && gcq_check(&b, GC_Q_GAME, sizeof(gcq_game), "Q_GAME", &h)) {
		gcq_rec(&b, &h, 0, &g, sizeof(g));
		r = g.blocker;
	}
	gc_buf_release(&b);
	return r;
}


static int32_t apply4(gc_instance* inst, int kind, int player, int argc, int32_t a0, int32_t a1, int32_t a2, int32_t a3, gc_error* err)
{
	int32_t args[4];
	gc_command c;
	args[0] = a0;
	args[1] = a1;
	args[2] = a2;
	args[3] = a3;
	memset(&c, 0, sizeof(c));
	c.kind = (uint16_t)kind;
	c.player = (uint8_t)player;
	c.argc = (uint32_t)argc;
	c.args = args;
	memset(err, 0, sizeof(*err));
	return gc_apply_command(inst, &c, err);
}

static void expect_rejected4(gc_instance* inst, const char* what, int kind, int player, int32_t a0, int32_t a1, int32_t a2, int32_t a3, const char* needle)
{
	gc_error err;
	char msg[300];
	int32_t rc = apply4(inst, kind, player, 4, a0, a1, a2, a3, &err);
	printf("  rejected: %s\n", err.message);
	sprintf(msg, "%s -> REJECTED naming \"%s\"", what, needle);
	CHECK(rc == GC_ERR_REJECTED && err.code == GC_ERR_REJECTED && strstr(err.message, needle) != NULL, msg);
}

/* F3 + F2: the commands that clear the end-turn blockers the smoke meets: orders for the human's units (FORTIFY, else SKIP) and a
 * production for every city without one (Monument, else Warrior). Returns the number of commands accepted. */
static int give_orders(gc_instance* inst, int human, int team)
{
	gcq_unit us[MAX_UNITS];
	gcq_city cs[MAX_CITIES];
	gc_error err;
	int n, i, done = 0, warrior = unit_type_id(inst, "UNIT_WARRIOR"), monument = catalog_id(inst, GC_CATALOG_BUILDINGS, "BUILDING_MONUMENT");
	n = read_units(inst, team, us, MAX_UNITS, "Q_UNITS(human team) for the orders");
	for (i = 0; i < n; ++i) {
		if (us[i].owner != human || !(us[i].flags & GC_UNIT_F_NEEDS_ORDERS)) continue;
		if (apply(inst, GC_CMD_UNIT_FORTIFY, human, 1, us[i].id, 0, 0, &err) == GC_OK) ++done;
		else if (apply(inst, GC_CMD_UNIT_SKIP, human, 1, us[i].id, 0, 0, &err) == GC_OK) ++done;
		else printf("  (unit %d takes neither FORTIFY nor SKIP: %s)\n", us[i].id, err.message);
	}
	n = read_cities(inst, team, cs, MAX_CITIES, "Q_CITIES(human team) for the production");
	for (i = 0; i < n; ++i) {
		if (cs[i].owner != human || cs[i].production_order >= 0) continue;
		if (apply(inst, GC_CMD_CITY_SET_PRODUCTION, human, 3, cs[i].id, GC_ORDER_CONSTRUCT, monument, &err) == GC_OK) ++done;
		else if (apply(inst, GC_CMD_CITY_SET_PRODUCTION, human, 3, cs[i].id, GC_ORDER_TRAIN, warrior, &err) == GC_OK) ++done;
		else printf("  (city %d takes neither Monument nor Warrior: %s)\n", cs[i].id, err.message);
	}
	return done;
}

/* END_TURN for the human; while the core names a blocker the orders above are given and the command repeated (research is chosen
 * by the runtime, F3). Then pumps until the human is up again. 1 = the turn advanced and the human is up. */
static int end_turn_resolving(gc_instance* inst, int human, int team, gc_pump_result* pr)
{
	gc_error err;
	int tries;
	int32_t rc = GC_ERR_REJECTED;
	for (tries = 0; tries < 4; ++tries) {
		rc = apply(inst, GC_CMD_END_TURN, human, 0, 0, 0, 0, &err);
		if (rc == GC_OK) break;
		printf("  END_TURN refused: %s\n", err.message);
		if (give_orders(inst, human, team) == 0) break;
		if (!pump_to_human(inst, pr)) break; /* the queued orders are delivered and their missions run */
	}
	if (rc != GC_OK) return 0;
	return pump_to_human(inst, pr);
}

/* The AI founds its capital on its own first turn. With the auto-choices (research) the smoke can end turns; before them the
 * human's turn cannot end and this part is skipped. Then every AI city is checked against the fog rule through Q_PLOTS. */
static void check_foreign_cities(gc_instance* inst, int human, int team, int ai_player, int ai_team)
{
	gcq_city ca[MAX_CITIES], ch[MAX_CITIES], cn[MAX_CITIES];
	gcq_plot pl;
	gcq_header hp;
	gc_buf bp = {0, 0, 0, 0};
	gc_pump_result pr;
	gc_error err;
	int32_t a[1];
	int nall = 0, nh, nn, i, j, t, ai_n = 0, w = 0, bad = 0, present;
	for (t = 0; t < 4; ++t) {
		nall = read_cities(inst, -1, ca, MAX_CITIES, "Q_CITIES(-1) looking for the AI capital");
		for (i = 0, ai_n = 0; i < nall; ++i) if (ca[i].owner == ai_player) ++ai_n;
		if (ai_n > 0 || nall < 0) break;
		if (!end_turn_resolving(inst, human, team, &pr)) { CHECK(0, "END_TURN and the pump back to the human while looking for the AI capital"); return; }
	}
	if (ai_n == 0) { printf("  (the AI founded no city within 4 turns)\n"); return; }
	a[0] = team;
	if (gc_query(inst, GC_Q_PLOTS, a, 1, &bp, &err) != GC_OK || !gcq_check(&bp, GC_Q_PLOTS, sizeof(gcq_plot), "Q_PLOTS(human team)", &hp)) { CHECK(0, "Q_PLOTS"); return; }
	{
		gc_buf bg = {0, 0, 0, 0};
		gcq_header hg;
		gcq_game g;
		if (gc_query(inst, GC_Q_GAME, NULL, 0, &bg, &err) == GC_OK && gcq_check(&bg, GC_Q_GAME, sizeof(gcq_game), "Q_GAME", &hg)) { gcq_rec(&bg, &hg, 0, &g, sizeof(g)); w = g.map_width; }
		gc_buf_release(&bg);
	}
	nh = read_cities(inst, team, ch, MAX_CITIES, "Q_CITIES(human team) with the AI on the map");
	nn = read_cities(inst, ai_team, cn, MAX_CITIES, "Q_CITIES(AI team)");
	for (i = 0; i < nall; ++i) {
		if (ca[i].owner != ai_player) continue;
		CHECK((ca[i].flags & GC_CITY_F_VISIBLE) && ca[i].population >= 1 && ca[i].hp > 0, "Q_CITIES(-1): the AI city with live data");
		gcq_rec(&bp, &hp, (uint32_t)ca[i].y * (uint32_t)w + (uint32_t)ca[i].x, &pl, sizeof(pl));
		present = 0;
		for (j = 0; j < nh; ++j) if (ch[j].owner == ca[i].owner && ch[j].id == ca[i].id) { present = 1; if (((ch[j].flags & GC_CITY_F_VISIBLE) != 0) != ((pl.flags & GC_PLOT_F_VISIBLE) != 0)) ++bad; if (!(ch[j].flags & GC_CITY_F_VISIBLE) && ch[j].population != 0) ++bad; }
		if (present != ((pl.flags & GC_PLOT_F_REVEALED) && ((pl.flags & GC_PLOT_F_VISIBLE) || pl.owner == ca[i].owner))) ++bad;
		printf("  AI city %d/%d at (%d,%d): plot revealed %d visible %d, in the human's list: %d\n", ca[i].owner, ca[i].id, ca[i].x, ca[i].y, (pl.flags & GC_PLOT_F_REVEALED) != 0, (pl.flags & GC_PLOT_F_VISIBLE) != 0, present);
		present = 0;
		for (j = 0; j < nn; ++j) if (cn[j].owner == ca[i].owner && cn[j].id == ca[i].id && (cn[j].flags & GC_CITY_F_VISIBLE)) present = 1;
		CHECK(present, "Q_CITIES(AI team) lists its own city with live data");
	}
	CHECK(bad == 0, "Q_CITIES(human team) lists a foreign city exactly when its plot is revealed (visible: live data; fogged: last seen, no population)");
	gc_buf_release(&bp);
}

static void run_city_checks(gc_instance* inst)
{
	static gcq_city_build bs[MAX_BUILDS];
	static char rs[MAX_BUILDS][160];
	uint8_t vec[100];
	gcq_city cs[MAX_CITIES], ca[MAX_CITIES], cn[MAX_CITIES];
	gcq_top_bar tb, tb2, tb3;
	gcq_game g;
	gcq_header hg;
	gc_buf bg = {0, 0, 0, 0};
	gc_error err;
	gc_pump_result pr;
	int32_t a[4];
	uint64_t d_all, d_all2, d_human, d_human2, d_ai = 0, d_before, d_after, d_after2;
	char msg[300];
	int human, team, ai_player = -1, ai_team = -1, n, nall, nn, i, k, own = -1, ai_seen_by_human = 0, bn;
	int warrior, worker, monument, library, units_n, cost_bad, wonder_n, cur_n, px, py;
	int buy_order = -1, buy_item = -1, buy_cost = 0, afford_order = -1, afford_item = -1;
	const gcq_city_build *bw, *bk, *bm;
	gcq_city c;

	printf("-- cities, top bar, city production, purchase, range attack, declare war, digest\n");

	/* xxh64: published vectors (seed 0), a 4-lane-loop input, and an independent implementation for 100 bytes with and without a seed */
	CHECK(lekmod_portable_xxh64("", 0, 0) == 0xEF46DB3751D8E999ULL, "xxh64(\"\") = EF46DB3751D8E999");
	CHECK(lekmod_portable_xxh64("a", 1, 0) == 0xD24EC4F1A98C6E5BULL, "xxh64(\"a\") = D24EC4F1A98C6E5B");
	CHECK(lekmod_portable_xxh64("abc", 3, 0) == 0x44BC2CF5AD770999ULL, "xxh64(\"abc\") = 44BC2CF5AD770999");
	CHECK(lekmod_portable_xxh64("Nobody inspects the spammish repetition", 39, 0) == 0xFBCEA83C8A378BF1ULL, "xxh64(39 bytes, 4-lane loop) = FBCEA83C8A378BF1");
	for (i = 0; i < 100; ++i) vec[i] = (uint8_t)i;
	CHECK(lekmod_portable_xxh64(vec, 100, 0) == 0x6AC1E58032166597ULL, "xxh64(bytes 0..99) = 6AC1E58032166597");
	CHECK(lekmod_portable_xxh64(vec, 100, 0x9E3779B1ULL) == 0x8832442A88284F11ULL, "xxh64(bytes 0..99, seed 9E3779B1) = 8832442A88284F11");

	if (gc_query(inst, GC_Q_GAME, NULL, 0, &bg, &err) != GC_OK || !gcq_check(&bg, GC_Q_GAME, sizeof(gcq_game), "Q_GAME", &hg)) { CHECK(0, "Q_GAME"); return; }
	gcq_rec(&bg, &hg, 0, &g, sizeof(g));
	gc_buf_release(&bg);
	human = g.human_player;
	team = g.human_team;

	/* argument errors */
	{
		gc_buf b = {0, 0, 0, 0};
		CHECK(gc_query(inst, GC_Q_CITIES, NULL, 0, &b, &err) == GC_ERR_INVALID_ARG && err.message[0] != '\0', "Q_CITIES without viewer_team -> INVALID_ARG");
		a[0] = 100000;
		CHECK(gc_query(inst, GC_Q_CITIES, a, 1, &b, &err) == GC_ERR_INVALID_ARG, "Q_CITIES viewer 100000 -> INVALID_ARG");
		CHECK(gc_query(inst, GC_Q_TOP_BAR, NULL, 0, &b, &err) == GC_ERR_INVALID_ARG, "Q_TOP_BAR without a player -> INVALID_ARG");
		a[0] = 100;
		CHECK(gc_query(inst, GC_Q_TOP_BAR, a, 1, &b, &err) == GC_ERR_INVALID_ARG && err.message[0] != '\0', "Q_TOP_BAR of a player that is not a major civilization -> INVALID_ARG");
		a[0] = human; a[1] = 999999;
		CHECK(gc_query(inst, GC_Q_CITY_BUILDS, a, 2, &b, &err) == GC_ERR_INVALID_ARG && err.message[0] != '\0', "Q_CITY_BUILDS of a missing city -> INVALID_ARG");
		CHECK(gc_query(inst, GC_Q_CITY_BUILDS, a, 1, &b, &err) == GC_ERR_INVALID_ARG, "Q_CITY_BUILDS with one argument -> INVALID_ARG");
		CHECK(b.data == NULL, "refused city queries leave the buffer alone");
	}

	/* the AI major civilization */
	{
		gc_buf bp = {0, 0, 0, 0};
		gcq_header hp;
		gcq_player pl;
		uint32_t j;
		a[0] = -1;
		if (gc_query(inst, GC_Q_PLAYERS, a, 1, &bp, &err) == GC_OK && gcq_check(&bp, GC_Q_PLAYERS, sizeof(gcq_player), "Q_PLAYERS(-1)", &hp)) {
			for (j = 0; j < hp.count; ++j) {
				gcq_rec(&bp, &hp, j, &pl, sizeof(pl));
				if (pl.id != human && pl.team != team && !(pl.flags & (GC_PLAYER_F_MINOR | GC_PLAYER_F_BARBARIAN)) && (pl.flags & GC_PLAYER_F_ALIVE) && ai_player < 0) { ai_player = pl.id; ai_team = pl.team; }
			}
		}
		gc_buf_release(&bp);
	}
	CHECK(ai_player >= 0, "there is an AI major civilization");

	/* cities: the human's, the fog, the whole truth */
	n = read_cities(inst, team, cs, MAX_CITIES, "Q_CITIES(human team)");
	nall = read_cities(inst, -1, ca, MAX_CITIES, "Q_CITIES(-1)");
	if (n < 0 || nall < 0) return;
	for (i = 0; i < n; ++i) {
		if (cs[i].owner == human && own < 0) own = i;
		if (cs[i].owner == ai_player) ++ai_seen_by_human;
	}
	CHECK(own >= 0, "Q_CITIES lists the human's city founded by the smoke");
	if (own < 0) return;
	c = cs[own];
	CHECK((c.flags & GC_CITY_F_VISIBLE) && (c.flags & GC_CITY_F_CAPITAL) && !(c.flags & GC_CITY_F_PUPPET), "own city: visible, capital, not a puppet");
	CHECK(c.population >= 1 && c.max_hp > 0 && c.hp == c.max_hp && c.defense > 0, "own city: population, full hit points, defense");
	CHECK(c.yields[GC_YIELD_FOOD] > 0 && c.yields[GC_YIELD_PRODUCTION] > 0, "own city: food and production per turn");
	CHECK(c.growth_turns > 0, "own city: turns to grow");
	CHECK(c.production_order == -1 && c.production_item == -1 && c.production_turns == -1, "own city: nothing in production yet");
	printf("  the human's view has %d city(ies), the whole game has %d\n", n, nall);
	CHECK(ai_seen_by_human == 0, "fog: no AI city is in Q_CITIES for the human's team (its plots are not revealed)");
	CHECK(nall >= n, "Q_CITIES(-1) lists at least as many cities as the human's view");
	nn = read_cities(inst, ai_team, cn, MAX_CITIES, "Q_CITIES(AI team)");
	{
		int saw_human = 0;
		for (i = 0; i < nn; ++i) if (cn[i].owner == human) saw_human = 1;
		CHECK(!saw_human, "fog: the AI team does not see the human's new city (its plot is not revealed to it)");
		CHECK(nall >= 1 && !saw_human, "Q_CITIES(-1) lists the human's city that the AI team does not see");
	}

	/* top bar */
	if (!read_top_bar(inst, human, &tb)) return;
	printf("  top bar: gold %d (%+d/turn) science %+d/turn research %d (%d turns, %d/%d) culture %d (%+d/turn, next policy %d) happiness %d golden age %d (%d/%d) faith %d (%+d) era %d flags %04x\n",
		tb.gold, tb.gold_per_turn, tb.science_per_turn, tb.research_tech, tb.research_turns, tb.research_progress, tb.research_cost, tb.culture, tb.culture_per_turn,
		tb.next_policy_cost, tb.happiness, tb.golden_age_turns, tb.golden_age_progress, tb.golden_age_threshold, tb.faith, tb.faith_per_turn, tb.era, (unsigned)tb.flags);
	CHECK(tb.gold >= 0 && tb.gold_per_turn > -50 && tb.gold_per_turn < 200, "Q_TOP_BAR: gold and gold per turn are sane");
	CHECK(tb.science_per_turn >= 0 && tb.science_per_turn < 100, "Q_TOP_BAR: science per turn is sane");
	CHECK(tb.culture >= 0 && tb.culture_per_turn >= 0 && tb.next_policy_cost > 0 && tb.faith >= 0 && tb.golden_age_turns == 0 && tb.era == 0, "Q_TOP_BAR: culture, next policy cost, faith, no golden age, ancient era");
	CHECK(tb.happiness >= -10 && tb.happiness < 100 && (tb.research_tech == -1 ? tb.research_turns == -1 : tb.research_cost > 0), "Q_TOP_BAR: happiness, research cost / turns");
	CHECK(read_top_bar(inst, human, &tb2) && memcmp(&tb, &tb2, sizeof(tb)) == 0, "Q_TOP_BAR is stable between calls (no saved state is touched)");

	/* what the city can build */
	warrior = catalog_id(inst, GC_CATALOG_UNITS, "UNIT_WARRIOR");
	worker = catalog_id(inst, GC_CATALOG_UNITS, "UNIT_WORKER");
	monument = catalog_id(inst, GC_CATALOG_BUILDINGS, "BUILDING_MONUMENT");
	library = catalog_id(inst, GC_CATALOG_BUILDINGS, "BUILDING_LIBRARY");
	CHECK(warrior >= 0 && worker >= 0 && monument >= 0 && library >= 0, "the catalog has Warrior, Worker, Monument, Library");
	bn = read_builds(inst, human, c.id, bs, rs, MAX_BUILDS);
	if (bn < 0) return;
	bw = build_of(bs, bn, GC_ORDER_TRAIN, warrior);
	bk = build_of(bs, bn, GC_ORDER_TRAIN, worker);
	bm = build_of(bs, bn, GC_ORDER_CONSTRUCT, monument);
	printf("  %d things to build; Warrior: %s turns %d cost %d gold %d; Worker: %s turns %d cost %d gold %d; Monument: %s turns %d cost %d gold %d\n", bn,
		bw ? ((bw->flags & GC_BUILD_F_ENABLED) ? "enabled" : "disabled") : "missing", bw ? bw->turns : 0, bw ? bw->production_cost : 0, bw ? bw->gold_cost : 0,
		bk ? ((bk->flags & GC_BUILD_F_ENABLED) ? "enabled" : "disabled") : "missing", bk ? bk->turns : 0, bk ? bk->production_cost : 0, bk ? bk->gold_cost : 0,
		bm ? ((bm->flags & GC_BUILD_F_ENABLED) ? "enabled" : "disabled") : "missing", bm ? bm->turns : 0, bm ? bm->production_cost : 0, bm ? bm->gold_cost : 0);
	CHECK(bw && (bw->flags & GC_BUILD_F_ENABLED) && bw->turns > 0 && bw->production_cost > 0, "Q_CITY_BUILDS: Warrior enabled, with turns and cost");
	CHECK(bk && (bk->flags & GC_BUILD_F_ENABLED) && bk->turns > 0 && bk->production_cost > 0, "Q_CITY_BUILDS: Worker enabled, with turns and cost");
	CHECK(bm && (bm->flags & GC_BUILD_F_ENABLED) && bm->turns > 0 && bm->production_cost > 0, "Q_CITY_BUILDS: Monument enabled, with turns and cost");
	{
		const gcq_city_build* bl = build_of(bs, bn, GC_ORDER_CONSTRUCT, library);
		if (bl) printf("  Library: %s%s%s\n", (bl->flags & GC_BUILD_F_ENABLED) ? "enabled" : "disabled", (bl->flags & GC_BUILD_F_ENABLED) ? "" : " reason: ", rs[bl - bs]);
		CHECK(!bl || !(bl->flags & GC_BUILD_F_ENABLED), "Q_CITY_BUILDS: the Library is not enabled yet (needs Writing)");
	}
	units_n = cost_bad = wonder_n = cur_n = 0;
	for (k = 0; k < bn; ++k) {
		if (bs[k].order_type == GC_ORDER_TRAIN) ++units_n;
		if (bs[k].order_type != GC_ORDER_MAINTAIN && bs[k].production_cost <= 0) ++cost_bad;
		if (bs[k].flags & GC_BUILD_F_WONDER) ++wonder_n;
		if (bs[k].flags & GC_BUILD_F_CURRENT) ++cur_n;
		if ((bs[k].flags & GC_BUILD_F_ENABLED) && bs[k].gold_cost > tb.gold && buy_item < 0 && bs[k].order_type == GC_ORDER_TRAIN && bs[k].item == warrior) { buy_order = bs[k].order_type; buy_item = bs[k].item; buy_cost = bs[k].gold_cost; }
		if (bs[k].gold_cost >= 0 && bs[k].gold_cost <= tb.gold && (bs[k].flags & GC_BUILD_F_CAN_PURCHASE) && afford_item < 0) { afford_order = bs[k].order_type; afford_item = bs[k].item; }
	}
	printf("  %d unit entries, %d wonder/project entries; first item above our %d gold: %d:%d costs %d; affordable: %d:%d\n", units_n, wonder_n, tb.gold, buy_order, buy_item, buy_cost, afford_order, afford_item);
	CHECK(units_n >= 3 && cost_bad == 0 && cur_n == 0, "Q_CITY_BUILDS: several units, every cost positive, nothing is the current production yet");

	/* the PRODUCTION blocker holds the turn until something is chosen */
	printf("  end-turn blocker before choosing production: %d (2 = PRODUCTION)\n", blocker_now(inst));
	CHECK(blocker_now(inst) == 2, "the new city's missing production is the end-turn blocker (PRODUCTION)");

	/* refused CITY_SET_PRODUCTION */
	CHECK(apply(inst, GC_CMD_CITY_SET_PRODUCTION, human, 2, c.id, GC_ORDER_TRAIN, 0, &err) == GC_ERR_INVALID_ARG && err.message[0] != '\0', "CITY_SET_PRODUCTION with two arguments -> INVALID_ARG");
	expect_rejected(inst, "set production of a missing city", GC_CMD_CITY_SET_PRODUCTION, human, 3, 999999, GC_ORDER_TRAIN, warrior, "no city");
	expect_rejected(inst, "set production with a bad order type", GC_CMD_CITY_SET_PRODUCTION, human, 3, c.id, 9, 0, "not a production order");
	expect_rejected(inst, "set production of an item that does not exist", GC_CMD_CITY_SET_PRODUCTION, human, 3, c.id, GC_ORDER_TRAIN, 99999, "no item");
	expect_rejected(inst, "set production of a Library (needs Writing)", GC_CMD_CITY_SET_PRODUCTION, human, 3, c.id, GC_ORDER_CONSTRUCT, library, "cannot be started");
	sprintf(msg, "set production as the AI (player %d)", ai_player);
	expect_rejected(inst, msg, GC_CMD_CITY_SET_PRODUCTION, ai_player, 3, c.id, GC_ORDER_TRAIN, warrior, "not the human");

	/* refused CITY_PURCHASE */
	CHECK(apply(inst, GC_CMD_CITY_PURCHASE, human, 3, c.id, GC_ORDER_TRAIN, warrior, &err) == GC_ERR_INVALID_ARG && err.message[0] != '\0', "CITY_PURCHASE with three arguments -> INVALID_ARG");
	expect_rejected4(inst, "purchase of a missing city", GC_CMD_CITY_PURCHASE, human, 999999, GC_ORDER_TRAIN, warrior, GC_YIELD_GOLD, "no city");
	expect_rejected4(inst, "purchase with the production yield", GC_CMD_CITY_PURCHASE, human, c.id, GC_ORDER_TRAIN, warrior, GC_YIELD_PRODUCTION, "cannot buy anything");
	expect_rejected4(inst, "purchase of a process", GC_CMD_CITY_PURCHASE, human, c.id, GC_ORDER_MAINTAIN, 0, GC_YIELD_GOLD, "cannot be bought");
	if (buy_item >= 0) {
		const gcq_city_build* bb = build_of(bs, bn, buy_order, buy_item);
		CHECK(bb && !(bb->flags & GC_BUILD_F_CAN_PURCHASE), "Q_CITY_BUILDS: an item above our gold is not can_purchase");
		expect_rejected4(inst, "purchase for more gold than we have", GC_CMD_CITY_PURCHASE, human, c.id, buy_order, buy_item, GC_YIELD_GOLD, "not enough gold");
	}
	CHECK(buy_item >= 0 || afford_item >= 0, "Q_CITY_BUILDS offers something enabled to buy");

	/* refused CITY_RANGE_ATTACK */
	CHECK(apply(inst, GC_CMD_CITY_RANGE_ATTACK, human, 2, c.id, c.x, 0, &err) == GC_ERR_INVALID_ARG && err.message[0] != '\0', "CITY_RANGE_ATTACK with two arguments -> INVALID_ARG");
	expect_rejected(inst, "city range attack of a missing city", GC_CMD_CITY_RANGE_ATTACK, human, 3, 999999, c.x + 1, c.y, "no city");
	expect_rejected(inst, "city range attack outside the map", GC_CMD_CITY_RANGE_ATTACK, human, 3, c.id, -1, 0, "outside the map");
	px = c.x + 1;
	py = c.y;
	expect_rejected(inst, "city range attack at an empty neighbouring plot", GC_CMD_CITY_RANGE_ATTACK, human, 3, c.id, px, py, "no enemy");
	expect_rejected(inst, "city range attack far away", GC_CMD_CITY_RANGE_ATTACK, human, 3, c.id, (c.x + 6) % 40, c.y, "");
	CHECK(!(cs[own].flags & GC_CITY_F_CAN_RANGE_ATTACK), "Q_CITIES: nothing to shoot at, so can_range_attack is clear");
	{
		gc_buf bt = {0, 0, 0, 0};
		gcq_header ht;
		int32_t ra[2];
		ra[0] = human; ra[1] = -c.id;
		CHECK(gc_query(inst, GC_Q_RANGE_TARGETS, ra, 2, &bt, &err) == GC_OK && gcq_check(&bt, GC_Q_RANGE_TARGETS, sizeof(gcq_range_target), "Q_RANGE_TARGETS(city)", &ht) && ht.count == 0,
			"Q_RANGE_TARGETS(-city) -> zero records (nothing to shoot at)");
		gc_buf_release(&bt);
	}

	/* refused DECLARE_WAR */
	CHECK(apply(inst, GC_CMD_DECLARE_WAR, human, 0, 0, 0, 0, &err) == GC_ERR_INVALID_ARG && err.message[0] != '\0', "DECLARE_WAR with no argument -> INVALID_ARG");
	expect_rejected(inst, "declare war on a team that does not exist", GC_CMD_DECLARE_WAR, human, 1, 999, 0, 0, "does not exist");
	expect_rejected(inst, "declare war on our own team", GC_CMD_DECLARE_WAR, human, 1, team, 0, 0, "own team");
	if (ai_team >= 0) {
		expect_rejected(inst, "declare war on a civilization not met yet", GC_CMD_DECLARE_WAR, human, 1, ai_team, 0, 0, "not been met");
		sprintf(msg, "declare war as the AI (player %d)", ai_player);
		expect_rejected(inst, msg, GC_CMD_DECLARE_WAR, ai_player, 1, team, 0, 0, "not the human");
	}

	/* digests: stable, per viewer, and they follow the state */
	CHECK(gc_state_digest(inst, -1, &d_all, &err) == GC_OK && gc_state_digest(inst, -1, &d_all2, &err) == GC_OK && d_all == d_all2 && d_all != 0, "gc_state_digest(-1) is stable between calls");
	CHECK(gc_state_digest(inst, team, &d_human, &err) == GC_OK && gc_state_digest(inst, team, &d_human2, &err) == GC_OK && d_human == d_human2 && d_human != 0, "gc_state_digest(human team) is stable between calls");
	CHECK(ai_team >= 0 && gc_state_digest(inst, ai_team, &d_ai, &err) == GC_OK && d_ai != 0, "gc_state_digest(AI team)");
	printf("  digests: whole game %016llx, human team %016llx, AI team %016llx\n", (unsigned long long)d_all, (unsigned long long)d_human, (unsigned long long)d_ai);
	CHECK(d_all != d_human && d_human != d_ai && d_all != d_ai, "the digest differs between viewers (whole game, human team, AI team)");
	d_before = d_human;

	/* CITY_SET_PRODUCTION: Warrior */
	CHECK(apply(inst, GC_CMD_CITY_SET_PRODUCTION, human, 3, c.id, GC_ORDER_TRAIN, warrior, &err) == GC_OK && err.code == GC_OK, "CITY_SET_PRODUCTION (Warrior) accepted");
	CHECK(pump_to_human(inst, &pr), "pump after CITY_SET_PRODUCTION");
	n = read_cities(inst, team, cs, MAX_CITIES, "Q_CITIES after choosing Warrior");
	c.production_order = -2;
	for (i = 0; i < n; ++i) if (cs[i].owner == human && cs[i].id == c.id) c = cs[i];
	CHECK(c.production_order == GC_ORDER_TRAIN && c.production_item == warrior && c.production_needed > 0 && c.production_turns > 0, "Q_CITIES shows the Warrior in production with its cost and turns");
	printf("  blocker after choosing production: %d\n", blocker_now(inst));
	CHECK(blocker_now(inst) != 2, "the PRODUCTION blocker is gone");
	CHECK(gc_state_digest(inst, team, &d_after, &err) == GC_OK && gc_state_digest(inst, team, &d_after2, &err) == GC_OK && d_after == d_after2 && d_after != d_before, "the digest followed the state change (new production, blocker) and is stable");
	bn = read_builds(inst, human, c.id, bs, rs, MAX_BUILDS);
	bw = bn > 0 ? build_of(bs, bn, GC_ORDER_TRAIN, warrior) : NULL;
	CHECK(bw && (bw->flags & GC_BUILD_F_CURRENT) && (bw->flags & GC_BUILD_F_ENABLED), "Q_CITY_BUILDS marks the Warrior as the current production (still enabled)");
	expect_rejected(inst, "set the same production again", GC_CMD_CITY_SET_PRODUCTION, human, 3, c.id, GC_ORDER_TRAIN, warrior, "already the current");

	/* ... and switch to the Monument: the queue is replaced */
	CHECK(apply(inst, GC_CMD_CITY_SET_PRODUCTION, human, 3, c.id, GC_ORDER_CONSTRUCT, monument, &err) == GC_OK, "CITY_SET_PRODUCTION (Monument) accepted");
	CHECK(pump_to_human(inst, &pr), "pump after switching to the Monument");
	n = read_cities(inst, team, cs, MAX_CITIES, "Q_CITIES after choosing Monument");
	for (i = 0; i < n; ++i) if (cs[i].owner == human && cs[i].id == c.id) c = cs[i];
	CHECK(c.production_order == GC_ORDER_CONSTRUCT && c.production_item == monument, "Q_CITIES shows the Monument in production");
	bn = read_builds(inst, human, c.id, bs, rs, MAX_BUILDS);
	CHECK(bn > 0 && build_of(bs, bn, GC_ORDER_CONSTRUCT, monument) && (build_of(bs, bn, GC_ORDER_CONSTRUCT, monument)->flags & GC_BUILD_F_CURRENT) &&
		build_of(bs, bn, GC_ORDER_TRAIN, warrior) && !(build_of(bs, bn, GC_ORDER_TRAIN, warrior)->flags & GC_BUILD_F_CURRENT), "only the Monument is current now");

	/* purchase accepted when the gold suffices (the start gold usually does not buy anything) */
	if (!read_top_bar(inst, human, &tb3)) return;
	if (afford_item >= 0) {
		const gcq_city_build* ab = build_of(bs, bn, afford_order, afford_item);
		int cost = ab ? ab->gold_cost : -1;
		CHECK(apply4(inst, GC_CMD_CITY_PURCHASE, human, 4, c.id, afford_order, afford_item, GC_YIELD_GOLD, &err) == GC_OK, "CITY_PURCHASE of an affordable item accepted");
		CHECK(pump_to_human(inst, &pr), "pump after CITY_PURCHASE");
		if (read_top_bar(inst, human, &tb2)) {
			printf("  gold %d -> %d (cost %d)\n", tb3.gold, tb2.gold, cost);
			CHECK(tb2.gold == tb3.gold - cost, "the purchase took exactly its cost from the gold");
		}
	} else
		printf("  (nothing is affordable with %d gold: the accepted-purchase path is not exercised)\n", tb3.gold);

	check_foreign_cities(inst, human, team, ai_player, ai_team);
}

/* Q_NOTIFICATIONS(human) lists exactly the notifications the events announced and did not remove. */
static void check_notifications_live(gc_instance* inst, int human, const char* when)
{
	gc_buf bn = {0, 0, 0, 0};
	gcq_header hn;
	gc_error err;
	int32_t a[1];
	uint32_t i;
	int matched = 0, text_ok = 1;
	char msg[200];
	a[0] = human;
	if (gc_query(inst, GC_Q_NOTIFICATIONS, a, 1, &bn, &err) != GC_OK || !gcq_check(&bn, GC_Q_NOTIFICATIONS, sizeof(gcq_notification), "Q_NOTIFICATIONS(human)", &hn)) { CHECK(0, "Q_NOTIFICATIONS(human)"); gc_buf_release(&bn); return; }
	for (i = 0; i < hn.count; ++i) {
		gcq_notification n;
		int k;
		gcq_rec(&bn, &hn, i, &n, sizeof(n));
		for (k = 0; k < g_ev_live_n; ++k) if (g_ev_live[k] == n.id) ++matched;
		if (gcq_str(&bn, &hn, n.message)[0] == '\0' || strncmp(gcq_str(&bn, &hn, n.message), "TXT_KEY_", 8) == 0) text_ok = 0;
	}
	sprintf(msg, "Q_NOTIFICATIONS(human) %s: %u record(s) = the %d live by the events, all with localized text", when, (unsigned)hn.count, g_ev_live_n);
	CHECK(hn.count == (uint32_t)g_ev_live_n && matched == (int)hn.count && text_ok, msg);
	gc_buf_release(&bn);
}

/* F2 + F3 + F7: ends several turns by END_TURN (production and unit orders given when the core asks, research chosen by the runtime) and
 * watches the event stream and Q_NOTIFICATIONS across them. check_foreign_cities (run_city_checks) ends turns itself the same way. */
#define SMOKE_TURNS 8
static void run_turn_checks(gc_instance* inst)
{
	gcq_game g;
	gcq_header hg;
	gc_buf bg = {0, 0, 0, 0};
	gc_error err;
	gc_pump_result pr;
	int human, team, t, turn0, advanced = 0, turn_ev0, added0, ok = 1;
	char msg[200];
	printf("-- ending turns (END_TURN, orders, auto-choices, events)\n");
	if (gc_query(inst, GC_Q_GAME, NULL, 0, &bg, &err) != GC_OK || !gcq_check(&bg, GC_Q_GAME, sizeof(gcq_game), "Q_GAME", &hg)) { CHECK(0, "Q_GAME"); gc_buf_release(&bg); return; }
	gcq_rec(&bg, &hg, 0, &g, sizeof(g));
	gc_buf_release(&bg);
	human = g.human_player;
	team = g.human_team;
	turn0 = g.turn;
	turn_ev0 = g_turn_events;
	added0 = g_ev_count[GC_EVENT_NOTIFICATION_ADDED];
	for (t = 0; t < SMOKE_TURNS; ++t) {
		const int before = turn0 + advanced, events_before = g_turn_events;
		if (!end_turn_resolving(inst, human, team, &pr)) { ok = 0; break; }
		++advanced;
		if (pr.turn != before + 1 || g_turn_events != events_before + 1 || g_last_turn_event != pr.turn) { ok = 0; break; }
		printf("  turn %d -> %d: status %d blocker %d, events so far: notification_added %d removed %d auto_choice %d\n", before, pr.turn, pr.status, pr.blocker,
			g_ev_count[GC_EVENT_NOTIFICATION_ADDED], g_ev_count[GC_EVENT_NOTIFICATION_REMOVED], g_ev_count[GC_EVENT_AUTO_CHOICE]);
		sprintf(msg, "after turn %d", pr.turn);
		check_notifications_live(inst, human, msg);
	}
	sprintf(msg, "END_TURN ends %d turns, each one TURN_STARTED event and the human up again", SMOKE_TURNS);
	CHECK(ok && advanced == SMOKE_TURNS && g_turn_events - turn_ev0 == SMOKE_TURNS, msg);
	CHECK(g_ev_count[GC_EVENT_NOTIFICATION_ADDED] > added0, "the turns brought NOTIFICATION_ADDED events (with text and text2)");
	CHECK(g_ev_text_ok == g_ev_count[GC_EVENT_NOTIFICATION_ADDED], "every NOTIFICATION_ADDED so far has non-empty text and text2");
	CHECK(g_ev_count[GC_EVENT_AUTO_CHOICE] > 0, "AUTO_CHOICE events arrived (the runtime chose the research)");
	CHECK(g_ac_bad == 0 && g_ac_first_text[0] != '\0', "AUTO_CHOICE: kind 0..4, player, item and the journal text");
	printf("  first auto-choice: \"%s\"; AUTO_CHOICE events %d\n", g_ac_first_text, g_ev_count[GC_EVENT_AUTO_CHOICE]);
	CHECK(g_ev_bad == 0, "no event broke the contract over the turns");
	CHECK(g_ev_count[GC_EVENT_LUA_ERROR] == 0, "no LUA_ERROR event over the turns");
}

/* F7: events 2-10 seen so far and Q_NOTIFICATIONS of the human at the point the pump stopped. */
static void run_event_checks(gc_instance* inst, const gc_pump_result* pr)
{
	gc_error err;
	gc_buf bn = {0, 0, 0, 0};
	gcq_header hn;
	int32_t args[1];
	uint32_t i;
	int live_match = 0, strings_ok = 1, plots_ok = 1, ids_ascending = 1, found_first = 0, text_match = 0;
	int32_t prev_id = -1;
	printf("-- events and Q_NOTIFICATIONS\n");
	printf("  events: player_turn_started %d, notification_added %d, notification_removed %d, popup_info %d, diplo %d, war_state %d, lua_error %d, game_over %d\n",
		g_ev_count[GC_EVENT_PLAYER_TURN_STARTED], g_ev_count[GC_EVENT_NOTIFICATION_ADDED], g_ev_count[GC_EVENT_NOTIFICATION_REMOVED], g_ev_count[GC_EVENT_POPUP_INFO],
		g_ev_count[GC_EVENT_DIPLO_AUTO_DECLINED], g_ev_count[GC_EVENT_WAR_STATE], g_ev_count[GC_EVENT_LUA_ERROR], g_ev_count[GC_EVENT_GAME_OVER]);
	CHECK(g_ev_bad == 0, "events 1-11 have the argc / text the spec gives (no unknown kind, no missing args or text)");
	CHECK(g_ev_count[GC_EVENT_PLAYER_TURN_STARTED] >= 1 && g_ev_first_player == pr->active_player, "PLAYER_TURN_STARTED for the human (args = the active player)");
	CHECK(g_ev_count[GC_EVENT_LUA_ERROR] == 0, "no LUA_ERROR event");
	/* The human's first turn may have no notification (seed 12345 has none); the turns with the research / production ones need
	 * GC_CMD_END_TURN (F2). The `humanturn` scenario covers them over 30 turns. */
	CHECK(g_ev_text_ok == g_ev_count[GC_EVENT_NOTIFICATION_ADDED], "every NOTIFICATION_ADDED has a localized message (text) and summary (text2)");
	CHECK(g_ev_first_text[0] != '0' && strncmp(g_ev_first_text, "TXT_KEY_", 8) != 0, "the first notification text is localized, not a text key");
	printf("  first notification #%d: \"%s\" / \"%s\"\n", g_ev_first_id, g_ev_first_text2, g_ev_first_text);

	args[0] = pr->active_player;
	memset(&err, 0, sizeof(err));
	CHECK(gc_query(inst, GC_Q_NOTIFICATIONS, args, 1, &bn, &err) == GC_OK && err.code == GC_OK, "Q_NOTIFICATIONS(human)");
	if (gcq_check(&bn, GC_Q_NOTIFICATIONS, sizeof(gcq_notification), "Q_NOTIFICATIONS(human)", &hn)) {
		printf("  Q_NOTIFICATIONS(human): %u record(s), %d live by the events\n", (unsigned)hn.count, g_ev_live_n);
		CHECK(hn.count == (uint32_t)g_ev_live_n, "Q_NOTIFICATIONS count equals added minus removed events");
		for (i = 0; i < hn.count; ++i) {
			gcq_notification n;
			int k, listed = 0;
			gcq_rec(&bn, &hn, i, &n, sizeof(n));
			if (!gcq_str_ok(&bn, &hn, n.message) || !gcq_str_ok(&bn, &hn, n.summary) || n.message == GCQ_NO_STRING || n.summary == GCQ_NO_STRING) strings_ok = 0;
			if (n.id <= prev_id) ids_ascending = 0;
			prev_id = n.id;
			if (n.dismissed != 0 || n.turn < 0) plots_ok = 0;
			for (k = 0; k < g_ev_live_n; ++k) if (g_ev_live[k] == n.id) listed = 1;
			if (listed) ++live_match;
			if (n.id == g_ev_first_id) {
				found_first = 1;
				text_match = strcmp(gcq_str(&bn, &hn, n.message), g_ev_first_text) == 0 && strcmp(gcq_str(&bn, &hn, n.summary), g_ev_first_text2) == 0;
			}
			if (i < 6) printf("    #%d type %08x at (%d,%d) data %d %d turn %d: \"%s\" / \"%s\"\n", (int)n.id, (unsigned)n.type, (int)n.x, (int)n.y, (int)n.data1, (int)n.data2, (int)n.turn,
				gcq_str(&bn, &hn, n.summary), gcq_str(&bn, &hn, n.message));
		}
		CHECK(live_match == (int)hn.count, "every Q_NOTIFICATIONS id was announced by a NOTIFICATION_ADDED event and not removed since");
		CHECK(strings_ok && ids_ascending && plots_ok, "Q_NOTIFICATIONS: strings inside the table, ids ascending, dismissed 0, turn set");
		CHECK(found_first ? text_match : 1, "Q_NOTIFICATIONS text equals the event text / text2");
	}
	gc_buf_release(&bn);
	args[0] = (pr->active_player + 1) % 4;
	if (args[0] == pr->active_player) args[0] = (args[0] + 1) % 4;
	CHECK(gc_query(inst, GC_Q_NOTIFICATIONS, args, 1, &bn, &err) == GC_OK && gcq_check(&bn, GC_Q_NOTIFICATIONS, sizeof(gcq_notification), "Q_NOTIFICATIONS(other player)", &hn) && hn.count == 0,
		"Q_NOTIFICATIONS of another player is empty");
	gc_buf_release(&bn);
	CHECK(gc_query(inst, GC_Q_NOTIFICATIONS, NULL, 0, &bn, &err) == GC_ERR_INVALID_ARG, "Q_NOTIFICATIONS without player -> INVALID_ARG");
	args[0] = 100000;
	CHECK(gc_query(inst, GC_Q_NOTIFICATIONS, args, 1, &bn, &err) == GC_ERR_INVALID_ARG, "Q_NOTIFICATIONS player 100000 -> INVALID_ARG");
	args[0] = -1;
	CHECK(gc_query(inst, GC_Q_NOTIFICATIONS, args, 1, &bn, &err) == GC_ERR_INVALID_ARG, "Q_NOTIFICATIONS player -1 -> INVALID_ARG");
}

int main(int argc, char** argv)
{
	gc_host host = {NULL, host_log, host_read_file, host_event};
	gc_host no_read = {NULL, host_log, NULL, host_event};
	gc_instance* inst;
	gc_error err;
	gc_pump_result pr;
	gc_command cmd;
	gc_buf buf = {0, 0, 0, 0};
	uint64_t d1 = 0, d2 = 0, d3 = 0;
	int32_t rc;
	int i, start_turn, calls;
	static const uint8_t junk[8] = {'n', 'o', 'p', 'e', 0, 0, 0, 0};

	for (i = 1; i < argc; ++i)
		if (!strcmp(argv[i], "--game") && i + 1 < argc) g_game_dir = argv[++i];
	setvbuf(stdout, NULL, _IOLBF, 0);

	printf("gamecore_abi version %u\n", gc_abi_version());
	CHECK(gc_abi_version() == GC_ABI_VERSION && GC_ABI_VERSION == 2, "gc_abi_version is 2");
	CHECK(GC_EVENT_PLAYER_TURN_ENDED == 12 && GC_EVENT_SLOT_CHANGED == 13 && GC_EVENT_HUMAN_PHASE_STARTED == 14 && GC_EVENT_HUMAN_WAR_DECLARED == 15, "v2 event kinds 12..15");
	CHECK(GC_CMD_MAKE_PEACE == 31 && GC_CMD_CANCEL_END_TURN == 42 && GC_CMD_END_TURN_TIMEOUT == 43, "v2 command kinds 31, 42, 43");
	CHECK(GC_Q_TURN == 14 && GC_Q_PLOTS_DIRTY == 21 && GC_REASON_SLOT_STATE == 11 && GC_SLOT_KICK == 3, "v2 query kinds 14..21, reasons 0..11, slot ops 1..3");
	if (!g_game_dir) { printf("smoke: --game GAMEDATA_DIR is required\nSMOKE FAIL (1)\n"); return 1; }

	printf("-- create\n");
	memset(&err, 0x55, sizeof(err));
	CHECK(gc_create(NULL, &err) == NULL && err.code == GC_ERR_INVALID_ARG && err.message[0] != '\0', "gc_create(NULL host) -> INVALID_ARG with a message");
	CHECK(gc_create(&no_read, &err) == NULL && err.code == GC_ERR_INVALID_ARG, "gc_create without read_file -> INVALID_ARG");
	CHECK(gc_create(NULL, NULL) == NULL, "gc_create tolerates err == NULL");
	memset(&err, 0x55, sizeof(err));
	inst = gc_create(&host, &err);
	CHECK(inst != NULL && err.code == GC_OK && err.message[0] == '\0', "gc_create boots the core (DB, localization, DLL context)");
	if (!inst) { printf("  create error: %d %s\n", err.code, err.message); printf("SMOKE FAIL (%d)\n", g_failures); return 1; }
	CHECK(gc_create(&host, &err) == NULL && err.code == GC_ERR_ALREADY_EXISTS, "second gc_create -> ALREADY_EXISTS (one instance per process)");

	printf("-- before a game\n");
	memset(&pr, 0, sizeof(pr));
	CHECK(gc_pump(inst, 10, &pr, &err) == GC_ERR_NOT_STARTED && err.code == GC_ERR_NOT_STARTED, "gc_pump -> NOT_STARTED");
	CHECK(gc_state_digest(inst, -1, &d1, &err) == GC_ERR_NOT_STARTED, "gc_state_digest -> NOT_STARTED");
	CHECK(gc_save(inst, &buf, &err) == GC_ERR_NOT_STARTED, "gc_save -> NOT_STARTED");
	CHECK(gc_query(inst, GC_Q_GAME, NULL, 0, &buf, &err) == GC_ERR_NOT_STARTED, "gc_query -> NOT_STARTED");
	{
		/* the lobby asks for civilizations and handicaps before the game exists */
		int32_t cat[1];
		gc_buf cb = {0, 0, 0, 0};
		cat[0] = GC_CATALOG_CIVILIZATIONS;
		CHECK(gc_query(inst, GC_Q_CATALOG, cat, 1, &cb, &err) == GC_OK && cb.data != NULL && cb.len > 0, "Q_CATALOG(civilizations) answers before gc_new_game");
		gc_buf_release(&cb);
		cat[0] = GC_CATALOG_HANDICAPS;
		CHECK(gc_query(inst, GC_Q_CATALOG, cat, 1, &cb, &err) == GC_OK && cb.data != NULL, "Q_CATALOG(handicaps) answers before gc_new_game");
		gc_buf_release(&cb);
	}
	memset(&cmd, 0, sizeof(cmd));
	cmd.kind = GC_CMD_END_TURN;
	CHECK(gc_apply_command(inst, &cmd, &err) == GC_ERR_NOT_STARTED, "gc_apply_command -> NOT_STARTED");
	CHECK(gc_load_game(inst, junk, sizeof(junk), &err) == GC_ERR_BAD_DATA && err.message[0] != '\0', "gc_load_game(junk) -> BAD_DATA with a reason");
	CHECK(gc_load_game(inst, NULL, 4, &err) == GC_ERR_INVALID_ARG, "gc_load_game(NULL, 4) -> INVALID_ARG");

	printf("-- new game\n");
	CHECK(gc_new_game(inst, NULL, &err) == GC_ERR_INVALID_ARG, "gc_new_game(NULL json) -> INVALID_ARG");
	CHECK(gc_new_game(NULL, "{}", &err) == GC_ERR_INVALID_ARG, "gc_new_game(NULL instance) -> INVALID_ARG");
	rc = gc_new_game(inst, "{\"seed\":1,\"no_such_key\":1}", &err);
	CHECK(rc == GC_ERR_INVALID_ARG && strstr(err.message, "no_such_key") != NULL, "unknown setup key -> INVALID_ARG naming the key");
	rc = gc_new_game(inst, "{\"seed\":\"x\"}", &err);
	CHECK(rc == GC_ERR_INVALID_ARG, "wrongly typed setup value -> INVALID_ARG");
	rc = gc_new_game(inst, "{\"seed\":1,\"world_size\":\"WORLDSIZE_NOPE\"}", &err);
	CHECK(rc == GC_ERR_INVALID_ARG && strstr(err.message, "world_size") != NULL && strstr(err.message, "WORLDSIZE_NOPE") != NULL, "unknown world_size -> INVALID_ARG naming key and value");
	rc = gc_new_game(inst, "{\"seed\":1,\"start_era\":\"ERA_NOPE\"}", &err);
	CHECK(rc == GC_ERR_INVALID_ARG && strstr(err.message, "ERA_NOPE") != NULL, "unknown start_era -> INVALID_ARG");
	rc = gc_new_game(inst, "{\"seed\":1,\"handicap\":\"HANDICAP_NOPE\"}", &err);
	CHECK(rc == GC_ERR_INVALID_ARG && strstr(err.message, "HANDICAP_NOPE") != NULL, "unknown handicap -> INVALID_ARG");
	rc = gc_new_game(inst, "{\"seed\":1,\"game_speed\":\"GAMESPEED_NOPE\"}", &err);
	CHECK(rc == GC_ERR_INVALID_ARG && strstr(err.message, "GAMESPEED_NOPE") != NULL, "unknown game_speed -> INVALID_ARG");
	rc = gc_new_game(inst, "{\"seed\":1,\"human\":{\"civ\":\"CIVILIZATION_NOPE\"}}", &err);
	CHECK(rc == GC_ERR_INVALID_ARG && strstr(err.message, "CIVILIZATION_NOPE") != NULL, "unknown human civ -> INVALID_ARG");
	rc = gc_new_game(inst, "{\"seed\":1,\"map_script\":\"Assets/Maps/nope.lua\"}", &err);
	CHECK(rc == GC_ERR_INVALID_ARG && strstr(err.message, "map_script") != NULL, "unknown map_script -> INVALID_ARG naming the key");
	rc = gc_new_game(inst, "{\"game_type\":\"single\",\"seed\":12345,\"ai_count\":1,\"world_size\":\"WORLDSIZE_DUEL\"}", &err);
	if (rc != GC_OK) printf("  new_game error: %d %s\n", err.code, err.message);
	CHECK(rc == GC_OK && err.code == GC_OK, "gc_new_game on the real game data (a bad setup did not use up the slot)");
	if (rc != GC_OK) { gc_destroy(inst); printf("SMOKE FAIL (%d)\n", g_failures); return 1; }
	CHECK(gc_new_game(inst, "{\"game_type\":\"single\",\"seed\":1}", &err) == GC_ERR_ALREADY_EXISTS, "second gc_new_game -> ALREADY_EXISTS");
	CHECK(gc_load_game(inst, junk, sizeof(junk), &err) == GC_ERR_ALREADY_EXISTS, "gc_load_game after gc_new_game -> ALREADY_EXISTS");

	printf("-- commands while the AI is moving\n");
	check_not_humans_turn(inst, "before the first pump");

	printf("-- pump\n");
	CHECK(gc_pump(inst, 0, &pr, &err) == GC_ERR_INVALID_ARG, "gc_pump(0 slices) -> INVALID_ARG");
	CHECK(gc_pump(inst, 1, NULL, &err) == GC_ERR_INVALID_ARG, "gc_pump(out == NULL) -> INVALID_ARG");
	memset(&pr, 0, sizeof(pr));
	rc = gc_pump(inst, 1, &pr, &err);
	CHECK(rc == GC_OK && pr.slices_used == 1 && pr.turn >= 0, "gc_pump(1 slice) runs exactly one slice");
	start_turn = pr.turn;
	printf("  start: turn %d status %d active_player %d blocker %d\n", pr.turn, pr.status, pr.active_player, pr.blocker);
	if (pr.status == GC_PUMP_AI_RUNNING) check_not_humans_turn(inst, "after one slice");
	for (calls = 0; rc == GC_OK && pr.status == GC_PUMP_AI_RUNNING && pr.turn == start_turn && calls < 2000; ++calls) {
		rc = gc_pump(inst, 50, &pr, &err);
	}
	printf("  after %d more call(s): turn %d status %d active_player %d blocker %d slices_used %d\n", calls, pr.turn, pr.status, pr.active_player,
		pr.blocker, pr.slices_used);
	if (rc != GC_OK) printf("  pump error: %d %s\n", err.code, err.message);
	CHECK(rc == GC_OK, "gc_pump returns OK until the human is up or the turn changes");
	CHECK(pr.status == GC_PUMP_WAITING_HUMAN || pr.turn != start_turn, "reached WAITING_HUMAN or a turn change");
	CHECK(pr.status != GC_PUMP_STUCK && pr.status != GC_PUMP_GAME_OVER, "status is neither STUCK nor GAME_OVER");
	CHECK(pr.active_player >= 0, "active_player is the human");
	CHECK(pr.humans_left == (pr.status == GC_PUMP_WAITING_HUMAN ? 1 : 0), "humans_left is 1 while the human is waited for, else 0");
	CHECK(pr.turn == start_turn ? g_turn_events == 0 : (g_turn_events == pr.turn - start_turn && g_last_turn_event == pr.turn),
		"TURN_STARTED events match the turn changes");
	CHECK(g_game_over_events == 0, "no GAME_OVER event");

	printf("-- digest\n");
	CHECK(gc_state_digest(inst, -1, &d1, &err) == GC_OK && d1 != 0, "gc_state_digest(whole game)");
	CHECK(gc_state_digest(inst, -1, &d2, &err) == GC_OK && d2 == d1, "gc_state_digest is stable between calls");
	CHECK(gc_state_digest(inst, -2, &d2, &err) == GC_OK && d2 != 0, "gc_state_digest(viewer -2): the simulation digest");
	CHECK(gc_state_digest(inst, -2, &d3, &err) == GC_OK && d3 == d2, "gc_state_digest(viewer -2) is stable between calls");
	CHECK(gc_state_digest(inst, -3, &d3, &err) == GC_OK && d3 != 0, "gc_state_digest(viewer -3): the replica digest (B4 gc-replica)");
	CHECK(gc_state_digest(inst, -4, &d3, &err) == GC_ERR_INVALID_ARG, "gc_state_digest(viewer -4) -> INVALID_ARG");
	CHECK(gc_state_digest(inst, 100000, &d2, &err) == GC_ERR_INVALID_ARG, "gc_state_digest(viewer 100000) -> INVALID_ARG");
	CHECK(gc_state_digest(inst, -1, NULL, &err) == GC_ERR_INVALID_ARG, "gc_state_digest(out == NULL) -> INVALID_ARG");
	printf("  state digest %016llx\n", (unsigned long long)d1);

	run_query_checks(inst, &pr);
	run_command_checks(inst, &pr);
	run_city_checks(inst);
	run_turn_checks(inst);
	run_event_checks(inst, &pr);

	printf("-- staged entries\n");
	memset(&err, 0, sizeof(err));
	rc = gc_save(inst, &buf, &err);
	/* G4a: GCS1 bytes at a save point, otherwise REJECTED with a reason and the buffer untouched (scenario saveload covers the rest). */
	CHECK((rc == GC_OK && buf.len > 32 && memcmp(buf.data, "GCS1", 4) == 0) || (rc == GC_ERR_REJECTED && err.message[0] != '\0' && buf.data == NULL),
		"gc_save -> GCS1 bytes at a save point or REJECTED with a reason");
	gc_buf_release(&buf);
	CHECK(gc_query(inst, GC_Q_GAME, NULL, 1, &buf, &err) == GC_ERR_INVALID_ARG, "gc_query(argc 1, args NULL) -> INVALID_ARG");
	cmd.kind = GC_CMD_CITY_SET_PRODUCTION;
	CHECK(gc_apply_command(inst, &cmd, &err) == GC_ERR_INVALID_ARG, "gc_apply_command(CITY_SET_PRODUCTION, no arguments) -> INVALID_ARG");
	CHECK(gc_apply_command(inst, NULL, &err) == GC_ERR_INVALID_ARG, "gc_apply_command(NULL) -> INVALID_ARG");
	CHECK(gc_lua_register(inst, "T", "f", host_lua_fn, NULL, &err) == GC_ERR_NOT_IMPLEMENTED, "gc_lua_register -> NOT_IMPLEMENTED");
	/* gc_slot_control (P8, N3.12; the slot changes themselves are scenario mpslots): argument errors first; this game is a single-type one, which has no slots to change:
	 * REJECTED with GC_REASON_SLOT_STATE and a message. */
	memset(&err, 0x55, sizeof(err));
	CHECK(gc_slot_control(inst, 0, GC_SLOT_KICK, NULL, &err) == GC_ERR_REJECTED && err.code == GC_ERR_REJECTED && err.reason == GC_REASON_SLOT_STATE && err.message[0] != '\0',
		"gc_slot_control in a single-type game -> REJECTED, reason SLOT_STATE, with a message");
	CHECK(gc_slot_control(NULL, 0, GC_SLOT_KICK, NULL, &err) == GC_ERR_INVALID_ARG, "gc_slot_control(NULL instance) -> INVALID_ARG");
	CHECK(gc_slot_control(inst, -1, GC_SLOT_KICK, NULL, &err) == GC_ERR_INVALID_ARG, "gc_slot_control(player -1) -> INVALID_ARG");
	CHECK(gc_slot_control(inst, 0, 99, NULL, &err) == GC_ERR_INVALID_ARG, "gc_slot_control(op 99) -> INVALID_ARG");
	memset(&err, 0x55, sizeof(err));
	CHECK(gc_lua_register(inst, "T", "f", host_lua_fn, NULL, &err) == GC_ERR_NOT_IMPLEMENTED && err.reason == GC_REASON_NONE, "gc_error.reason is 0 on errors without a reason");

	printf("-- buffers\n");
	buf.data = (uint8_t*)"x";
	buf.len = 1;
	buf.release = release_static;
	buf.owner = NULL;
	gc_buf_release(&buf);
	CHECK(buf.data == NULL && buf.len == 0 && buf.release == NULL && buf.owner == NULL, "gc_buf_release calls release then zeroes the buffer");
	gc_buf_release(&buf);
	gc_buf_release(NULL);
	CHECK(1, "gc_buf_release is safe on a zeroed buffer and on NULL");

	printf("-- destroy\n");
	gc_destroy(inst);
	gc_destroy(NULL);
	CHECK(1, "gc_destroy (and gc_destroy(NULL))");
	CHECK(g_log_errors == 0, "no error-level log lines");
	(void)g_other_events;

	printf(g_failures ? "SMOKE FAIL (%d)\n" : "SMOKE PASS\n", g_failures);
	return g_failures ? 1 : 0;
}
