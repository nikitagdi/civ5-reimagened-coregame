// Scenario `mpslots` (task N3.12; run.sh --scenario mpslots --humans 3 --ai 1 --turns 35): who plays a slot, through the C ABI only, as the game server
// drives it: a network game with `humans` test-bot humans (setup "autoplay_humans") and `ai` AI. gc_slot_control at safe points:
//  - turn 3: the refusals of the state (KICK of an AI slot, of the server seat and of a closed slot, CONNECT onto an AI slot without a nick, bad arguments);
//  - turn 5: not at a safe point after a command: REJECTED / NOT_SAFE_POINT, and the safe point is back after a gc_pump;
//  - turn 10: DISCONNECT of human 1: the slot stays human, Q_TURN shows it off line (the autopause flag), the turn does not end however long gc_pump runs (humans_left 1, the
//    others have ended); CONNECT brings it back (same turn, the spec's "turn 12" is not reachable while the turn waits) and the turn goes on;
//  - turn 20: DISCONNECT of human 2, the turn waits; KICK: the slot is the AI's (SS_COMPUTER, the AI's handicap, KICKED, not human), a slot change before the next gc_pump
//    is NOT_SAFE_POINT, the AI plays the rest of the turn and the turn ends without the kicked human's end of turn;
//  - turn 25 and 30: CONNECT with a new nick onto the kicked human's slot (25) and onto the original AI's slot (30): SS_TAKEN, the nick, the handicap of the humans, no kick, the
//    end of the running turn is marked (PLAYER_TURN_ENDED, the turn does not hang), and from the next turn the new human plays (its test bot ends turns).
// Every call leaves the server seat the active player; one SLOT_CHANGED event per successful call; "mpslots-digest|turn|hex" lines (RtSession::StateDigest) let run.sh compare two
// runs of one seed turn by turn.
#include "CvGameCoreDLLPCH.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <string>
#include <vector>

#include "../abi/gc_internal.h"
#include "rt_active.h"
#include "rt_autoplay.h"
#include "rt_selftest.h"
#include "rt_selftest_slots.h"
#include "rt_session.h"

using namespace lekmod_portable;

