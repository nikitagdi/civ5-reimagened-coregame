// gamecore_abi.h v1 over the portable runtime. A thin wrapper: every gc_* entry validates its
// arguments, runs inside try/catch and forwards to lekmod_portable::RtSession (boot Load, NewGame, Pump, StateDigest).
// Staged (return GC_ERR_NOT_IMPLEMENTED after validation): later command and query kinds, gc_lua_register. gc_save / gc_load_game (G4a):
// GCS1 through RtSession::SaveGame / CheckSave / LoadGame (runtime/rt_save.*); a save only at a save point (an unanswered WAITING_HUMAN). gc_query (E2, F2): Q_GAME, Q_PLOTS, Q_UNITS, Q_PLAYERS, Q_UNIT_ACTIONS, Q_PATH, Q_REACHABLE, Q_CATALOG in
// gc_query.cpp; (G1a) Q_CITIES, Q_TOP_BAR, Q_CITY_BUILDS in gc_query_city.cpp. gc_apply_command (F2, G1a): unit commands 1..12,
// city commands 20..22, DECLARE_WAR, END_TURN, FORCE_END_TURN in gc_command.cpp. gc_state_digest (G1a): xxh64 in gc_digest.cpp.
// F8: with the setup's "autoplay_human": 1, gc_pump hands every WAITING_HUMAN to the test bot (runtime/rt_autoplay), which gives the human's
// commands through gc_command.cpp; gc_pump then reports AI_RUNNING (or STUCK when the bot cannot go on), so a host only pumps. G4a: the bot's
// round for a WAITING_HUMAN runs at the start of the next gc_pump (same call order as before), so a save point exists between the two calls.
#include "CvGameCoreDLLPCH.h"

#include "gamecore_abi.h"

#include <new>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "../runtime/rt_active.h"
#include "../runtime/rt_autoplay.h"
#include "../runtime/rt_bytes.h"
#include "../runtime/rt_save.h"
#include "../runtime/rt_session.h"
#include "../runtime/rt_setup.h"
#include "../runtime/rt_slots.h"
#include "gc_command.h"
#include "gc_digest.h"
#include "gc_internal.h"
#include "gc_query.h"
#include "gc_query_dirty.h"
#include "gc_replica.h"
#include "lekmod_portable/host_log.h"

using lekmod_portable::RtSession;
using lekmod_portable::RtSetup;

namespace {

bool g_instance_exists = false;

// The query kinds whose args[0] is the player the query is for: answered under ActiveAs(player) (audit F-06). Q_GAME takes an optional player but
// reads no local-player getter; Q_TURN has no argument (it takes ActiveAs per human itself).
bool QueryIsForPlayer(int32_t kind)
{
	return (kind >= GC_Q_TOP_BAR && kind <= GC_Q_NOTIFICATIONS) || kind == GC_Q_DIPLO || kind == GC_Q_REACHABLE_TREE || kind == GC_Q_COMBAT_PREVIEW;
}

// Layout of the GCS1 "abi" section (gc_save / gc_load_game): u32 version, i32 lastPumpStatus, u8 gameOverSent, u8 rawWaiting, u8 botPending,
// then the test bot (rt_save.h WriteAutoplayState); v2 (N3.13) then u8 netBots and, when set, u32 botRound and per slot 0..MAX_PLAYERS-1 u8 botSlot and, for a
// bot slot, its state (the test bots of the humans of a network game). v3 (B4 gc-replica) then per slot 0..MAX_PLAYERS-1 u8 flags: 1 = on line at the
// save (RtEngine::SlotConnected), 2 = off line only by a load (SlotOfflineByLoad). A load leaves every human off line until the host connects it; a
// replica takes the server's flags (ApplySavedConnections).
const uint32_t kAbiStateVersion = 3;

void ReleaseVector(gc_buf* b) { delete static_cast<std::vector<uint8_t>*>(b->owner); }

}  // namespace

struct gc_instance
{
	gc_host host;
	RtSession session;
	bool gameStarted;   // gc_new_game / gc_load_game was accepted: the one game of this instance is taken
	bool gameOverSent;  // GC_EVENT_GAME_OVER was emitted
	int32_t lastPumpStatus;  // GC_PUMP_* of the last gc_pump call, -1 before the first (Q_GAME status)
	lekmod_portable::RtAutoplay autoplay;  // F8: the test bot in the human's seat (setup "autoplay_human": 1, single game type)
	bool rawWaiting;  // G4a: the last gc_pump ended with the runtime at WAITING_HUMAN (before the bot conversion); a save point needs it
	bool botPending;  // G4a: autoplay_human: the bot owes its round for that WAITING_HUMAN; the next gc_pump gives it before pumping
	// N3.5: a network game with test-bot humans (setup "autoplay_humans", or "autoplay_human" in a network game = every human).
	bool netBots;                                          // some human slot is played by a bot
	bool botSlot[MAX_PLAYERS];                             // which slots
	std::vector<lekmod_portable::RtAutoplay> bots;         // one bot per slot (the per-turn state is the human's)
	unsigned botRound;                                     // rotates the human that gets the first bot round of a step
	lekmod_portable::PlotDirtyTracker dirty;               // N3.8: the snapshot of Q_PLOTS_DIRTY (the ABI layer's, never saved)
	bool commandSince;                                    // N3.6: a gc_apply_command was accepted after the last gc_pump (not a safe point)
	int32_t role;                                         // B4 gc-replica: GC_ROLE_*
	int32_t replicaPlayer;                                // GC_ROLE_REPLICA: the seat whose UI this replica serves, -1 = none
	bool haveSavedConnected;                              // the last loaded save carried savedConnected (abi section v3)
	uint8_t savedConnected[MAX_PLAYERS];                  // per slot at that save: 1 = on line, 2 = off line only by a load (abi section v3)

	gc_instance() : gameStarted(false), gameOverSent(false), lastPumpStatus(-1), rawWaiting(false), botPending(false), netBots(false), botRound(0), commandSince(false), role(GC_ROLE_PLAIN), replicaPlayer(-1), haveSavedConnected(false)
	{
		memset(&host, 0, sizeof(host));
		for (int i = 0; i < MAX_PLAYERS; ++i)
		{
			botSlot[i] = false;
			savedConnected[i] = 0;
		}
	}
};

// N3.5: the test bots of a network game, one per human slot ("autoplay_humans"; a network game's "autoplay_human" means every human). Also after a load: the setup
// is the saved game's, and the save's abi section then overrides who is a bot (a kicked slot is none any more, a claimed one is).
static void InitNetBots(gc_instance* inst)
{
	const RtSetup& su = inst->session.Setup();
	if (!su.Network()) return;
	std::vector<int> slots = su.autoplay_humans;
	if (slots.empty() && su.autoplay_human)
		for (size_t i = 0; i < su.players.size(); ++i)
			if (su.players[i].human) slots.push_back((int)i);
	for (size_t i = 0; i < slots.size(); ++i)
		if (slots[i] >= 0 && slots[i] < MAX_PLAYERS) inst->botSlot[slots[i]] = true;
	inst->netBots = !slots.empty();
	if (inst->netBots) inst->bots.assign(MAX_PLAYERS, lekmod_portable::RtAutoplay());
}

