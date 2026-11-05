// Scenario `timer` (task N3.6; run.sh --scenario timer --humans N --ai A [--turns T]): the pieces a game server's turn clock needs, through the C ABI
// only, with the humans played by this scenario (no test bots): a network game (simultaneous turns, a real TurnTimers row), gc_pump to the human phase, then per turn
//  - Q_GAME v2 (turn_base_s of the row, turn_len_s 0 on game turn 0 and the enforced length after, humans, humans_left, phase, safe point with its reason) and Q_TURN
//    (a record per human: active, not ended, simultaneous, waiting for, a blocker with its target);
//  - END_TURN of a human with a blocker is refused with GC_REASON_BLOCKED (the blocker is named); END_TURN_TIMEOUT of the same human is accepted (RW-23: HumanMayAct and
//    sendTurnComplete, no blocker checks), the human's record shows the end of turn and humans_left falls by one;
//  - CANCEL_END_TURN takes it back (humans_left rises again); CANCEL_END_TURN of a human who has not ended is an idempotent OK; the safe point is lost after a command;
//  - the turn waits until every human has ended: after the last END_TURN_TIMEOUT gc_pump runs the AI turn and the next turn starts; a CANCEL_END_TURN at that moment is either
//    taken (the turn goes on waiting) or refused with GC_REASON_TURN_ENDED / NOT_YOUR_TURN, and what happened is printed;
//  - events: PLAYER_TURN_STARTED and PLAYER_TURN_ENDED per human and turn, HUMAN_PHASE_STARTED once per turn.
#include "CvGameCoreDLLPCH.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "../abi/gc_internal.h"
#include "rt_active.h"
#include "rt_selftest.h"
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
		printf("timer: %-4s %s: %s\n", ok ? "ok" : "FAIL", name, detail);
		if (!ok) ++failed;
	}
};