namespace {

std::string Format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string Format(const char* fmt, ...)
{
	char buf[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return buf;
}

std::string Escape(const std::string& s)
{
	std::string out;
	for (size_t i = 0; i < s.size(); ++i)
	{
		const char ch = s[i];
		if (ch == '"' || ch == '\\') { out += '\\'; out += ch; }
		else if ((unsigned char)ch >= 0x20) out += ch;
	}
	return out;
}

std::string Num(long long v) { return Format("%lld", v); }

struct Check
{
	std::string json;
	int failed;
	Check() : failed(0) {}
	void add(const char* name, bool ok, const char* fmt, ...) __attribute__((format(printf, 4, 5)))
	{
		char detail[2048];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(detail, sizeof(detail), fmt, ap);
		va_end(ap);
		json += std::string(json.empty() ? "" : ",") + "{\"check\":\"" + name + "\",\"ok\":" + (ok ? "true" : "false") + ",\"detail\":\"" + Escape(detail) + "\"}";
		printf("mpslots: %-4s %s: %s\n", ok ? "ok" : "FAIL", name, detail);
		if (!ok) ++failed;
	}
};

struct SlotEvent
{
	int player, slot, connected, kicked;
	std::string nick;
};

struct Seen
{
	const gc_host* outer;
	int logErrors, logWarnings, luaErrors;
	int events[32];
	std::vector<SlotEvent> slotEvents;
	std::vector<std::pair<int, int> > turnEnded;  // event 12: player, ended
	std::vector<std::string> lines;
	Seen() : outer(NULL), logErrors(0), logWarnings(0), luaErrors(0) { memset(events, 0, sizeof(events)); }
};

void OnLog(void* user, int32_t level, const char* msg)
{
	Seen* s = static_cast<Seen*>(user);
	if (level == GC_LOG_ERROR) { ++s->logErrors; if (s->lines.size() < 20) s->lines.push_back(std::string("log error: ") + msg); }
	if (level == GC_LOG_WARN) ++s->logWarnings;
	if (s->outer && s->outer->log) s->outer->log(s->outer->user, level, msg);
}

int32_t OnReadFile(void* user, const char* path, gc_buf* out)
{
	Seen* s = static_cast<Seen*>(user);
	return s->outer->read_file(s->outer->user, path, out);
}

void OnEvent(void* user, const gc_event* ev)
{
	Seen* s = static_cast<Seen*>(user);
	if (ev->kind >= 0 && ev->kind < 32) ++s->events[ev->kind];
	if (ev->kind == GC_EVENT_SLOT_CHANGED && ev->argc >= 4)
	{
		SlotEvent e;
		e.player = ev->args[0];
		e.slot = ev->args[1];
		e.connected = ev->args[2];
		e.kicked = ev->args[3];
		e.nick = ev->text ? ev->text : "";
		s->slotEvents.push_back(e);
	}
	if (ev->kind == GC_EVENT_PLAYER_TURN_ENDED && ev->argc >= 2) s->turnEnded.push_back(std::make_pair(ev->args[0], ev->args[1]));
	if (ev->kind == GC_EVENT_LUA_ERROR)
	{
		++s->luaErrors;
		if (s->lines.size() < 20) s->lines.push_back(std::string("lua error: ") + (ev->text ? ev->text : ""));
	}
}

gc_command Cmd(int kind, int player)
{
	gc_command c;
	memset(&c, 0, sizeof(c));
	c.kind = (uint16_t)kind;
	c.player = (uint8_t)player;
	return c;
}

bool GetGame(gc_instance* inst, gcq_game* out)
{
	gc_buf b = {0, 0, 0, 0};
	gc_error err;
	memset(&err, 0, sizeof(err));
	const int32_t rc = gc_query(inst, GC_Q_GAME, NULL, 0, &b, &err);
	const bool ok = rc == GC_OK && b.len >= sizeof(gcq_header) + sizeof(gcq_game);
	if (ok) memcpy(out, b.data + sizeof(gcq_header), sizeof(*out));
	gc_buf_release(&b);
	return ok;
}

// The Q_TURN record of a player; false when there is none.
bool GetTurn(gc_instance* inst, int player, gcq_turn* out)
{
	gc_buf b = {0, 0, 0, 0};
	gc_error err;
	memset(&err, 0, sizeof(err));
	bool found = false;
	if (gc_query(inst, GC_Q_TURN, NULL, 0, &b, &err) == GC_OK && b.len >= sizeof(gcq_header))
	{
		gcq_header h;
		memcpy(&h, b.data, sizeof(h));
		for (uint32_t k = 0; k < h.count && h.record_size >= sizeof(gcq_turn); ++k)
		{
			gcq_turn r;
			memcpy(&r, b.data + sizeof(h) + (size_t)k * h.record_size, sizeof(r));
			if (r.player == player) { *out = r; found = true; }
		}
	}
	gc_buf_release(&b);
	return found;
}

struct Driver
{
	gc_instance* inst;
	RtSession* session;
	Seen* seen;
	int seat;
	int activeBroken;
	int stuck, gameOver, pumpErrors;
	long long calls;
	std::string stop;
	gc_pump_result pr;
	gc_error err;
	std::vector<std::string> digestLines;
	int lastDigestTurn, lastUnsafe, lastUnsafeTurn;

	Driver() : inst(NULL), session(NULL), seen(NULL), seat(-1), activeBroken(0), stuck(0), gameOver(0), pumpErrors(0), calls(0), lastDigestTurn(-1), lastUnsafe(-1), lastUnsafeTurn(-1) { memset(&pr, 0, sizeof(pr)); memset(&err, 0, sizeof(err)); }

	// One gc_pump call; false (and stop set) on a STUCK, GAME_OVER or error.
	bool Pump(uint32_t slices = 64)
	{
		++calls;
		const int32_t rc = gc_pump(inst, slices, &pr, &err);
		if (CvPreGame::activePlayer() != seat) ++activeBroken;
		if (pr.status == GC_PUMP_STUCK) { ++stuck; stop = Format("STUCK at turn %d: %s", pr.turn, err.message); return false; }
		if (pr.status == GC_PUMP_GAME_OVER) { ++gameOver; stop = Format("GAME_OVER at turn %d", pr.turn); return false; }
		if (rc != GC_OK) { ++pumpErrors; stop = Format("gc_pump at turn %d: %d %s", pr.turn, rc, err.message); return false; }
		if (pr.turn != lastDigestTurn)
		{
			lastDigestTurn = pr.turn;
			printf("mpslots-digest|%d|%016llx\n", pr.turn, (unsigned long long)RtSession::StateDigest());
		}
		return true;
	}

	// gc_pump until the game turn is `turn` or later and the game is at a safe point (Q_GAME.safe_point): the start of that turn's human phase.
	bool ToSafePoint(int turn)
	{
		for (int i = 0; i < 40000; ++i)
		{
			gcq_game g;
			if (GC.getGame().getGameTurn() >= turn && GetGame(inst, &g) && g.safe_point == 1 && g.phase == GC_PHASE_HUMANS) return true;
			if (GC.getGame().getGameTurn() >= turn && GetGame(inst, &g) && g.phase == GC_PHASE_HUMANS && (g.unsafe_reason != lastUnsafe || GC.getGame().getGameTurn() != lastUnsafeTurn))
			{
				// diagnostics: the human phase without a safe point (the reason, once per turn and reason)
				lastUnsafe = g.unsafe_reason;
				lastUnsafeTurn = GC.getGame().getGameTurn();
				printf("mpslots-unsafe|turn %d|reason %d|status %d|humans_left %d\n", lastUnsafeTurn, (int)g.unsafe_reason, (int)pr.status, (int)g.humans_left);
			}
			if (!Pump()) return false;
		}
		stop = Format("no safe point at turn %d after 40000 gc_pump calls", turn);
		return false;
	}

	int32_t Slot(int player, int op, const char* nick)
	{
		const int32_t rc = gc_slot_control(inst, player, op, nick, &err);
		if (CvPreGame::activePlayer() != seat) ++activeBroken;
		return rc;
	}
};

}  // namespace

extern "C" int rt_selftest_mpslots(const gc_host* host, unsigned seed, unsigned turns, unsigned humans, unsigned ai, const char* mode, const char* world_size, char* report, size_t report_len)
{
	Check c;
	Seen seen;
	seen.outer = host;
	gc_host mine = {&seen, OnLog, OnReadFile, OnEvent};
	gc_error err;
	memset(&err, 0, sizeof(err));
	if (humans < 3) humans = 3;
	if (humans > 6) humans = 6;
	if (ai < 1) ai = 1;
	const std::string turnMode = mode && *mode ? mode : "simultaneous";
	const bool hybrid = turnMode == "hybrid";
	const double t0 = 0;
	(void)t0;

	gc_instance* inst = gc_create(&mine, &err);
	c.add("create", inst != NULL, "gc_create: %d %s", err.code, err.message);

	Driver d;
	d.inst = inst;
	d.seen = &seen;
	int startTurn = -1, endTurn = -1;
	std::vector<int> humanList;
	if (inst)
	{
		std::string players, bots;
		for (unsigned i = 0; i < humans; ++i)
		{
			players += Format("%s{\"kind\":\"human\",\"civ\":\"%s\",\"nick\":\"bot%u\"}", i ? "," : "", i ? "random" : "CIVILIZATION_ROME", i);
			bots += Format("%s%u", i ? "," : "", i);
		}
		for (unsigned i = 0; i < ai; ++i) players += ",{\"kind\":\"ai\",\"civ\":\"random\"}";
		const std::string setup = Format("{\"seed\":%u,\"game_type\":\"network\",\"players\":[%s],\"turn_mode\":\"%s\",\"autoplay_humans\":[%s],\"handicap\":\"HANDICAP_CHIEFTAIN\"%s%s%s}", seed,
			players.c_str(), turnMode.c_str(), bots.c_str(), world_size && *world_size ? ",\"world_size\":\"" : "", world_size && *world_size ? world_size : "", world_size && *world_size ? "\"" : "");
		const int32_t rc = gc_new_game(inst, setup.c_str(), &err);
		c.add("new_game", rc == GC_OK, "%s: %d %s", setup.c_str(), rc, err.message);
		d.session = InstanceSession(inst);
		if (rc == GC_OK && d.session)
		{
			RtSession* session = d.session;
			d.seat = (int)ServerSeat();
			for (int i = 0; i < MAX_PLAYERS; ++i)
				if (IsSeatedHuman(i)) humanList.push_back(i);
			const int H = (int)humanList.size();
			const int firstAi = (int)humans;  // the original AI slot
			const int h1 = humanList.size() > 1 ? humanList[1] : -1, h2 = humanList.size() > 2 ? humanList[2] : -1;
			startTurn = GC.getGame().getGameTurn();
			const int target = startTurn + (int)turns;
			const int humanHandicap = GC.getInfoTypeForString("HANDICAP_CHIEFTAIN", true);
			const int aiHandicap = GC.getAI_HANDICAP();

			// ---- turn 3: refusals of the state ----
			if (d.ToSafePoint(3))
			{
				gcq_game g;
				bool allOn = GetGame(inst, &g) && g.humans == H;
				for (int h = 0; h < H; ++h)
				{
					gcq_turn r;
					allOn = allOn && GetTurn(inst, humanList[h], &r) && (r.flags & GC_TURN_F_CONNECTED) && !(r.flags & (GC_TURN_F_DISCONNECTED | GC_TURN_F_KICKED)) && (r.flags & GC_TURN_F_HUMAN);
				}
				c.add("start_connected", allOn, "%d human(s), all on line, none disconnected or kicked at turn %d", H, GC.getGame().getGameTurn());
				const int32_t kickAi = d.Slot(firstAi, GC_SLOT_KICK, NULL);
				const int kickAiReason = (int)d.err.reason;
				const int32_t kickSeat = d.Slot(d.seat, GC_SLOT_KICK, NULL);
				const int kickSeatReason = (int)d.err.reason;
				const int32_t discAi = d.Slot(firstAi, GC_SLOT_DISCONNECT, NULL);
				const int discAiReason = (int)d.err.reason;
				const int32_t closed = d.Slot(MAX_MAJOR_CIVS - 1, GC_SLOT_KICK, NULL);  // the last major slot is closed in a game with fewer than 21 civs
				const int closedReason = (int)d.err.reason;
				const int32_t noNick = d.Slot(firstAi, GC_SLOT_CONNECT, NULL);
				const int32_t emptyNick = d.Slot(firstAi, GC_SLOT_CONNECT, "");
				const int32_t badPlayer = d.Slot(-1, GC_SLOT_KICK, NULL);
				const int32_t cityState = gc_slot_control(inst, MAX_MAJOR_CIVS, GC_SLOT_KICK, NULL, &d.err);
				const int32_t badOp = gc_slot_control(inst, 0, 99, NULL, &d.err);
				const std::string longNick(65, 'x');
				const int32_t longNickRc = gc_slot_control(inst, firstAi, GC_SLOT_CONNECT, longNick.c_str(), &d.err);
				c.add("slot_state_refusals", kickAi == GC_ERR_REJECTED && kickAiReason == GC_REASON_SLOT_STATE && kickSeat == GC_ERR_REJECTED && kickSeatReason == GC_REASON_SLOT_STATE &&
					discAi == GC_ERR_REJECTED && discAiReason == GC_REASON_SLOT_STATE && closed == GC_ERR_REJECTED && closedReason == GC_REASON_SLOT_STATE && noNick == GC_ERR_INVALID_ARG &&
					emptyNick == GC_ERR_INVALID_ARG && badPlayer == GC_ERR_INVALID_ARG && cityState == GC_ERR_INVALID_ARG && badOp == GC_ERR_INVALID_ARG && longNickRc == GC_ERR_INVALID_ARG,
					"KICK of an AI %d/%d, of the seat %d/%d, DISCONNECT of an AI %d/%d, KICK of a closed slot %d/%d (want REJECTED/SLOT_STATE); CONNECT onto an AI without a nick %d, with an empty one %d, "
					"player -1 %d, a city-state %d, op 99 %d, a 65-byte nick %d (want INVALID_ARG)", kickAi, kickAiReason, kickSeat, kickSeatReason, discAi, discAiReason, closed, closedReason,
					noNick, emptyNick, badPlayer, cityState, badOp, longNickRc);
				c.add("refusals_changed_nothing", seen.events[GC_EVENT_SLOT_CHANGED] == 0, "%d SLOT_CHANGED event(s) after the refused calls", seen.events[GC_EVENT_SLOT_CHANGED]);
			}

			// ---- turn 5: a command takes the safe point away ----
			if (d.stop.empty() && d.ToSafePoint(5))
			{
				gc_command cc = Cmd(GC_CMD_CANCEL_END_TURN, humanList[0]);  // not ended: an accepted, idempotent command
				const int32_t crc = gc_apply_command(inst, &cc, &err);
				gcq_game g;
				const bool got = GetGame(inst, &g);
				const int32_t unsafe = d.Slot(h1, GC_SLOT_DISCONNECT, NULL);
				const int unsafeReason = (int)d.err.reason;
				const std::string unsafeMsg = d.err.message;
				bool backAfterPump = false;
				if (d.Pump())
					backAfterPump = d.ToSafePoint(GC.getGame().getGameTurn());
				c.add("not_safe_after_command", crc == GC_OK && got && g.safe_point == 0 && g.unsafe_reason == GC_UNSAFE_COMMAND_GIVEN && unsafe == GC_ERR_REJECTED && unsafeReason == GC_REASON_NOT_SAFE_POINT &&
					backAfterPump && seen.events[GC_EVENT_SLOT_CHANGED] == 0,
					"after a command: Q_GAME safe_point %d unsafe_reason %d; DISCONNECT rc %d reason %d \"%s\"; safe again after a gc_pump: %d", got ? (int)g.safe_point : -1, got ? (int)g.unsafe_reason : -1,
					unsafe, unsafeReason, unsafeMsg.c_str(), backAfterPump ? 1 : 0);
			}

			// ---- turn 10: DISCONNECT of human 1, the turn waits, CONNECT ----
			if (d.stop.empty() && d.ToSafePoint(10))
			{
				const int turn = GC.getGame().getGameTurn();
				const size_t ev0 = seen.slotEvents.size();
				const int32_t rc1 = d.Slot(h1, GC_SLOT_DISCONNECT, NULL);
				gcq_turn r;
				const bool got = GetTurn(inst, h1, &r);
				const bool flagsOk = got && (r.flags & GC_TURN_F_HUMAN) && !(r.flags & GC_TURN_F_CONNECTED) && (r.flags & GC_TURN_F_DISCONNECTED) && !(r.flags & GC_TURN_F_KICKED) && r.slot_status == SS_TAKEN;
				const bool evOk = seen.slotEvents.size() == ev0 + 1 && seen.slotEvents[ev0].player == h1 && seen.slotEvents[ev0].slot == SS_TAKEN && seen.slotEvents[ev0].connected == 0 && seen.slotEvents[ev0].kicked == 0;
				c.add("disconnect", rc1 == GC_OK && flagsOk && evOk && IsSeatedHuman(h1) && !session->Engine().SlotConnected((PlayerTypes)h1),
					"turn %d: DISCONNECT of %d rc %d %s; Q_TURN flags 0x%04x slot status %d; SLOT_CHANGED event %d", turn, h1, rc1, d.err.message, got ? (unsigned)r.flags : 0u, got ? (int)r.slot_status : -1,
					evOk ? 1 : 0);
				// The turn does not end however long the others take: pump until the humans who are on line have ended, then a while more.
				int waited = 0;
				bool held = true;
				for (; waited < 400 && d.stop.empty(); ++waited)
				{
					if (!d.Pump()) break;
					if (d.pr.turn != turn) { held = false; break; }
				}
				gcq_game g;
				GetGame(inst, &g);
				gcq_turn r2;
				const bool got2 = GetTurn(inst, h1, &r2);
				c.add("turn_waits_for_disconnected", d.stop.empty() && held && d.pr.status == GC_PUMP_WAITING_HUMAN && g.humans_left == 1 && got2 && (r2.flags & GC_TURN_F_WAITING_FOR) &&
					(r2.flags & GC_TURN_F_TURN_ACTIVE) && !(r2.flags & GC_TURN_F_TURN_ENDED),
					"%d gc_pump call(s): turn %d -> %d, status %d, humans_left %d, Q_TURN of %d: flags 0x%04x", waited, turn, d.pr.turn, d.pr.status, got ? (int)g.humans_left : -1, h1, got2 ? (unsigned)r2.flags : 0u);
				const size_t ev1 = seen.slotEvents.size();
				// The scenario's own bot does not play a human who is off line: a CONNECT is not at a safe point after those gc_pump calls only if a command was given; none was.
				const int32_t rc2 = d.Slot(h1, GC_SLOT_CONNECT, NULL);
				gcq_turn r3;
				const bool got3 = GetTurn(inst, h1, &r3);
				c.add("reconnect", rc2 == GC_OK && got3 && (r3.flags & GC_TURN_F_CONNECTED) && !(r3.flags & (GC_TURN_F_DISCONNECTED | GC_TURN_F_KICKED)) && seen.slotEvents.size() == ev1 + 1 &&
					seen.slotEvents[ev1].connected == 1 && seen.slotEvents[ev1].player == h1,
					"CONNECT of %d rc %d %s; Q_TURN flags 0x%04x; SLOT_CHANGED events %d", h1, rc2, d.err.message, got3 ? (unsigned)r3.flags : 0u, (int)(seen.slotEvents.size() - ev1));
				const bool moved = d.stop.empty() && d.ToSafePoint(turn + 1);
				c.add("turn_goes_on_after_reconnect", moved, "turn %d -> %d%s%s", turn, GC.getGame().getGameTurn(), d.stop.empty() ? "" : ": ", d.stop.c_str());
			}

			// ---- turn 20: DISCONNECT then KICK of human 2 ----
			if (d.stop.empty() && d.ToSafePoint(20))
			{
				const int turn = GC.getGame().getGameTurn();
				const int32_t rc1 = d.Slot(h2, GC_SLOT_DISCONNECT, NULL);
				int waited = 0;
				bool held = true;
				for (; waited < 400 && d.stop.empty(); ++waited)
				{
					if (!d.Pump()) break;
					if (d.pr.turn != turn) { held = false; break; }
				}
				c.add("second_disconnect_holds", rc1 == GC_OK && d.stop.empty() && held && d.pr.status == GC_PUMP_WAITING_HUMAN, "turn %d: DISCONNECT of %d rc %d; turn still %d after %d gc_pump call(s), status %d", turn, h2, rc1,
					d.pr.turn, waited, d.pr.status);
				const int unitsBefore = GET_PLAYER((PlayerTypes)h2).getNumUnits(), citiesBefore = GET_PLAYER((PlayerTypes)h2).getNumCities();
				const size_t ev0 = seen.slotEvents.size();
				const int32_t rk = d.Slot(h2, GC_SLOT_KICK, "ignored-nick");
				const CvPlayer& kp = GET_PLAYER((PlayerTypes)h2);
				gcq_turn r;
				const bool got = GetTurn(inst, h2, &r);
				const bool evOk = seen.slotEvents.size() == ev0 + 1 && seen.slotEvents[ev0].player == h2 && seen.slotEvents[ev0].slot == SS_COMPUTER && seen.slotEvents[ev0].connected == 0 && seen.slotEvents[ev0].kicked == 1;
				c.add("kick", rk == GC_OK && CvPreGame::slotStatus((PlayerTypes)h2) == SS_COMPUTER && !kp.isHuman() && CvPreGame::handicap((PlayerTypes)h2) == (HandicapTypes)aiHandicap && got && !(r.flags & GC_TURN_F_HUMAN) &&
					(r.flags & GC_TURN_F_KICKED) && !(r.flags & GC_TURN_F_CONNECTED) && (r.flags & GC_TURN_F_ALIVE) && evOk && !IsSeatedHuman(h2),
					"KICK of %d rc %d %s; slot status %d, human %d, handicap %d (the AI's %d); Q_TURN flags 0x%04x; SLOT_CHANGED event %d", h2, rk, d.err.message, (int)CvPreGame::slotStatus((PlayerTypes)h2), kp.isHuman() ? 1 : 0,
					(int)CvPreGame::handicap((PlayerTypes)h2), aiHandicap, got ? (unsigned)r.flags : 0u, evOk ? 1 : 0);
				// The AI has the rest of the turn to play: not a safe point until the next gc_pump, so a second slot change must wait.
				gcq_game g;
				const bool gotG = GetGame(inst, &g);
				const int32_t again = d.Slot(h2, GC_SLOT_CONNECT, "too-early");
				const int againReason = (int)d.err.reason;
				c.add("kick_not_safe_until_pump", gotG && g.safe_point == 0 && again == GC_ERR_REJECTED && againReason == GC_REASON_NOT_SAFE_POINT, "after the KICK: Q_GAME safe_point %d unsafe_reason %d; CONNECT before a gc_pump rc %d reason %d",
					gotG ? (int)g.safe_point : -1, gotG ? (int)g.unsafe_reason : -1, again, againReason);
				const bool moved = d.stop.empty() && d.ToSafePoint(turn + 1);
				const int unitsAfter = kp.getNumUnits(), citiesAfter = kp.getNumCities();
				gcq_game g2;
				const bool gotG2 = GetGame(inst, &g2);
				c.add("ai_plays_kicked_slot", moved && kp.isAlive() && !kp.isTurnActive() && gotG2 && g2.humans == H - 1 && g2.humans_left <= H - 1,
					"turn %d -> %d without the kicked human's end of turn; player %d alive %d, turn active %d, %d -> %d unit(s), %d -> %d city(ies); humans %d, humans_left %d%s%s", turn, GC.getGame().getGameTurn(), h2,
					kp.isAlive() ? 1 : 0, kp.isTurnActive() ? 1 : 0, unitsBefore, unitsAfter, citiesBefore, citiesAfter, gotG2 ? (int)g2.humans : -1, gotG2 ? (int)g2.humans_left : -1, d.stop.empty() ? "" : ": ", d.stop.c_str());
			}

			// ---- turn 25 and 30: new humans take AI slots ----
			struct Claim { int turn, slot; const char* nick; const char* what; } claims[2] = {{25, h2, "latecomer", "the kicked human's slot"}, {30, firstAi, "newcomer", "the original AI's slot"}};
			for (int k = 0; k < 2 && d.stop.empty() && claims[k].slot >= 0; ++k)
			{
				if (!d.ToSafePoint(claims[k].turn)) break;
				const int turn = GC.getGame().getGameTurn();
				const int slot = claims[k].slot;
				const CvPlayer& cp = GET_PLAYER((PlayerTypes)slot);
				const bool wasAlive = cp.isAlive();
				const size_t ev0 = seen.slotEvents.size();
				const size_t end0 = seen.turnEnded.size();
				gcq_game g0;
				const bool got0 = GetGame(inst, &g0);
				const int32_t rc = d.Slot(slot, GC_SLOT_CONNECT, claims[k].nick);
				gcq_turn r;
				const bool got = GetTurn(inst, slot, &r);
				gcq_game g1;
				const bool got1 = GetGame(inst, &g1);
				bool endedEvent = false;
				for (size_t i = end0; i < seen.turnEnded.size(); ++i) endedEvent = endedEvent || (seen.turnEnded[i].first == slot && seen.turnEnded[i].second == 1);
				const bool evOk = seen.slotEvents.size() == ev0 + 1 && seen.slotEvents[ev0].player == slot && seen.slotEvents[ev0].slot == SS_TAKEN && seen.slotEvents[ev0].connected == 1 &&
					seen.slotEvents[ev0].kicked == 0 && seen.slotEvents[ev0].nick == claims[k].nick;
				c.add(k == 0 ? "claim_kicked_slot" : "claim_ai_slot", wasAlive && rc == GC_OK && CvPreGame::slotStatus((PlayerTypes)slot) == SS_TAKEN && cp.isHuman() && std::string(CvPreGame::nickname((PlayerTypes)slot).c_str()) == claims[k].nick &&
					CvPreGame::handicap((PlayerTypes)slot) == (HandicapTypes)humanHandicap && got && (r.flags & GC_TURN_F_HUMAN) && (r.flags & GC_TURN_F_CONNECTED) && !(r.flags & GC_TURN_F_KICKED) &&
					(r.flags & GC_TURN_F_TURN_ENDED) && !(r.flags & GC_TURN_F_WAITING_FOR) && !cp.isTurnActive() && endedEvent && evOk && got0 && got1 && g1.humans == g0.humans + 1 && g1.humans_left == g0.humans_left,
					"turn %d: CONNECT of %s (%d) as '%s' rc %d %s; slot status %d, human %d, handicap %d (humans' %d), Q_TURN flags 0x%04x, PLAYER_TURN_ENDED %d, SLOT_CHANGED %d; humans %d -> %d, humans_left %d -> %d",
					turn, claims[k].what, slot, claims[k].nick, rc, d.err.message, (int)CvPreGame::slotStatus((PlayerTypes)slot), cp.isHuman() ? 1 : 0, (int)CvPreGame::handicap((PlayerTypes)slot), humanHandicap,
					got ? (unsigned)r.flags : 0u, endedEvent ? 1 : 0, evOk ? 1 : 0, got0 ? (int)g0.humans : -1, got1 ? (int)g1.humans : -1, got0 ? (int)g0.humans_left : -1, got1 ? (int)g1.humans_left : -1);
				// The turn must not hang on the new human; from the next turn it plays (its test bot ends turns).
				const bool moved = d.ToSafePoint(turn + 1);
				gcq_turn r2;
				const bool got2 = GetTurn(inst, slot, &r2);
				c.add(k == 0 ? "claimed_turn_not_hung" : "claimed_ai_turn_not_hung", moved && GC.getGame().getGameTurn() == turn + 1 && got2 && cp.isHuman() && (r2.flags & GC_TURN_F_TURN_ACTIVE) && (r2.flags & GC_TURN_F_WAITING_FOR) && !(r2.flags & GC_TURN_F_TURN_ENDED),
					"turn %d -> %d: the new human of %d is turn-active and waited for (Q_TURN flags 0x%04x)%s%s", turn, GC.getGame().getGameTurn(), slot, got2 ? (unsigned)r2.flags : 0u, d.stop.empty() ? "" : ": ", d.stop.c_str());
			}

			// ---- on to the target turn; the new humans must have played ----
			if (d.stop.empty())
			{
				for (int i = 0; i < 40000 && GC.getGame().getGameTurn() < target && d.stop.empty(); ++i)
					if (!d.Pump()) break;
			}
			endTurn = GC.getGame().getGameTurn();
			for (int k = 0; k < 2; ++k)
			{
				const int slot = claims[k].slot;
				const RtAutoplay* b = slot >= 0 ? InstanceBot(inst, slot) : NULL;
				c.add(k == 0 ? "latecomer_played" : "newcomer_played", b && b->Stats().turns >= 1 && IsSeatedHuman(slot), "player %d: %d turn(s) ended by its bot, %d command(s)", slot, b ? b->Stats().turns : -1,
					b ? b->Stats().commands : -1);
			}
			// Humans 0 and 1 played all the way (human 1 off line for the turn of its disconnect only).
			{
				const RtAutoplay* b0 = InstanceBot(inst, humanList[0]);
				const RtAutoplay* b1 = InstanceBot(inst, h1);
				c.add("others_played_on", b0 && b1 && b0->Stats().turns >= endTurn - startTurn - 1 && b1->Stats().turns >= endTurn - startTurn - 1, "human %d ended %d and human %d ended %d of %d turn(s)", humanList[0],
					b0 ? b0->Stats().turns : -1, h1, b1 ? b1->Stats().turns : -1, endTurn - startTurn);
			}
			c.add("slot_events", seen.slotEvents.size() == 6, "%d SLOT_CHANGED event(s) (want disconnect, connect, disconnect, kick, two claims = 6)", (int)seen.slotEvents.size());
		}
	}
	for (size_t i = 0; i < seen.lines.size(); ++i) printf("mpslots-event|%s\n", seen.lines[i].c_str());
	const int doneTurns = endTurn >= 0 && startTurn >= 0 ? endTurn - startTurn : 0;
	c.add("turns", d.stop.empty() && doneTurns == (int)turns, "%d/%u turn(s) (turn %d -> %d), %lld gc_pump call(s)%s%s", doneTurns, turns, startTurn, endTurn, d.calls, d.stop.empty() ? "" : ": ", d.stop.c_str());
	c.add("no_stuck", d.stuck == 0 && d.gameOver == 0 && d.pumpErrors == 0, "%d STUCK, %d GAME_OVER, %d gc_pump error(s)", d.stuck, d.gameOver, d.pumpErrors);
	c.add("active_rule", d.activeBroken == 0, "%d time(s) the active player was not the server seat %d after a gc_ call", d.activeBroken, d.seat);
	c.add("lua_errors", seen.luaErrors == 0, "%d LUA_ERROR event(s)", seen.luaErrors);
	c.add("log_errors", seen.logErrors == 0, "%d error(s), %d warning(s) logged", seen.logErrors, seen.logWarnings);

	const std::string out = "{\"failed\":" + Num(c.failed) + ",\"mode\":\"" + turnMode + "\",\"seed\":" + Num(seed) + ",\"humans\":" + Num(humans) + ",\"ai\":" + Num(ai) + ",\"turns_requested\":" + Num(turns) +
		",\"turns_done\":" + Num(doneTurns) + ",\"start_turn\":" + Num(startTurn) + ",\"end_turn\":" + Num(endTurn) + ",\"stuck\":" + Num(d.stuck) + ",\"slot_events\":" + Num((long long)seen.slotEvents.size()) +
		",\"log_errors\":" + Num(seen.logErrors) + ",\"lua_errors\":" + Num(seen.luaErrors) + ",\"pump_calls\":" + Num(d.calls) + ",\"checks\":[" + c.json + "]}";
	if (report && report_len)
	{
		strncpy(report, out.c_str(), report_len - 1);
		report[report_len - 1] = '\0';
	}
	printf("mpslots-summary|%s|seed %u|humans %u|ai %u|turns %d/%u|stuck %d|slot events %d|failed %d\n", turnMode.c_str(), seed, humans, ai, doneTurns, turns, d.stuck, (int)seen.slotEvents.size(), c.failed);
	(void)hybrid;
	gc_destroy(inst);
	return c.failed == 0 ? 0 : 1;
}