namespace lekmod_portable {
RtSession* InstanceSession(gc_instance* inst) { return inst ? &inst->session : 0; }
const RtAutoplay* InstanceAutoplay(gc_instance* inst) { return inst ? &inst->autoplay : 0; }
int32_t InstanceRole(gc_instance* inst) { return inst ? inst->role : GC_ROLE_PLAIN; }
const RtAutoplay* InstanceBot(gc_instance* inst, int player)
{
	return inst && player >= 0 && player < (int)inst->bots.size() ? &inst->bots[player] : 0;
}
}  // namespace lekmod_portable

namespace {

int32_t Fail(gc_error* err, int32_t code, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
int32_t Fail(gc_error* err, int32_t code, const char* fmt, ...)
{
	if (err)
	{
		err->code = code;
		err->reason = GC_REASON_NONE;  // v2: the commands and slot control fill the reason after Fail (package P3)
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(err->message, sizeof(err->message), fmt, ap);
		va_end(ap);
		err->message[sizeof(err->message) - 1] = '\0';
	}
	return code;
}

void Ok(gc_error* err)
{
	if (err)
	{
		err->code = GC_OK;
		err->message[0] = '\0';
		err->reason = GC_REASON_NONE;
	}
}

void LogSink(void* user, int level, const char* msg)
{
	gc_instance* inst = static_cast<gc_instance*>(user);
	if (inst && inst->host.log) inst->host.log(inst->host.user, level, msg);
}

void EmitEvent(gc_instance* inst, int32_t kind, const int32_t* args, uint32_t argc)
{
	if (!inst->host.game_event) return;
	gc_event ev;
	ev.kind = kind;
	ev.argc = argc;
	ev.args = args;
	ev.text = NULL;
	ev.text2 = NULL;
	inst->host.game_event(inst->host.user, &ev);
}

// F7: hands every event the runtime queued (RtSession::Events(): turn changes, notifications, popups, diplomacy auto-declines, war state, Lua
// errors, in the order they happened) to the host. The strings live in the popped RtEvent, so they are valid during the callback.
// F3: the auto-choices the runtime made during the pump (RtAutoChoice) are queued first as GC_EVENT_AUTO_CHOICE (choice_kind, player, item; text
// = the journal line), so they come out in the one ordered stream, after the events the pump itself produced.
void EmitQueuedEvents(gc_instance* inst)
{
	lekmod_portable::RtAutoChoiceEvent ac;
	while (inst->session.AutoChoice().PopEvent(ac))
		inst->session.Events().Push(GC_EVENT_AUTO_CHOICE, 3, (int)ac.kind, ac.player, ac.id, 0, 0, 0, ac.text.c_str());
	lekmod_portable::RtEvent e;
	while (inst->session.Events().Pop(e))
	{
		if (!inst->host.game_event) continue;
		gc_event ev;
		ev.kind = e.kind;
		ev.argc = (uint32_t)e.argc;
		ev.args = e.args;
		ev.text = e.hasText ? e.text.c_str() : NULL;
		ev.text2 = e.hasText2 ? e.text2.c_str() : NULL;
		inst->host.game_event(inst->host.user, &ev);
	}
}

// the safe point: the last gc_pump ended in the human phase (WAITING_HUMAN, also when the test bots take it), no command since, no open AI request
// to a human, no human in auto moves. GC_UNSAFE_NONE at a safe point, else the first reason that does not hold (Q_GAME.unsafe_reason).
int32_t UnsafeReason(gc_instance* inst)
{
	if (!inst->rawWaiting) return GC_UNSAFE_AI_RUNNING;
	if (inst->commandSince) return GC_UNSAFE_COMMAND_GIVEN;
	if (inst->session.Diplo().Pending() > 0) return GC_UNSAFE_AI_REQUEST_OPEN;
	// Live game 931 (2026-10-30): only a human whose turn is active runs auto moves (CvGame::updateMoves processes the humans with isTurnActive). A human who took
	// an AI slot after the AI had played (rt_slots CONNECT, SIMP-056) keeps the AI's auto-moves flag with its turn over and marked ended until the next turn starts;
	// counted here it held every safe point (saves, kicks, replica snapshots of rejoining seats) for the rest of that turn, which another human's blocker made forever.
	for (int i = 0; i < MAX_MAJOR_CIVS; ++i)
	{
		const CvPlayer& p = GET_PLAYER((PlayerTypes)i);
		if (lekmod_portable::IsSeatedHuman(i) && p.isAutoMoves() && p.isTurnActive()) return GC_UNSAFE_AUTOMOVES;
	}
	// Live game 931 (2026-10-30): a seated human's unit or city that is still busy (a move the core times one slice per step,
	// CvUnitMission::CalculateMissionTimer) is no save point for gc_save (RtSession::SavePointProblem), so Q_GAME says no as well; the
	// host saw "safe" here, gc_save refused, and the host's save looped. It asks for a gc_pump, as a command given does (no ABI reason of its own).
	if (inst->session.Engine().NetworkGame())
		for (int i = 0; i < MAX_MAJOR_CIVS; ++i)
			if (lekmod_portable::IsSeatedHuman(i) && GET_PLAYER((PlayerTypes)i).hasBusyUnitOrCity()) return GC_UNSAFE_COMMAND_GIVEN;
	return GC_UNSAFE_NONE;
}

// The reason of an unsafe point as a sentence (gc_save, gc_slot_control).
const char* UnsafeText(int32_t reason)
{
	switch (reason)
	{
	case GC_UNSAFE_AI_RUNNING: return "the last gc_pump did not end in the human phase (the AI is computing); pump until the humans are up";
	case GC_UNSAFE_COMMAND_GIVEN: return "a command was given after the last gc_pump; pump first";
	case GC_UNSAFE_AI_REQUEST_OPEN: return "an AI request to a human is open";
	case GC_UNSAFE_AUTOMOVES: return "a human is in auto moves";
	}
	return "not a safe point";
}

// Instance + staged-call preconditions shared by the entries that need a running game.
int32_t NeedGame(gc_instance* inst, gc_error* err)
{
	if (!inst) return Fail(err, GC_ERR_INVALID_ARG, "instance is NULL");
	if (!inst->gameStarted || !inst->session.InGame()) return Fail(err, GC_ERR_NOT_STARTED, "no game is running (gc_new_game has not completed)");
	return GC_OK;
}

}  // namespace

#define GC_GUARD_BEGIN try {
#define GC_GUARD_END(err)                                                                  \
	}                                                                                      \
	catch (const std::bad_alloc&)                                                          \
	{                                                                                      \
		return Fail(err, GC_ERR_INTERNAL, "out of memory");                                \
	}                                                                                      \
	catch (const std::exception& e)                                                        \
	{                                                                                      \
		lekmod_portable::Log(GC_LOG_ERROR, "gamecore: exception: %s", e.what());           \
		return Fail(err, GC_ERR_INTERNAL, "exception: %s", e.what());                      \
	}                                                                                      \
	catch (...)                                                                            \
	{                                                                                      \
		return Fail(err, GC_ERR_INTERNAL, "unknown exception");                            \
	}