struct Seen
{
	const gc_host* outer;
	int logErrors, logWarnings, luaErrors;
	int events[32];
	int endedSent, endedCancelled;
	std::vector<std::string> lines;
	Seen() : outer(NULL), logErrors(0), logWarnings(0), luaErrors(0), endedSent(0), endedCancelled(0) { memset(events, 0, sizeof(events)); }
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
	if (ev->kind == GC_EVENT_PLAYER_TURN_ENDED && ev->argc >= 2) { if (ev->args[1]) ++s->endedSent; else ++s->endedCancelled; }
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

bool GetGame(gc_instance* inst, int player, gcq_game* out)
{
	gc_buf b = {0, 0, 0, 0};
	gc_error err;
	memset(&err, 0, sizeof(err));
	const int32_t arg[1] = {(int32_t)player};
	const int32_t rc = gc_query(inst, GC_Q_GAME, player >= 0 ? arg : NULL, player >= 0 ? 1 : 0, &b, &err);
	const bool ok = rc == GC_OK && b.len >= sizeof(gcq_header) + sizeof(gcq_game);
	if (ok) memcpy(out, b.data + sizeof(gcq_header), sizeof(*out));
	gc_buf_release(&b);
	return ok;
}

// The Q_TURN record of a player (by id); false when there is none.
bool GetTurn(gc_instance* inst, int player, gcq_turn* out, int* count)
{
	gc_buf b = {0, 0, 0, 0};
	gc_error err;
	memset(&err, 0, sizeof(err));
	const int32_t rc = gc_query(inst, GC_Q_TURN, NULL, 0, &b, &err);
	bool found = false;
	if (rc == GC_OK && b.len >= sizeof(gcq_header))
	{
		gcq_header h;
		memcpy(&h, b.data, sizeof(h));
		if (count) *count = (int)h.count;
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

// gc_pump until the humans are up (WAITING_HUMAN); false when it did not come to that.
bool PumpToHumans(gc_instance* inst, gc_pump_result* pr, std::string* why)
{
	gc_error err;
	memset(&err, 0, sizeof(err));
	for (int calls = 0; calls < 20000; ++calls)
	{
		const int32_t rc = gc_pump(inst, 64, pr, &err);
		if (rc != GC_OK) { *why = Format("gc_pump: %d %s", rc, err.message); return false; }
		if (pr->status == GC_PUMP_WAITING_HUMAN) return true;
		if (pr->status == GC_PUMP_GAME_OVER || pr->status == GC_PUMP_STUCK) { *why = Format("gc_pump status %d at turn %d", pr->status, pr->turn); return false; }
	}
	*why = "no WAITING_HUMAN after 20000 gc_pump calls";
	return false;
}

}  // namespace

extern "C" int rt_selftest_timer(const gc_host* host, unsigned seed, unsigned turns, unsigned humans, unsigned ai, const char* world_size, char* report, size_t report_len)
{
	Check c;
	Seen seen;
	seen.outer = host;
	gc_host mine = {&seen, OnLog, OnReadFile, OnEvent};
	gc_error err;
	memset(&err, 0, sizeof(err));
	if (humans < 2) humans = 2;
	if (humans > 4) humans = 4;

	gc_instance* inst = gc_create(&mine, &err);
	c.add("create", inst != NULL, "gc_create: %d %s", err.code, err.message);

	int doneTurns = 0, startTurn = -1;
	int setupBad = 0, turnBase = 0, lenAtStart = -1, lenLater = -1, blockerSeen = 0, blockedRefused = 0, blockedReasonOk = 0, timeoutOk = 0, timeoutBad = 0, cancelBad = 0, cancelOk = 0;
	int idemOk = 0, idemBad = 0, safeBad = 0, qBad = 0, lateTried = 0, lateOk = 0, lateRefused = 0, lateBad = 0, nonHumanOk = 0, nonHumanTried = 0, lenBad = 0, humansLeftBad = 0;
	std::string stopReason, setupNote, blockedNote, firstBad, lateNote, nonHumanNote, qNote, lenNote;
	std::vector<int> humanList;
	if (inst)
	{
		std::string players;
		for (unsigned i = 0; i < humans; ++i)
			players += Format("%s{\"kind\":\"human\",\"civ\":\"%s\",\"nick\":\"human%u\"}", i ? "," : "", i ? "random" : "CIVILIZATION_ROME", i);
		for (unsigned i = 0; i < ai; ++i) players += std::string(humans || i ? "," : "") + "{\"kind\":\"ai\",\"civ\":\"random\"}";
		const std::string setup = Format("{\"seed\":%u,\"game_type\":\"network\",\"players\":[%s],\"turn_mode\":\"simultaneous\",\"turn_timer\":\"TURNTIMER_MEDIUM\",\"handicap\":\"HANDICAP_CHIEFTAIN\"%s%s%s}", seed,
			players.c_str(), world_size && *world_size ? ",\"world_size\":\"" : "", world_size && *world_size ? world_size : "", world_size && *world_size ? "\"" : "");
		const int32_t rc = gc_new_game(inst, setup.c_str(), &err);
		c.add("new_game", rc == GC_OK, "%s: %d %s", setup.c_str(), rc, err.message);
		RtSession* session = InstanceSession(inst);
		if (rc == GC_OK && session)
		{
			for (int i = 0; i < MAX_PLAYERS; ++i)
				if (IsSeatedHuman(i)) humanList.push_back(i);
			gc_pump_result pr;
			memset(&pr, 0, sizeof(pr));
			startTurn = GC.getGame().getGameTurn();
			const int H = (int)humanList.size();
			for (int t = 0; t < (int)turns && stopReason.empty(); ++t)
			{
				if (!PumpToHumans(inst, &pr, &stopReason)) break;
				const int turn = pr.turn;
				// ---- Q_GAME v2 and Q_TURN at the start of the human phase ----
				gcq_game g;
				if (!GetGame(inst, -1, &g)) { stopReason = "Q_GAME failed"; break; }
				if (t == 0)
				{
					turnBase = g.turn_base_s;
					lenAtStart = g.turn_len_s;
					setupNote = Format("turn %d: game_type %d turn_mode %d humans %d humans_left %d phase %d turn_base_s %d turn_len_s %d safe_point %d unsafe_reason %d", turn, (int)g.game_type, (int)g.turn_mode,
						(int)g.humans, (int)g.humans_left, (int)g.phase, (int)g.turn_base_s, (int)g.turn_len_s, (int)g.safe_point, (int)g.unsafe_reason);
					if (g.game_type != 1 || g.turn_mode != GC_TURN_MODE_SIMULTANEOUS || g.humans != H || g.phase != GC_PHASE_HUMANS || g.turn_base_s <= 0 || g.turn_len_s != 0) ++setupBad;
				}
				else
				{
					if (lenLater < 0) lenLater = g.turn_len_s;
					if (g.turn_len_s <= 0 || g.turn_len_s < g.turn_base_s) { ++lenBad; if (lenNote.empty()) lenNote = Format("turn %d: turn_len_s %d, turn_base_s %d", turn, (int)g.turn_len_s, (int)g.turn_base_s); }
				}
				if (g.humans_left != H || pr.humans_left != H) { ++humansLeftBad; if (firstBad.empty()) firstBad = Format("turn %d: humans_left %d (pump %d), want %d", turn, (int)g.humans_left, pr.humans_left, H); }
				if (g.safe_point != 1 || g.unsafe_reason != GC_UNSAFE_NONE) { ++safeBad; if (firstBad.empty()) firstBad = Format("turn %d: not a safe point at the start of the human phase (reason %d)", turn, (int)g.unsafe_reason); }

				int turnBlockers = 0;
				for (int h = 0; h < H; ++h)
				{
					gcq_turn r;
					int count = 0;
					if (!GetTurn(inst, humanList[h], &r, &count) || count != H) { ++qBad; if (qNote.empty()) qNote = Format("turn %d: no Q_TURN record for player %d (count %d)", turn, humanList[h], count); continue; }
					const unsigned want = GC_TURN_F_HUMAN | GC_TURN_F_ALIVE | GC_TURN_F_CONNECTED | GC_TURN_F_TURN_ACTIVE | GC_TURN_F_SIMULTANEOUS | GC_TURN_F_WAITING_FOR;
					if ((r.flags & want) != want || (r.flags & (GC_TURN_F_TURN_ENDED | GC_TURN_F_DISCONNECTED | GC_TURN_F_KICKED)) || r.turn_order != -1)
					{
						++qBad;
						if (qNote.empty()) qNote = Format("turn %d player %d: flags 0x%04x turn_order %d", turn, humanList[h], (unsigned)r.flags, (int)r.turn_order);
					}
					if (r.blocker >= 0) ++turnBlockers;
				}
				blockerSeen += turnBlockers;

				// ---- END_TURN of a human with a blocker is refused, END_TURN_TIMEOUT is not ----
				for (int h = 0; h < H; ++h)
				{
					const int who = humanList[h];
					gcq_turn r;
					int count = 0;
					if (!GetTurn(inst, who, &r, &count)) continue;
					if (r.blocker >= 0)
					{
						gc_command endc = Cmd(GC_CMD_END_TURN, who);
						const int32_t erc = gc_apply_command(inst, &endc, &err);
						++blockedRefused;
						if (erc == GC_ERR_REJECTED && err.reason == GC_REASON_BLOCKED) ++blockedReasonOk;
						if (blockedNote.empty()) blockedNote = Format("turn %d player %d: blocker %d (unit %d city %d at %d,%d): END_TURN rc %d reason %d \"%s\"", turn, who, (int)r.blocker, (int)r.blocker_unit,
							(int)r.blocker_city, (int)r.blocker_x, (int)r.blocker_y, erc, (int)err.reason, err.message);
						gcq_game pg;
						if (GetGame(inst, who, &pg) && pg.blocker != r.blocker) { ++qBad; if (qNote.empty()) qNote = Format("turn %d player %d: Q_GAME(player) blocker %d, Q_TURN blocker %d", turn, who, (int)pg.blocker, (int)r.blocker); }
					}
				}
				// A non-human (the AI at slot H): refused with NOT_HUMAN.
				if (t == 0)
				{
					int ai0 = -1;
					for (int i = 0; i < MAX_MAJOR_CIVS; ++i)
						if (GET_PLAYER((PlayerTypes)i).isAlive() && !IsSeatedHuman(i) && (int)ServerSeat() != i) { ai0 = i; break; }
					if (ai0 >= 0)
					{
						gc_command to = Cmd(GC_CMD_END_TURN_TIMEOUT, ai0);
						const int32_t nrc = gc_apply_command(inst, &to, &err);
						++nonHumanTried;
						if (nrc == GC_ERR_REJECTED && err.reason == GC_REASON_NOT_HUMAN) ++nonHumanOk;
						nonHumanNote = Format("player %d: END_TURN_TIMEOUT rc %d reason %d", ai0, nrc, (int)err.reason);
					}
				}

				// ---- timeout of human 0, then take it back ----
				{
					const int who = humanList[0];
					gc_command idem = Cmd(GC_CMD_CANCEL_END_TURN, who);
					int32_t irc = gc_apply_command(inst, &idem, &err);  // not ended yet: idempotent
					if (irc == GC_OK) ++idemOk; else { ++idemBad; if (firstBad.empty()) firstBad = Format("turn %d: CANCEL_END_TURN before an end of turn: rc %d reason %d %s", turn, irc, (int)err.reason, err.message); }
					gc_command to = Cmd(GC_CMD_END_TURN_TIMEOUT, who);
					const int32_t trc = gc_apply_command(inst, &to, &err);
					gcq_turn r;
					int count = 0;
					gcq_game after;
					const bool got = GetTurn(inst, who, &r, &count) && GetGame(inst, -1, &after);
					if (trc == GC_OK && got && (r.flags & GC_TURN_F_TURN_ENDED) && !(r.flags & GC_TURN_F_WAITING_FOR) && after.humans_left == H - 1 && after.safe_point == 0 && after.unsafe_reason == GC_UNSAFE_COMMAND_GIVEN)
						++timeoutOk;
					else
					{
						++timeoutBad;
						if (firstBad.empty()) firstBad = Format("turn %d: END_TURN_TIMEOUT of %d: rc %d reason %d \"%s\"; ended %d humans_left %d safe %d/%d", turn, who, trc, (int)err.reason, err.message,
							got ? ((r.flags & GC_TURN_F_TURN_ENDED) ? 1 : 0) : -1, got ? (int)after.humans_left : -1, got ? (int)after.safe_point : -1, got ? (int)after.unsafe_reason : -1);
					}
					gc_command cancel = Cmd(GC_CMD_CANCEL_END_TURN, who);
					const int32_t crc = gc_apply_command(inst, &cancel, &err);
					gcq_game back;
					if (crc == GC_OK && GetGame(inst, -1, &back) && back.humans_left == H && GetTurn(inst, who, &r, &count) && !(r.flags & GC_TURN_F_TURN_ENDED) && (r.flags & GC_TURN_F_WAITING_FOR)) ++cancelOk;
					else { ++cancelBad; if (firstBad.empty()) firstBad = Format("turn %d: CANCEL_END_TURN of %d: rc %d reason %d %s", turn, who, crc, (int)err.reason, err.message); }
				}
				// A gc_pump while the humans still wait: still WAITING_HUMAN, and the safe point is back.
				{
					gc_pump_result again;
					memset(&again, 0, sizeof(again));
					gc_error perr;
					memset(&perr, 0, sizeof(perr));
					const int32_t prc = gc_pump(inst, 64, &again, &perr);
					gcq_game sg;
					if (prc != GC_OK || again.status != GC_PUMP_WAITING_HUMAN || again.humans_left != H || !GetGame(inst, -1, &sg) || sg.safe_point != 1)
					{
						++safeBad;
						if (firstBad.empty()) firstBad = Format("turn %d: gc_pump with the humans still up: rc %d status %d humans_left %d", turn, prc, again.status, again.humans_left);
					}
				}

				// ---- every human times out; the last one's end closes the turn ----
				for (int h = 0; h < H && stopReason.empty(); ++h)
				{
					const int who = humanList[h];
					gc_command to = Cmd(GC_CMD_END_TURN_TIMEOUT, who);
					const int32_t trc = gc_apply_command(inst, &to, &err);
					if (trc != GC_OK) { ++timeoutBad; if (firstBad.empty()) firstBad = Format("turn %d: END_TURN_TIMEOUT of %d: rc %d reason %d %s", turn, who, trc, (int)err.reason, err.message); }
					if (h == H - 1 && trc == GC_OK && t == 0)
					{
						// CANCEL_END_TURN with every human's end given and gc_pump not yet run.
						gc_command late = Cmd(GC_CMD_CANCEL_END_TURN, humanList[0]);
						const int32_t lrc = gc_apply_command(inst, &late, &err);
						++lateTried;
						lateNote = Format("turn %d: CANCEL_END_TURN of %d after every human ended: rc %d reason %d \"%s\"", turn, humanList[0], lrc, (int)err.reason, err.message);
						if (lrc == GC_OK)
						{
							++lateOk;
							gc_command again = Cmd(GC_CMD_END_TURN_TIMEOUT, humanList[0]);
							if (gc_apply_command(inst, &again, &err) != GC_OK) { ++lateBad; lateNote += "; the second end of turn was refused"; }
						}
						else if (lrc == GC_ERR_REJECTED && (err.reason == GC_REASON_TURN_ENDED || err.reason == GC_REASON_NOT_YOUR_TURN)) ++lateRefused;
						else ++lateBad;
					}
				}
				if (!stopReason.empty()) break;
				// The turn closes: gc_pump runs the AI turn and the next one starts.
				gc_pump_result next;
				memset(&next, 0, sizeof(next));
				const int turnBefore = turn;
				bool moved = false;
				for (int calls = 0; calls < 20000 && !moved; ++calls)
				{
					const int32_t prc = gc_pump(inst, 64, &next, &err);
					if (prc != GC_OK || next.status == GC_PUMP_GAME_OVER || next.status == GC_PUMP_STUCK) { stopReason = Format("gc_pump after the end of turn %d: rc %d status %d %s", turnBefore, prc, next.status, err.message); break; }
					if (next.turn != turnBefore) moved = true;
				}
				if (moved) ++doneTurns;
				else if (stopReason.empty()) stopReason = Format("turn %d did not end", turnBefore);
			}
		}
	}
	for (size_t i = 0; i < seen.lines.size(); ++i) printf("timer-event|%s\n", seen.lines[i].c_str());

	const int H = (int)humanList.size();
	c.add("turns", doneTurns == (int)turns && stopReason.empty(), "%d/%u turn(s) ended by END_TURN_TIMEOUT only%s%s", doneTurns, turns, stopReason.empty() ? "" : ": ", stopReason.c_str());
	c.add("q_game_start", setupBad == 0 && turnBase > 0 && lenAtStart == 0, "%s", setupNote.empty() ? "not run" : setupNote.c_str());
	c.add("turn_len", lenBad == 0 && (turns < 2 || lenLater > 0), "turn_len_s 0 on game turn 0, then %d (the length the exe enforces: Lekmod's once per turn getPreviousTurnLen); %d turn(s) with turn_len_s <= 0 or below the base time%s%s",
		lenLater, lenBad, lenNote.empty() ? "" : ": ", lenNote.c_str());
	c.add("humans_left", humansLeftBad == 0, "humans_left was the number of humans at the start of every human phase, %d time(s) not%s%s", humansLeftBad, firstBad.empty() ? "" : "; first problem: ", firstBad.c_str());
	c.add("q_turn", qBad == 0, "%s", qBad == 0 ? "every human's record: HUMAN ALIVE CONNECTED TURN_ACTIVE SIMULTANEOUS WAITING_FOR, not ended, turn_order -1" : qNote.c_str());
	c.add("blocker", blockerSeen > 0 && blockedRefused > 0 && blockedReasonOk == blockedRefused, "%d human turn start(s) with a blocker in Q_TURN; END_TURN refused with GC_REASON_BLOCKED %d of %d; %s", blockerSeen, blockedReasonOk,
		blockedRefused, blockedNote.empty() ? "no blocker found" : blockedNote.c_str());
	c.add("end_turn_timeout", timeoutBad == 0 && timeoutOk == doneTurns && doneTurns > 0, "END_TURN_TIMEOUT accepted without blocker checks (ended flag, humans_left %d, safe point lost with COMMAND_GIVEN): %d of %d turn(s), %d refused", H - 1, timeoutOk,
		doneTurns, timeoutBad);
	c.add("cancel_end_turn", cancelBad == 0 && cancelOk == doneTurns && idemBad == 0 && idemOk == doneTurns && doneTurns > 0, "taken back %d of %d time(s) (humans_left back to %d, WAITING_FOR again); idempotent OK without an end of turn %d of %d", cancelOk, doneTurns, H,
		idemOk, doneTurns);
	c.add("cancel_after_all_ended", lateTried == 1 && lateBad == 0 && (lateOk + lateRefused) == 1, "%s", lateNote.empty() ? "not run" : lateNote.c_str());
	c.add("not_human", nonHumanTried == 1 && nonHumanOk == 1, "%s", nonHumanNote.empty() ? "no AI player" : nonHumanNote.c_str());
	c.add("safe_point", safeBad == 0, "the safe point holds at the start of the human phase and after a gc_pump with the humans up, is lost by a command: %d deviation(s)%s%s", safeBad, firstBad.empty() ? "" : "; first: ", firstBad.c_str());
	const int ended = doneTurns * H;
	c.add("events", seen.events[GC_EVENT_PLAYER_TURN_STARTED] >= ended && seen.endedSent >= ended && seen.endedCancelled >= doneTurns && seen.events[GC_EVENT_HUMAN_PHASE_STARTED] >= doneTurns &&
		seen.events[GC_EVENT_HUMAN_PHASE_STARTED] <= doneTurns + 1,
		"PLAYER_TURN_STARTED %d, PLAYER_TURN_ENDED %d sent / %d cancelled (at least %d / %d), HUMAN_PHASE_STARTED %d for %d turn(s)", seen.events[GC_EVENT_PLAYER_TURN_STARTED], seen.endedSent, seen.endedCancelled,
		ended, doneTurns, seen.events[GC_EVENT_HUMAN_PHASE_STARTED], doneTurns);
	c.add("lua_errors", seen.luaErrors == 0, "%d LUA_ERROR event(s)", seen.luaErrors);
	c.add("log_errors", seen.logErrors == 0, "%d error(s), %d warning(s) logged", seen.logErrors, seen.logWarnings);

	const std::string out = "{\"failed\":" + Format("%d", c.failed) + ",\"seed\":" + Format("%u", seed) + ",\"humans\":" + Format("%u", humans) + ",\"ai\":" + Format("%u", ai) + ",\"turns_requested\":" + Format("%u", turns) +
		",\"turns_done\":" + Format("%d", doneTurns) + ",\"start_turn\":" + Format("%d", startTurn) + ",\"turn_base_s\":" + Format("%d", turnBase) + ",\"turn_len_s\":" + Format("%d", lenLater) +
		",\"log_errors\":" + Format("%d", seen.logErrors) + ",\"lua_errors\":" + Format("%d", seen.luaErrors) + ",\"checks\":[" + c.json + "]}";
	if (report && report_len)
	{
		strncpy(report, out.c_str(), report_len - 1);
		report[report_len - 1] = '\0';
	}
	printf("timer-summary|seed %u|humans %u|ai %u|turns %d/%u|turn_base_s %d|turn_len_s %d|%s\n", seed, humans, ai, doneTurns, turns, turnBase, lenLater, stopReason.c_str());
	gc_destroy(inst);
	return c.failed == 0 ? 0 : 1;
}