extern "C" uint32_t gc_abi_version(void) { return GC_ABI_VERSION; }

extern "C" gc_instance* gc_create(const gc_host* host, gc_error* err)
{
	if (!host || !host->read_file)
	{
		Fail(err, GC_ERR_INVALID_ARG, "host and host->read_file are required");
		return 0;
	}
	if (g_instance_exists)
	{
		Fail(err, GC_ERR_ALREADY_EXISTS, "an instance already exists in this process");
		return 0;
	}
	gc_instance* inst = 0;
	try
	{
		inst = new gc_instance();
		inst->host = *host;
		g_instance_exists = true;
		lekmod_portable::SetLogSink(LogSink, inst);
		RtSession::LoadStatus ls;
		if (!inst->session.Load(inst->host, ls))
		{
			Fail(err, GC_ERR_INTERNAL, "boot failed at step %s: %s", RtSession::StepName(ls.step), ls.error.c_str());
			delete inst;
			lekmod_portable::SetLogSink(0, 0);
			g_instance_exists = false;
			return 0;
		}
		lekmod_portable::Log(GC_LOG_INFO, "gamecore: ABI v%u instance created", GC_ABI_VERSION);
		Ok(err);
		return inst;
	}
	catch (const std::exception& e)
	{
		Fail(err, GC_ERR_INTERNAL, "exception during boot: %s", e.what());
	}
	catch (...)
	{
		Fail(err, GC_ERR_INTERNAL, "unknown exception during boot");
	}
	try
	{
		delete inst;
	}
	catch (...)
	{
	}
	lekmod_portable::SetLogSink(0, 0);
	g_instance_exists = false;
	return 0;
}

extern "C" void gc_destroy(gc_instance* inst)
{
	if (!inst) return;
	try
	{
		delete inst;  // ~RtSession unloads the core
	}
	catch (...)
	{
	}
	lekmod_portable::SetLogSink(0, 0);
	g_instance_exists = false;
}

extern "C" int32_t gc_lua_register(gc_instance* inst, const char* table, const char* name, gc_lua_fn fn, void* user, gc_error* err)
{
	(void)user;
	if (!inst || !table || !name || !fn) return Fail(err, GC_ERR_INVALID_ARG, "instance, table, name and fn are required");
	return Fail(err, GC_ERR_NOT_IMPLEMENTED, "gc_lua_register is not implemented yet");
}

extern "C" int32_t gc_new_game(gc_instance* inst, const char* setup_json, gc_error* err)
{
	if (!inst || !setup_json) return Fail(err, GC_ERR_INVALID_ARG, "instance and setup_json are required");
	lekmod_portable::ActiveRuleGuard ruleGuard("new_game", true);  //
	GC_GUARD_BEGIN
	if (inst->gameStarted) return Fail(err, GC_ERR_ALREADY_EXISTS, "a game was already started in this instance");
	{
		// Syntax and keys first: a bad setup must not use up the one game of the instance.
		RtSetup probe;
		std::string perr;
		if (!probe.Parse(setup_json, perr)) return Fail(err, GC_ERR_INVALID_ARG, "setup: %s", perr.c_str());
		// N3.18: the map script check falls back to the file list of vfs-index.json, so the index must be read before it (not fatal here: InitScripts reports it).
		if (!inst->session.Vfs().IsLoaded()) { std::string verr; inst->session.Vfs().Load(&inst->host, verr); }
		if (!probe.Validate(inst->session, perr)) return Fail(err, GC_ERR_INVALID_ARG, "setup: %s", perr.c_str());
	}
	inst->gameStarted = true;
	RtSession::NewGameStatus st;
	if (!inst->session.NewGame(inst->host, setup_json, st))
		return Fail(err, GC_ERR_INTERNAL, "new game failed at step %s: %s", RtSession::NewGameStepName(st.step), st.error.c_str());
	InitNetBots(inst);
	Ok(err);
	return GC_OK;
	GC_GUARD_END(err)
}

// The ABI layer's own state from a save's "abi" section (gc_load_game, gc_resync).
// B4 gc-replica: IsPlayerConnected is read by the rules (CvNotifications::Update, CvPlayer), so a replica's slots are the server's at the snapshot,
// set as plain flags (no reconnect logic: the game state already is the server's); later changes come as SLOT records of the journal.
static void ApplySavedConnections(gc_instance* inst)
{
	if (!inst->haveSavedConnected) return;
	for (int i = 0; i < MAX_PLAYERS; ++i) inst->session.Engine().SetSlotState((PlayerTypes)i, (inst->savedConnected[i] & 1) != 0, (inst->savedConnected[i] & 2) != 0);
}

static int32_t RestoreAbiState(gc_instance* inst, const std::vector<uint8_t>& abi, gc_error* err)
{
	lekmod_portable::RtByteReader r(&abi[0], abi.size());
	const uint32_t version = r.U32();
	if (version != 2 && version != kAbiStateVersion) return Fail(err, GC_ERR_INTERNAL, "load: the save's ABI state has another version");
	inst->lastPumpStatus = r.I32();
	inst->gameOverSent = r.Bool();
	inst->rawWaiting = r.Bool();
	inst->botPending = r.Bool();
	if (!lekmod_portable::ReadAutoplayState(inst->autoplay, r)) return Fail(err, GC_ERR_INTERNAL, "load: the save's ABI state is corrupt");
	// v2 (N3.13): the test bots of the humans of a network game, as they were at the save (a slot kicked or taken since the setup is told apart here).
	InitNetBots(inst);
	{
		const bool savedBots = r.Bool();
		if (savedBots)
		{
			inst->netBots = true;
			inst->bots.assign(MAX_PLAYERS, lekmod_portable::RtAutoplay());
			inst->botRound = r.U32();
			for (int i = 0; i < MAX_PLAYERS && r.Ok(); ++i)
			{
				inst->botSlot[i] = r.Bool();
				if (inst->botSlot[i] && !lekmod_portable::ReadAutoplayState(inst->bots[i], r)) return Fail(err, GC_ERR_INTERNAL, "load: the save's ABI state is corrupt (a test bot)");
			}
		}
		else
		{
			inst->netBots = false;
			for (int i = 0; i < MAX_PLAYERS; ++i) inst->botSlot[i] = false;
		}
	}
	inst->haveSavedConnected = version >= 3;
	for (int i = 0; i < MAX_PLAYERS; ++i) inst->savedConnected[i] = inst->haveSavedConnected ? (uint8_t)r.U8() : 0;
	if (!r.Ok() || !r.AtEnd()) return Fail(err, GC_ERR_INTERNAL, "load: the save's ABI state is corrupt");
	return GC_OK;
}

extern "C" int32_t gc_load_game(gc_instance* inst, const uint8_t* p, uint32_t n, gc_error* err)
{
	if (!inst || (n > 0 && !p)) return Fail(err, GC_ERR_INVALID_ARG, "instance and save bytes are required");
	lekmod_portable::ActiveRuleGuard ruleGuard("load_game", true);
	GC_GUARD_BEGIN
	if (inst->gameStarted) return Fail(err, GC_ERR_ALREADY_EXISTS, "a game was already started in this instance");
	if (!inst->session.Loaded()) return Fail(err, GC_ERR_INTERNAL, "the instance did not boot");
	{
		// Magic, format version, length, checksum, sections and build id first: bad data must not use up the one game of the instance.
		std::string why;
		if (!inst->session.CheckSave(p, n, why)) return Fail(err, GC_ERR_BAD_DATA, "gc_load_game: %s", why.c_str());
	}
	inst->gameStarted = true;
	std::vector<uint8_t> abi;
	RtSession::LoadGameStatus st;
	if (!inst->session.LoadGame(inst->host, p, n, abi, st))
		return Fail(err, GC_ERR_INTERNAL, "load failed at step %s: %s", st.step.c_str(), st.error.c_str());
	if (abi.empty())
	{
		// Import of a .Civ5Save (rt_save.h): no ABI state; the layer starts as after gc_new_game.
		InitNetBots(inst);
		Ok(err);
		return GC_OK;
	}
	const int32_t restored = RestoreAbiState(inst, abi, err);
	if (restored != GC_OK) return restored;
	Ok(err);
	return GC_OK;
	GC_GUARD_END(err)
}

extern "C" int32_t gc_pump(gc_instance* inst, uint32_t max_slices, gc_pump_result* out, gc_error* err)
{
	if (!inst || !out || max_slices == 0) return Fail(err, GC_ERR_INVALID_ARG, "instance, out and max_slices >= 1 are required");
	lekmod_portable::ActiveRuleGuard ruleGuard("pump", true);
	GC_GUARD_BEGIN
	const int32_t need = NeedGame(inst, err);
	if (need != GC_OK) return need;
	if (inst->role == GC_ROLE_REPLICA) return Fail(err, GC_ERR_REJECTED, "gc_pump: a replica runs only the server's inputs (gc_journal_apply)");
	RtSession::PumpResult r;
	bool botStuck = false;
	if (inst->botPending)
	{
		// F8: with autoplay_human the test bot holds the human's seat. G4a: its round for the WAITING_HUMAN the previous gc_pump reached runs
		// here, before the pump that delivers its commands (the same call sequence as acting at the end of the previous gc_pump), so between
		// the two calls the game is at a save point. For the host the game keeps running (AI_RUNNING) until the bot cannot go on (STUCK).
		inst->botPending = false;
		std::string why;
		bool acted = true;
		if (inst->netBots)
		{
			// N3.5: a network game. Every waiting bot human gets a round, the first one rotating; a round leaves commands queued, and a human is
			// not waiting while the queue is not empty (B1's IsHumanWaiting rule), so in practice one human acts per step.
			const std::vector<PlayerTypes> waiting = inst->session.WaitingHumans();
			for (size_t j = 0; j < waiting.size() && acted; ++j)
			{
				const PlayerTypes who = waiting[(j + inst->botRound) % waiting.size()];
				if (!inst->botSlot[(int)who] || !inst->session.IsHumanWaiting(who)) continue;
				if (!inst->session.Engine().SlotConnected(who)) continue;  // P8: nobody plays a human who is off line (the turn waits for it)
				acted = inst->bots[(int)who].ActAs(inst->session, (int)who, why);
			}
			++inst->botRound;
		}
		else
			acted = inst->autoplay.Act(inst->session, why);
		if (!acted)
		{
			botStuck = true;
			r.status = RtSession::PUMP_STUCK;
			r.reason = why;
			r.turn = GC.getGame().getGameTurn();
			r.activePlayer = (int)inst->session.LeadHuman();
			r.blocking = inst->session.CurrentBlocking(&r.target);
			lekmod_portable::Log(GC_LOG_WARN, "gamecore: %s", why.c_str());
		}
	}
	if (!botStuck)
	{
		r = inst->session.Pump(max_slices, false);
		if (lekmod_portable::JournalOn())
		{
			// B4 gc-replica: the slices run are an input of the game too (the virtual clock, the turn timer, the AI): a replica runs as many.
			lekmod_portable::JournalRecord jr;
			jr.type = GC_JOURNAL_PUMP;
			jr.slices = r.slices;
			jr.status = (int32_t)r.status;
			jr.turn = (int32_t)r.turn;
			lekmod_portable::JournalAppend(jr);
		}
		inst->rawWaiting = r.status == RtSession::PUMP_WAITING_HUMAN;
		if (inst->rawWaiting && inst->netBots)
		{
			// The bots owe a round for the humans they play; a human nobody plays (a person) keeps the status at WAITING_HUMAN.
			bool anyBot = false, anyOther = false;
			const std::vector<PlayerTypes> waiting = inst->session.WaitingHumans();
			for (size_t j = 0; j < waiting.size(); ++j)
			{
				if (inst->botSlot[(int)waiting[j]] && inst->session.Engine().SlotConnected(waiting[j])) anyBot = true;
				else anyOther = true;
			}
			if (anyBot) inst->botPending = true;
			if (anyBot && !anyOther) r.status = RtSession::PUMP_AI_RUNNING;
		}
		else if (inst->rawWaiting && inst->session.Setup().autoplay_human)
		{
			inst->botPending = true;
			r.status = RtSession::PUMP_AI_RUNNING;
		}
	}
	else
		inst->rawWaiting = false;
	out->turn = r.turn;
	out->status = (int32_t)r.status;  // RtSession::PumpStatus values equal GC_PUMP_*
	out->active_player = r.activePlayer;
	out->blocker = r.blocking;
	out->slices_used = (int32_t)r.slices;
	// v2 (N3.5): the humans the turn still waits for (RtSession::HumansLeft: seated living humans without an end of turn, 0 while the AI civs move).
	out->humans_left = (r.status == RtSession::PUMP_GAME_OVER || r.status == RtSession::PUMP_STUCK) ? 0 : (int32_t)r.humansLeft;
	inst->lastPumpStatus = (int32_t)r.status;
	inst->commandSince = false;
	EmitQueuedEvents(inst);  // TURN_STARTED per turn change (RtSession::Pump) and everything else queued
	if (r.status == RtSession::PUMP_GAME_OVER && !inst->gameOverSent)
	{
		inst->gameOverSent = true;
		CvGame& game = GC.getGame();
		const int32_t args[2] = {(int32_t)game.getWinner(), (int32_t)game.getVictory()};
		EmitEvent(inst, GC_EVENT_GAME_OVER, args, 2);
	}
	if (r.status == RtSession::PUMP_STUCK) return Fail(err, GC_ERR_STUCK, "%s", r.reason.c_str());
	Ok(err);
	return GC_OK;
	GC_GUARD_END(err)
}

extern "C" int32_t gc_apply_command(gc_instance* inst, const gc_command* cmd, gc_error* err)
{
	if (!inst || !cmd || (cmd->argc > 0 && !cmd->args)) return Fail(err, GC_ERR_INVALID_ARG, "instance and a well-formed command are required");
	lekmod_portable::ActiveRuleGuard ruleGuard("apply_command", true);
	GC_GUARD_BEGIN
	const int32_t need = NeedGame(inst, err);
	if (need != GC_OK) return need;
	if (inst->role == GC_ROLE_REPLICA) return Fail(err, GC_ERR_REJECTED, "gc_apply_command: a replica takes no commands; its UI's go to the server (gc_net_take)");
	std::string msg;
	int32_t reason = GC_REASON_NONE;
	const int32_t rc = lekmod_portable::ApplyHumanCommand(inst->session, *cmd, msg, &reason);
	if (rc != GC_OK)
	{
		Fail(err, rc, "gc_apply_command kind %d: %s", (int)cmd->kind, msg.c_str());
		if (err) err->reason = reason;  // v2: the refusal as a code the host can act on
		return rc;
	}
	inst->commandSince = true;
	Ok(err);
	return GC_OK;
	GC_GUARD_END(err)
}

extern "C" int32_t gc_query(gc_instance* inst, int32_t kind, const int32_t* args, uint32_t argc, gc_buf* out, gc_error* err)
{
	if (!inst || !out || (argc > 0 && !args)) return Fail(err, GC_ERR_INVALID_ARG, "instance, out and well-formed args are required");
	lekmod_portable::ActiveRuleGuard ruleGuard("query", true);
	GC_GUARD_BEGIN
	// Q_CATALOG reads only the DB and the info vectors, both loaded by gc_create: the lobby asks it before gc_new_game. Every other query needs the game.
	if (!(kind == GC_Q_CATALOG && inst->session.Loaded()))
	{
		const int32_t need = NeedGame(inst, err);
		if (need != GC_OK) return need;
	}
	gc_buf reply = {0, 0, 0, 0};
	std::string msg;
	RtSession::BlockTarget target;
	const int32_t blocking = inst->session.CurrentBlocking(&target);
	// the queries of a player (args[0] = player: Q_TOP_BAR .. Q_NOTIFICATIONS, Q_DIPLO, Q_REACHABLE_TREE, Q_COMBAT_PREVIEW; audit F-06) are
	// answered as that human, so the getters that look at the local player see it.
	const bool asPlayer = QueryIsForPlayer(kind) && argc >= 1 && lekmod_portable::IsSeatedHuman(args[0]);
	lekmod_portable::ActiveAs queryAs(asPlayer ? (PlayerTypes)args[0] : NO_PLAYER, asPlayer);
	lekmod_portable::GameQueryExtra extra;
	extra.session = &inst->session;
	extra.dirty = &inst->dirty;
	extra.unsafeReason = (kind == GC_Q_GAME && inst->session.InGame()) ? UnsafeReason(inst) : GC_UNSAFE_NONE;
	const int32_t rc = lekmod_portable::RunGameQuery(kind, args, argc, inst->lastPumpStatus, blocking, target, &reply, msg, &extra);
	if (rc != GC_OK) return Fail(err, rc, "gc_query kind %d: %s", (int)kind, msg.c_str());
	*out = reply;
	Ok(err);
	return GC_OK;
	GC_GUARD_END(err)
}

extern "C" int32_t gc_save(gc_instance* inst, gc_buf* out, gc_error* err)
{
	if (!inst || !out) return Fail(err, GC_ERR_INVALID_ARG, "instance and out are required");
	lekmod_portable::ActiveRuleGuard ruleGuard("save", true);
	GC_GUARD_BEGIN
	const int32_t need = NeedGame(inst, err);
	if (need != GC_OK) return need;
	// G4a: a save point is a WAITING_HUMAN the host has not acted on yet (no command queued, turn not ended, no leader message open).
	// SIMPLIFIED(SIMP-026): saves only at such a save point; original: the player saves from the menu at any moment of the turn, observer games too.
	// N3.13: a network game's save point is the human phase, nothing queued or busy (the session), the last gc_pump ended in it, no command since,
	// no AI request open, nobody in auto moves (UnsafeReason). The test bots of the humans are saved with the game.
	std::string problem = inst->session.SavePointProblem();
	if (problem.empty() && !inst->rawWaiting) problem = "the last gc_pump did not end at WAITING_HUMAN; pump until the human is up";
	if (problem.empty() && inst->session.Engine().NetworkGame() && UnsafeReason(inst) != GC_UNSAFE_NONE) problem = UnsafeText(UnsafeReason(inst));
	if (problem.empty() && !inst->netBots && inst->session.Setup().autoplay_human && !inst->botPending) problem = "the test bot has acted on this WAITING_HUMAN; pump on";
	if (!problem.empty()) return Fail(err, GC_ERR_REJECTED, "gc_save: not at a save point: %s", problem.c_str());
	std::vector<uint8_t> abi;
	{
		lekmod_portable::RtByteWriter w(abi);
		w.U32(kAbiStateVersion);
		w.I32(inst->lastPumpStatus);
		w.Bool(inst->gameOverSent);
		w.Bool(inst->rawWaiting);
		w.Bool(inst->botPending);
		lekmod_portable::WriteAutoplayState(inst->autoplay, w);
		w.Bool(inst->netBots);
		if (inst->netBots)
		{
			w.U32(inst->botRound);
			for (int i = 0; i < MAX_PLAYERS; ++i)
			{
				w.Bool(inst->botSlot[i]);
				if (inst->botSlot[i]) lekmod_portable::WriteAutoplayState(inst->bots[i], w);
			}
		}
		for (int i = 0; i < MAX_PLAYERS; ++i)
		{
			const lekmod_portable::RtEngine& e = inst->session.Engine();
			w.U8((e.SlotConnected((PlayerTypes)i) ? 1u : 0u) | (e.SlotOfflineByLoad((PlayerTypes)i) ? 2u : 0u));
		}
	}
	std::vector<uint8_t>* bytes = new std::vector<uint8_t>();
	std::string msg;
	if (!inst->session.SaveGame(abi, *bytes, msg))
	{
		delete bytes;
		return Fail(err, GC_ERR_INTERNAL, "gc_save: %s", msg.c_str());
	}
	out->data = bytes->empty() ? NULL : &(*bytes)[0];
	out->len = bytes->size();
	out->release = ReleaseVector;
	out->owner = bytes;
	Ok(err);
	return GC_OK;
	GC_GUARD_END(err)
}

extern "C" int32_t gc_state_digest(gc_instance* inst, int32_t viewer_team, uint64_t* out, gc_error* err)
{
	if (!inst || !out || viewer_team < -3 || viewer_team >= MAX_TEAMS) return Fail(err, GC_ERR_INVALID_ARG, "instance, out and viewer_team in -3..%d are required", (int)MAX_TEAMS - 1);
	lekmod_portable::ActiveRuleGuard ruleGuard("state_digest", true);
	GC_GUARD_BEGIN
	const int32_t need = NeedGame(inst, err);
	if (need != GC_OK) return need;
	std::string msg;
	uint64_t digest = 0;
	if (viewer_team == -3)
	{
		const int32_t rc3 = lekmod_portable::ReplicaDigest(inst->session, &digest, msg);
		if (rc3 != GC_OK) return Fail(err, rc3, "gc_state_digest: %s", msg.c_str());
		*out = digest;
		Ok(err);
		return GC_OK;
	}
	RtSession::BlockTarget target;
	const int32_t blocking = inst->session.CurrentBlocking(&target);
	const int32_t rc = lekmod_portable::ViewerDigest(viewer_team, inst->lastPumpStatus, blocking, target, &digest, msg);
	if (rc != GC_OK) return Fail(err, rc, "gc_state_digest: %s", msg.c_str());
	*out = digest;
	Ok(err);
	return GC_OK;
	GC_GUARD_END(err)
}

extern "C" int32_t gc_build_id(gc_instance* inst, gc_buf* out, gc_error* err)
{
	if (!inst || !out) return Fail(err, GC_ERR_INVALID_ARG, "instance and out are required");
	GC_GUARD_BEGIN
	const std::string id = inst->session.BuildId();
	std::vector<uint8_t>* bytes = new std::vector<uint8_t>(id.begin(), id.end());
	out->data = bytes->empty() ? NULL : &(*bytes)[0];
	out->len = bytes->size();
	out->release = ReleaseVector;
	out->owner = bytes;
	Ok(err);
	return GC_OK;
	GC_GUARD_END(err)
}

// P8, N3.12: who plays a slot. Only at a safe point; the work is rt_slots.cpp's.
extern "C" int32_t gc_slot_control(gc_instance* inst, int32_t player, int32_t op, const char* nick, gc_error* err)
{
	if (!inst) return Fail(err, GC_ERR_INVALID_ARG, "instance is NULL");
	lekmod_portable::ActiveRuleGuard ruleGuard("slot_control", true);
	if (player < 0 || player >= MAX_MAJOR_CIVS) return Fail(err, GC_ERR_INVALID_ARG, "gc_slot_control: player %d is not a major civ slot", (int)player);
	if (op != GC_SLOT_CONNECT && op != GC_SLOT_DISCONNECT && op != GC_SLOT_KICK) return Fail(err, GC_ERR_INVALID_ARG, "gc_slot_control: unknown op %d", (int)op);
	if (nick && strlen(nick) > 64) return Fail(err, GC_ERR_INVALID_ARG, "gc_slot_control: nick longer than 64 bytes");
	GC_GUARD_BEGIN
	const int32_t need = NeedGame(inst, err);
	if (need != GC_OK) return need;
	if (inst->role == GC_ROLE_REPLICA) return Fail(err, GC_ERR_REJECTED, "gc_slot_control: a replica follows the server's slot changes (gc_journal_apply)");
	if (!inst->session.Engine().NetworkGame())
	{
		// The human of a single game is always on line: CONNECT of that slot changes nothing (a host that hands the slots to their owners after a load,
		// such as game-host's worker on a .Civ5Save import, rt_save.h).
		if (op == GC_SLOT_CONNECT && GET_PLAYER((PlayerTypes)player).isHuman())
		{
			Ok(err);
			return GC_OK;
		}
		Fail(err, GC_ERR_REJECTED, "gc_slot_control: only the network game type has slots to change");
		if (err) err->reason = GC_REASON_SLOT_STATE;
		return GC_ERR_REJECTED;
	}
	const int32_t unsafe = UnsafeReason(inst);
	if (unsafe != GC_UNSAFE_NONE)
	{
		Fail(err, GC_ERR_REJECTED, "gc_slot_control: not at a safe point: %s", UnsafeText(unsafe));
		if (err) err->reason = GC_REASON_NOT_SAFE_POINT;
		return GC_ERR_REJECTED;
	}
	lekmod_portable::RtSlotChange change;
	std::string msg;
	int32_t reason = GC_REASON_NONE;
	const int32_t rc = lekmod_portable::ControlSlot(inst->session, player, op, nick, change, msg, &reason);
	if (rc != GC_OK)
	{
		Fail(err, rc, "%s", msg.c_str());
		if (err) err->reason = reason;
		return rc;
	}
	// The test bots follow the slot: a kicked human is an AI now, a new human on an AI slot is played by a bot when the game's humans are (a test only).
	if (op == GC_SLOT_KICK && inst->netBots) inst->botSlot[player] = false;
	if (change.tookAiSlot && inst->netBots)
	{
		inst->botSlot[player] = true;
		inst->bots[player] = lekmod_portable::RtAutoplay();
	}
	// A kicked human's AI has its turn still to play: the AI is computing until the next gc_pump (a save or a slot change must wait for it).
	if (op == GC_SLOT_KICK) inst->rawWaiting = false;
	if (lekmod_portable::JournalOn())
	{
		lekmod_portable::JournalRecord jr;
		jr.type = GC_JOURNAL_SLOT;
		jr.player = player;
		jr.op = op;
		jr.hasText = nick != NULL;
		if (nick) jr.text = nick;
		lekmod_portable::JournalAppend(jr);
	}
	EmitQueuedEvents(inst);
	Ok(err);
	return GC_OK;
	GC_GUARD_END(err)
}

// ---- B4 gc-replica ----------------------------------------------------------------------------------------

namespace {

void ReleaseBytes(gc_buf* b) { delete static_cast<std::vector<uint8_t>*>(b->owner); }

void HandOut(std::vector<uint8_t>* bytes, gc_buf* out)
{
	out->data = bytes->empty() ? NULL : &(*bytes)[0];
	out->len = bytes->size();
	out->release = ReleaseBytes;
	out->owner = bytes;
}

}  // namespace

extern "C" int32_t gc_set_role(gc_instance* inst, int32_t role, int32_t player, gc_error* err)
{
	if (!inst) return Fail(err, GC_ERR_INVALID_ARG, "instance is NULL");
	if (role != GC_ROLE_PLAIN && role != GC_ROLE_SERVER && role != GC_ROLE_REPLICA) return Fail(err, GC_ERR_INVALID_ARG, "gc_set_role: unknown role %d", (int)role);
	if (player < -1 || player >= MAX_MAJOR_CIVS) return Fail(err, GC_ERR_INVALID_ARG, "gc_set_role: player %d is not -1 or a major civ slot", (int)player);
	GC_GUARD_BEGIN
	if (role == GC_ROLE_REPLICA)
	{
		const int32_t need = NeedGame(inst, err);
		if (need != GC_OK) return need;
		// The server's test bots and inputs drive the game; the replica's own bots never act.
		inst->netBots = false;
		inst->botPending = false;
		for (int i = 0; i < MAX_PLAYERS; ++i) inst->botSlot[i] = false;
		ApplySavedConnections(inst);
	}
	inst->role = role;
	inst->replicaPlayer = role == GC_ROLE_REPLICA ? player : -1;
	lekmod_portable::JournalEnable(role == GC_ROLE_SERVER);
	inst->session.Engine().SetOutbox(role == GC_ROLE_REPLICA);
	// B4 M2 WO-1 (the UI bridge): a replica serves a UI. The engine is told to have graphics (the DLL then announces what the UI draws), the interface half is on,
	// and between two gc_* calls the active player is the seat (rt_active.h). Any other role is as before.
	if (inst->session.InGame())
	{
		inst->session.UiReplica().SetRole(role == GC_ROLE_REPLICA, (PlayerTypes)player);
		GC.SetGraphicsInitialized(role == GC_ROLE_REPLICA);
		lekmod_portable::SetRestPlayer(role == GC_ROLE_REPLICA && player >= 0 ? (PlayerTypes)player : NO_PLAYER);
	}
	Ok(err);
	return GC_OK;
	GC_GUARD_END(err)
}

extern "C" int32_t gc_journal_take(gc_instance* inst, uint32_t flags, gc_buf* out, gc_error* err)
{
	if (!inst || !out) return Fail(err, GC_ERR_INVALID_ARG, "instance and out are required");
	lekmod_portable::ActiveRuleGuard ruleGuard("journal_take", true);
	GC_GUARD_BEGIN
	if (inst->role != GC_ROLE_SERVER) return Fail(err, GC_ERR_REJECTED, "gc_journal_take: only a server (GC_ROLE_SERVER) keeps a journal");
	if ((flags & GC_JOURNAL_TAKE_CHECK) && lekmod_portable::JournalSize() > 0 && inst->gameStarted && inst->session.InGame())
	{
		lekmod_portable::JournalRecord jr;
		jr.type = GC_JOURNAL_CHECK;
		std::string msg;
		const int32_t rc = lekmod_portable::ReplicaDigest(inst->session, &jr.digest, msg);
		if (rc != GC_OK) return Fail(err, rc, "gc_journal_take: %s", msg.c_str());
		lekmod_portable::JournalAppend(jr);
	}
	std::vector<uint8_t>* bytes = new std::vector<uint8_t>();
	lekmod_portable::JournalTake(*bytes);
	HandOut(bytes, out);
	Ok(err);
	return GC_OK;
	GC_GUARD_END(err)
}

extern "C" int32_t gc_journal_apply(gc_instance* inst, const uint8_t* p, uint32_t n, gc_error* err)
{
	if (!inst || (n > 0 && !p)) return Fail(err, GC_ERR_INVALID_ARG, "instance and journal bytes are required");
	lekmod_portable::ActiveRuleGuard ruleGuard("journal_apply", true);
	GC_GUARD_BEGIN
	const int32_t need = NeedGame(inst, err);
	if (need != GC_OK) return need;
	if (inst->role != GC_ROLE_REPLICA) return Fail(err, GC_ERR_REJECTED, "gc_journal_apply: only a replica (GC_ROLE_REPLICA) replays a journal");
	std::vector<lekmod_portable::JournalRecord> recs;
	std::string why;
	if (!lekmod_portable::DecodeJournal(p, n, recs, why)) return Fail(err, GC_ERR_BAD_DATA, "gc_journal_apply: %s", why.c_str());
	RtSession& s = inst->session;
	for (size_t i = 0; i < recs.size(); ++i)
	{
		const lekmod_portable::JournalRecord& r = recs[i];
		switch (r.type)
		{
		case GC_JOURNAL_COMMAND:
		{
			lekmod_portable::RtCoreScope core(s.Engine());
			gc_command cmd;
			memset(&cmd, 0, sizeof(cmd));
			cmd.kind = (uint16_t)r.kind;
			cmd.player = (uint8_t)r.player;
			cmd.argc = (uint32_t)r.args.size();
			cmd.args = r.args.empty() ? NULL : &r.args[0];
			cmd.text = r.hasText ? r.text.c_str() : NULL;
			std::string msg;
			const int32_t rc = lekmod_portable::ApplyHumanCommand(s, cmd, msg, NULL);
			if (rc != GC_OK)
			{
				EmitQueuedEvents(inst);
				return Fail(err, GC_ERR_DIVERGED, "gc_journal_apply: record %u: command %d of player %d refused here: %s", (unsigned)i, r.kind, r.player, msg.c_str());
			}
			inst->commandSince = true;
			break;
		}
		case GC_JOURNAL_NET:
		{
			lekmod_portable::RtNetMessage m = r.net;
			m.ePlayer = (PlayerTypes)r.player;
			s.Engine().Inject(m);
			inst->commandSince = true;
			break;
		}
		case GC_JOURNAL_PUMP:
		{
			const RtSession::PumpResult pr = s.Pump(r.slices, false);
			inst->rawWaiting = pr.status == RtSession::PUMP_WAITING_HUMAN;
			inst->lastPumpStatus = (int32_t)pr.status;
			inst->commandSince = false;
			EmitQueuedEvents(inst);
			if (pr.slices != r.slices || (int32_t)pr.status != r.status || (int32_t)pr.turn != r.turn)
				return Fail(err, GC_ERR_DIVERGED, "gc_journal_apply: record %u: pump ran %u slices to status %d, turn %d; the server's ran %u to status %d, turn %d", (unsigned)i,
					(unsigned)pr.slices, (int)pr.status, (int)pr.turn, (unsigned)r.slices, (int)r.status, (int)r.turn);
			break;
		}
		case GC_JOURNAL_SLOT:
		{
			lekmod_portable::RtCoreScope core(s.Engine());
			lekmod_portable::RtSlotChange change;
			std::string msg;
			const int32_t rc = lekmod_portable::ControlSlot(s, r.player, r.op, r.hasText ? r.text.c_str() : NULL, change, msg, NULL);
			if (r.op == GC_SLOT_KICK) inst->rawWaiting = false;
			EmitQueuedEvents(inst);
			if (rc != GC_OK) return Fail(err, GC_ERR_DIVERGED, "gc_journal_apply: record %u: slot change %d of player %d refused here: %s", (unsigned)i, r.op, r.player, msg.c_str());
			break;
		}
		case GC_JOURNAL_CHECK:
		{
			uint64_t mine = 0;
			std::string msg;
			const int32_t rc = lekmod_portable::ReplicaDigest(s, &mine, msg);
			if (rc != GC_OK) return Fail(err, rc, "gc_journal_apply: %s", msg.c_str());
			if (mine != r.digest)
				return Fail(err, GC_ERR_DIVERGED, "gc_journal_apply: record %u: digest %016llx, the server's %016llx", (unsigned)i, (unsigned long long)mine, (unsigned long long)r.digest);
			break;
		}
		default: break;
		}
	}
	if (inst->role == GC_ROLE_REPLICA)
	{
		// B4 M2 WO-1: what the batch did to the seat's notifications and fog is told to its UI (the DLL announces only what the active player sees, and the active
		// player of a replay is the server seat).
		s.UI().SyncReplicaNotifications();
		s.UiReplica().SyncFog();
	}
	Ok(err);
	return GC_OK;
	GC_GUARD_END(err)
}

extern "C" int32_t gc_net_take(gc_instance* inst, gc_buf* out, gc_error* err)
{
	if (!inst || !out) return Fail(err, GC_ERR_INVALID_ARG, "instance and out are required");
	GC_GUARD_BEGIN
	std::vector<lekmod_portable::RtNetMessage> msgs;
	inst->session.Engine().TakeOutbox(msgs);
	std::vector<uint8_t>* bytes = new std::vector<uint8_t>();
	lekmod_portable::EncodeNet(msgs, *bytes);
	HandOut(bytes, out);
	Ok(err);
	return GC_OK;
	GC_GUARD_END(err)
}

extern "C" int32_t gc_net_submit(gc_instance* inst, int32_t player, const uint8_t* p, uint32_t n, gc_error* err)
{
	if (!inst || (n > 0 && !p)) return Fail(err, GC_ERR_INVALID_ARG, "instance and message bytes are required");
	lekmod_portable::ActiveRuleGuard ruleGuard("net_submit", true);
	GC_GUARD_BEGIN
	const int32_t need = NeedGame(inst, err);
	if (need != GC_OK) return need;
	if (inst->role == GC_ROLE_REPLICA) return Fail(err, GC_ERR_REJECTED, "gc_net_submit: a replica sends its UI's messages to the server (gc_net_take)");
	std::vector<lekmod_portable::RtNetMessage> msgs;
	std::string why;
	if (!lekmod_portable::DecodeNet(p, n, msgs, why)) return Fail(err, GC_ERR_INVALID_ARG, "gc_net_submit: %s", why.c_str());
	// All or nothing: every message is checked before the first is queued.
	for (size_t i = 0; i < msgs.size(); ++i)
	{
		if (!lekmod_portable::NetMessageFromUi(msgs[i].eId))
		{
			Fail(err, GC_ERR_REJECTED, "gc_net_submit: message %u (%s) is not one a UI sends", (unsigned)i, lekmod_portable::NetMessageName(msgs[i].eId));
			if (err) err->reason = GC_REASON_RULE;
			return GC_ERR_REJECTED;
		}
	}
	std::string msg;
	int32_t reason = GC_REASON_NONE;
	if (!lekmod_portable::CheckHumanMayAct(inst->session, player, msg, &reason))
	{
		Fail(err, GC_ERR_REJECTED, "gc_net_submit: %s", msg.c_str());
		if (err) err->reason = reason;
		return GC_ERR_REJECTED;
	}
	for (size_t i = 0; i < msgs.size(); ++i)
	{
		lekmod_portable::RtNetMessage m = msgs[i];
		m.ePlayer = (PlayerTypes)player;  // the sender is the seat of the connection, never what the client wrote
		inst->session.Engine().Inject(m);
		lekmod_portable::JournalRecord jr;
		jr.type = GC_JOURNAL_NET;
		jr.player = player;
		jr.net = m;
		lekmod_portable::JournalAppend(jr);
	}
	inst->commandSince = true;
	Ok(err);
	return GC_OK;
	GC_GUARD_END(err)
}

extern "C" int32_t gc_resync(gc_instance* inst, const uint8_t* p, uint32_t n, gc_error* err)
{
	if (!inst || (n > 0 && !p)) return Fail(err, GC_ERR_INVALID_ARG, "instance and save bytes are required");
	lekmod_portable::ActiveRuleGuard ruleGuard("resync", true);
	GC_GUARD_BEGIN
	const int32_t need = NeedGame(inst, err);
	if (need != GC_OK) return need;
	{
		std::string why;
		if (!inst->session.CheckSave(p, n, why)) return Fail(err, GC_ERR_BAD_DATA, "gc_resync: %s", why.c_str());
	}
	std::vector<uint8_t> abi;
	RtSession::LoadGameStatus st;
	// B4 M2 WO-1: the old game going and the new one coming are not news for a replica's UI (gc_ui_setup_graphical announces the new one).
	inst->session.UiReplica().SetMuted(true);
	const bool resynced = inst->session.ResyncGame(inst->host, p, n, abi, st);
	inst->session.UiReplica().SetMuted(false);
	if (!resynced) return Fail(err, GC_ERR_INTERNAL, "gc_resync failed at step %s: %s", st.step.c_str(), st.error.c_str());
	inst->dirty.Reset();
	inst->commandSince = false;
	if (!abi.empty())
	{
		const int32_t restored = RestoreAbiState(inst, abi, err);
		if (restored != GC_OK) return restored;
	}
	if (inst->role == GC_ROLE_REPLICA)
	{
		inst->netBots = false;
		inst->botPending = false;
		for (int i = 0; i < MAX_PLAYERS; ++i) inst->botSlot[i] = false;
		ApplySavedConnections(inst);
	}
	inst->session.Engine().SetOutbox(inst->role == GC_ROLE_REPLICA);
	if (inst->role == GC_ROLE_REPLICA)
	{
		GC.SetGraphicsInitialized(true);
		inst->session.UiReplica().Revalidate();
		inst->session.UiReplica().ForgetFog();
	}
	EmitQueuedEvents(inst);
	Ok(err);
	return GC_OK;
	GC_GUARD_END(err)
}

extern "C" void gc_buf_release(gc_buf* buf)
{
	if (!buf) return;
	if (buf->release) buf->release(buf);
	buf->data = 0;
	buf->len = 0;
	buf->release = 0;
	buf->owner = 0;
}
