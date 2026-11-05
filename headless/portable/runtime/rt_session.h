// rt_session: one game instance of the portable runtime (one game instance).
// Load() is the civhost boot as direct calls: D1-D2 (DllMain, CvDllGameContext), B1-B4 (game DB from memory,
// en_US localization) and I1-I8 (exe ReloadGameCoreDLL / SetActiveDLCandMods: SetDLLIFace, SetGameDatabase,
// ctx->Init(), logging off, SetEngineUserInterface, Flush / PostProcessing / CacheGameDatabaseData, DLC allowed).
// NewGame() is milestone M2: Lua boot, setup JSON -> RtSetup (G1-G9), L1-L6 (LaunchToMap: cache,
// closeInactiveSlots, Init(handicap), Init2 = map script, LaterInit x64, InitTacticalAnalysisMap, SetInitialTime), L7 the
// 37 gameplay contexts, L8 DoGameStarted + SendPlayerOptions, L9 Events.SequenceGameInitComplete.
// Pump() is the turn loop (M3) with the human's turn (F1, M4a: UpdateTestEndTurn, end-turn blockers and their targets, EndTurn()).
// Ported from headless/host/src/dllhost/HstDllHost.cpp, control/HstSession.cpp (Run boot part, NewGameImpl, StartGame) and
// game/HstGameRunner.cpp (LaunchToMap, StartGame, LogMapSummary).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "gamecore_abi.h"
#include "rt_autochoice.h"
#include "rt_db.h"
#include "rt_diplo.h"
#include "rt_engine.h"
#include "rt_events.h"
#include "rt_setup.h"
#include "rt_ui.h"
#include "rt_ui_replica.h"
#include "rt_vfs.h"

class CvDllGameContext;

namespace lekmod_portable {

class RtScriptSystem;

class RtSession
{
public:
	// New-game steps (civhost names: G = pregame, L = launch); NewGameStatus.step names the last one reached.
	enum NewGameStep
	{
		NG_NONE = 0,
		NG_S1,    // Lua boot: vfs-index.json (rt_vfs), master state, exe-Lua, RegisterScriptLibraries, gDLL script system
		NG_G0,    // setup JSON parse (RtSetup::Parse)
		NG_G1,    // G1-G9 (RtSetup::Apply)
		NG_L1,    // CacheGameDatabaseData (no-op when cached)
		NG_L2,    // closeInactiveSlots
		NG_L3,    // game->Init(handicap)
		NG_L4,    // game->Init2(): InitPlayers, InitMap (Lua map script), initDiplomacy, setInitialItems
		NG_L5,    // LaterInit x64, InitTacticalAnalysisMap
		NG_L6,    // SetInitialTime(0)
		NG_L7,    // gameplay contexts
		NG_L8,    // DoGameStarted, SendPlayerOptions(true), Clear*Deltas
		NG_L9,    // Events.SequenceGameInitComplete
		NG_DONE
	};
	static const char* NewGameStepName(int step);

	struct NewGameStatus
	{
		int step;
		std::string error;
		std::string setup;           // RtSetup::Describe()
		bool init2;                  // Init2() result
		int width, height, plots;    // map after Init2
		bool wrapX, wrapY;
		uint64_t mapDigest;          // MapDigest() right after L6 (map generation done, before contexts)
		uint64_t mapDigestStarted;   // MapDigest() after L9
		uint64_t mapWriteDigest;     // civhost's definition: FNV-1a 64 of CvMap::Write (includes the map GUID and derived state)
		std::vector<unsigned char> mapWrite;  // those CvMap::Write bytes (taken after L6, as civhost's map-write.bin)
		int luaErrorsMap;            // Lua errors reported during S1-L6 (map script)
		int luaErrorsContexts;       // during L7
		int luaErrorsStart;          // during L8-L9
		int contextsTotal, contextsLoaded;
		int initListeners;           // Events.SequenceGameInitComplete listeners called
		int turn;                    // game turn after L9
		int majorsAlive, minorsAlive, units, startPlots;
		double msLaunch, msContexts, msStart;  // wall time of L1-L6, L7, L8-L9 (informational only)
		double msInit, msInit2;      // wall time of game->Init(handicap) (L3) and game->Init2() = the map script (L4)
		NewGameStatus()
			: step(NG_NONE), init2(false), width(0), height(0), plots(0), wrapX(false), wrapY(false), mapDigest(0), mapDigestStarted(0),
			  mapWriteDigest(0), luaErrorsMap(0), luaErrorsContexts(0), luaErrorsStart(0), contextsTotal(0), contextsLoaded(0), initListeners(0),
			  turn(-1), majorsAlive(0), minorsAlive(0), units(0), startPlots(0), msLaunch(0), msContexts(0), msStart(0), msInit(0), msInit2(0) {}
	};

	// Boot steps in civhost order; LoadStatus.step names the one that failed.
	enum Step
	{
		STEP_NONE = 0,
		STEP_D1,  // DllMain attach: CvDllGameContext singleton
		STEP_D2,  // CvDllGameContext::GetSingleton
		STEP_B1,  // read db/gameplay.db, db/loc-en_us.db through gc_host.read_file
		STEP_B2,  // gameplay DB from the buffer
		STEP_B3,  // attach en_US localization
		STEP_B4,  // Localization::Initialize
		STEP_I1,  // cache folder of ./Civ5SavedGameDatabase.db (rt_fs)
		STEP_I2,  // SetDLLIFace
		STEP_I3,  // SetGameDatabase
		STEP_I4,  // ctx->Init(): GC.init (CvGame/CvMap, post-processing, cache)
		STEP_I5,  // logging off
		STEP_I6,  // SetEngineUserInterface
		STEP_I7,  // database utility: Flush, PerformDatabasePostProcessing, CacheGameDatabaseData
		STEP_I8,  // pregame DLC allowed set from DownloadableContent
		STEP_DONE
	};
	static const char* StepName(int step);

	struct LoadStatus
	{
		int step;            // last step reached (STEP_DONE on success)
		std::string error;   // why it stopped, empty on success
		bool flush, postProcess, cache;  // I7 results
		int dlcAllowed;      // I8
		LoadStatus() : step(STEP_NONE), flush(false), postProcess(false), cache(false), dlcAllowed(0) {}
	};

	RtSession();
	~RtSession();

	// Boots the gamecore for this instance. One RtSession per process (the gamecore is a singleton);
	// a second Load returns false.
	bool Load(const gc_host& host, LoadStatus& status);
	bool Loaded() const { return m_loaded; }

	// A new game from the setup JSON after Load(): Lua boot when not done yet, then G1-G9, L1-L9.
	// Once per instance (a second game in one process diverges); a second call returns false.
	bool NewGame(const gc_host& host, const std::string& setupJson, NewGameStatus& status);
	bool InGame() const { return m_inGame; }

	// Lua boot (S1) on its own; NewGame calls it when needed. False + error on failure.
	bool InitScripts(const gc_host& host, std::string& error);

	// Map digest (M2): FNV-1a 64 over the canonical map text MapText(); identical maps give identical digests on every
	// target. The text is "W H\n" and per plot row y one line "y:" followed by, per plot x = 0..W-1,
	// "plotType,terrain,feature,resource,numResource,NEOfRiver,WOfRiver,NWOfRiver,riverEFlow,riverSEFlow,riverSWFlow;"
	// (resource = getResourceType(NO_TEAM); river flags 0/1; flows FlowDirectionTypes, -1 = none). The Lua probe
	// runtime/lua/newgame/map_probe.lua prints the same rows, so civhost's map can be digested the same way.
	static std::string MapText();
	static uint64_t MapDigest();
	static uint64_t Fnv1a64(const void* data, size_t len, uint64_t hash = 14695981039346656037ull);

	// ---- turn loop (A9, M3; human turn F1, M4a) ----------------------------------------------------------------------------
	enum PumpStatus
	{
		PUMP_WAITING_HUMAN,  // the human's turn is active, the AI civs are done, nothing is busy and turn complete is not sent; blocking says what holds it
		PUMP_AI_RUNNING,     // the slice budget ran out (or, with stopOnTurnChange, a turn ended); call Pump again
		PUMP_GAME_OVER,      // CvGame::getGameState() == GAMESTATE_OVER
		PUMP_STUCK           // the turn did not end within kMaxSlicesPerTurn slices, no game is running, or (NoHumanInput) a choice blocker is left to a human
                             // nobody is there to be; reason says why
	};
	static const char* PumpStatusName(int status);
	// "ENDTURN_BLOCKING_UNITS" for ENDTURN_BLOCKING_UNITS, "NONE" for NO_ENDTURN_BLOCKING_TYPE.
	static const char* BlockingName(int blocking);
	// A turn that needs more slices than this is stuck (civhost default --max-slices 20000 = 5000 s of virtual time). Slices the human
	// spends in WAITING_HUMAN do not count: the budget restarts every time the human is up.
	static const uint32_t kMaxSlicesPerTurn = 20000;
	// Most slices one network Pump runs past WAITING_HUMAN for a human's busy unit or city (live game 931): a move is timed one slice per
	// step, so this covers the longest move of a turn; past it the pump stops as before.
	static const uint32_t kMaxSettleSlices = 64;

	// What an end-turn blocker points at, so a UI can jump to it. Units: the first ready unit (UNITS, STACKED_UNITS) or the
	// first unit with a promotion ready (UNIT_PROMOTION). Notification blockers: the notification's plot; CITY when the active player owns
	// a city there (PRODUCTION, CITY_RANGE_ATTACK); NONE when the notification has no plot (RESEARCH, POLICY, ...).
	enum TargetKind { TARGET_NONE, TARGET_UNIT, TARGET_CITY, TARGET_PLOT };
	static const char* TargetKindName(int kind);
	struct BlockTarget
	{
		TargetKind kind;
		int owner;             // player owning the unit / city, -1 otherwise
		int id;                // unit or city id, -1 for PLOT / NONE
		int x, y;              // the plot, -1 when none
		int notification;      // GetEndTurnBlockingNotificationIndex (lookup id), -1 for unit blockers
		int notificationType;  // NotificationTypes of that notification, -1 when unknown
		BlockTarget() : kind(TARGET_NONE), owner(-1), id(-1), x(-1), y(-1), notification(-1), notificationType(-1) {}
	};

	struct PumpResult
	{
		PumpStatus status;
		int turn;              // game turn after the call
		int activePlayer;      // the active (local) player: the human, or the observer slot
		int blocking;          // EndTurnBlockingTypes of the active player after UpdateTestEndTurn (NO_ENDTURN_BLOCKING_TYPE = -1)
		BlockTarget target;    // what the blocker points at
		bool canEndTurn;       // the host's canEndTurn (true: RtSession::EndTurn(false) would be accepted)
		uint32_t slices;       // Update() slices run by this call
		uint32_t turnSlices;   // slices spent in the current turn so far (across calls; restarts when the human is up)
		int turnsAdvanced;     // game-turn changes during this call
		int netMessages;       // loopback commands delivered by this call
		int humansLeft;        // v2: humans the turn waits for (HumansLeft() at the end of the call; 0 while the AI moves)
		int waitingHumans;     // network game: humans for whom IsHumanWaiting() holds when status is WAITING_HUMAN (a subset of humansLeft: the others are queued in hybrid)
		double ms;             // wall time of this call (informational only)
		std::string reason;    // STUCK / GAME_OVER detail
		PumpResult() : status(PUMP_AI_RUNNING), turn(-1), activePlayer(-1), blocking(-1), canEndTurn(false), slices(0), turnSlices(0), turnsAdvanced(0), netMessages(0), humansLeft(0), waitingHumans(0), ms(0) {}
	};

	// Runs at most maxSlices slices of: loopback delivery -> +250 ms virtual time -> ICvGame1::Update() -> ICvGame1::UpdateTestEndTurn()
	// -> host canEndTurn. Returns early when the game is over, the human is up (WAITING_HUMAN; every further call runs one slice, so queued
	// human commands are delivered and their missions run), the turn is stuck, or (stopOnTurnChange) a turn ended. The budget is in slices,
	// never in wall time. Needs InGame().
	PumpResult Pump(uint32_t maxSlices, bool stopOnTurnChange = false);

	// The human's "end turn": CONTROL_ENDTURN when there is no blocker; with force, CONTROL_FORCEENDTURN when the blocker is
	// UNITS or STACKED_UNITS (the gamecore itself accepts FORCEENDTURN only for UNITS, CvGame.cpp:3913: STACKED_UNITS comes back rejected);
	// any other blocker rejects the command with that blocker and its target. ACCEPTED means turn complete was sent: the next Pump calls
	// deliver it and run the AI turn.
	enum EndTurnStatus
	{
		END_TURN_ACCEPTED,
		END_TURN_REJECTED_BLOCKED,    // blocking / target say why
		END_TURN_REJECTED_NOT_READY   // not the human's turn now (no game, AI still running, already ended, units busy); reason says why
	};
	static const char* EndTurnStatusName(int status);
	struct EndTurnResult
	{
		EndTurnStatus status;
		int turn;
		int blocking;          // the blocker when the command was given
		BlockTarget target;
		bool forced;           // CONTROL_FORCEENDTURN was used
		std::string reason;
		EndTurnResult() : status(END_TURN_REJECTED_NOT_READY), turn(-1), blocking(-1), forced(false) {}
	};
	EndTurnResult EndTurn(bool force);
	// The same for human `who` of a network game: runs as that human (ActiveAs), so the active player is not who's concern.
	EndTurnResult EndTurn(bool force, PlayerTypes who);

	// True while PUMP_WAITING_HUMAN holds: the active player is a living human whose turn is active, the AI civs and unit AI are done,
	// turn complete is not sent, no loopback command is queued and no unit or city of theirs is busy.
	bool IsHumanWaiting();
	// The same for one human (spike S1: network game, several humans); WaitingHumans lists every human for whom it holds.
	bool IsHumanWaiting(PlayerTypes who);
	std::vector<PlayerTypes> WaitingHumans();
	// A seated human has a busy unit or city (CvPlayer::hasBusyUnitOrCity: a move still timed, a combat): no save point, END_TURN
	// asks for a pump; a network Pump does not stop for WAITING_HUMAN while it holds (at most kMaxSettleSlices slices).
	bool SeatedHumanBusy() const;
	// The human the no-argument IsHumanWaiting / CurrentBlocking and the pump's active_player mean: the first waiting human (network game), the active player (single).
	PlayerTypes LeadHuman();

	// ---- humans of a network game (task N3.5) ----
	// The AI civs and the unit AI of this turn are done: the humans may act (Q_GAME.phase 0). False while the AI moves.
	bool HumanPhase() const;
	// Humans the turn waits for: in the human phase, the living seated humans (SS_TAKEN) that have not sent turn complete (in a hybrid turn a queued human
	// counts before its turn starts); 0 in the AI phase. A single-player game: the one human.
	int HumansLeft() const;
	// Living seated humans (SS_TAKEN majors; the server seat is not one).
	int SeatedHumans() const;
	// The slot is, or was, a human of this game (the setup's "players" kind human, or seated now): the slots Q_TURN lists.
	bool EverHuman(int player) const;
	// Network game: every seated human is dead (and there was one): the game is over for the server although the seat does not die (decision 3.13).
	bool AllSeatedHumansDead() const;
	// The active player's blocker (after the last UpdateTestEndTurn) and what it points at.
	int CurrentBlocking(BlockTarget* target);
	// The blocker of human `who` (after the last UpdateTestEndTurn run for who) and what it points at.
	int CurrentBlocking(BlockTarget* target, PlayerTypes who);

	// Cheap state hash for determinism checks: StateText() lists the game turn, RNG call count and per player (alive, cities, units, score,
	// gold, techs, policies, population) and the plot owners; StateDigest() = FNV-1a 64 of that text and the map text.
	static std::string StateText();
	static uint64_t StateDigest();

	CvDllGameContext* Context() { return m_ctx; }
	RtEngine& Engine() { return m_engine; }
	RtUserInterface& UI() { return m_ui; }
	// B4 M2 WO-1: the interface half of a replica (events for the UI, selection, interface mode); idle in every other role.
	RtUiReplica& UiReplica() { return m_uiRep; }
	// AI diplomacy towards the human, auto-declined after every Update (F6, SIMP-003): pending state, events (TakeEvents), counts.
	RtDiplo& Diplo() { return m_diplo; }
	// Events for the ABI (F7): Pump pushes TURN_STARTED and the diplomacy auto-declines, rt_ui / rt_engine / rt_lua the rest; gc_pump drains them.
	RtEventQueue& Events() { return m_events; }
	RtDatabase& Db() { return m_db; }
	RtVfs& Vfs() { return m_vfs; }
	RtScriptSystem* Scripts() { return m_scripts; }
	const RtSetup& Setup() const { return m_setup; }
	// Auto-choices for the human (SIMP-002): flags and the AUTO_CHOICE event queue.
	RtAutoChoice& AutoChoice() { return m_autochoice; }
	// Whether anybody can answer a blocker the human has. False (default): a human or UI sits in front of the game, so a choice
	// blocker the auto-choices leave alone (flag off) is WAITING_HUMAN. True: nobody will answer it (the setup's autoplay_human bot, a host without a UI), and
	// Pump returns STUCK with the blocker and the auto_choices flag that would resolve it. Unset, it follows the setup's autoplay_human.
	bool NoHumanInput() const { return m_noHumanInputSet ? m_noHumanInput : m_setup.autoplay_human != 0; }
	void SetNoHumanInput(bool value) { m_noHumanInput = value; m_noHumanInputSet = true; }

	// ---- save / load (task G4a, milestone M5; rt_save.cpp, format in rt_save.h) ------------------------------------------------
	struct LoadGameStatus
	{
		std::string step;      // last load step reached ("check", "lua", "runtime", "pregame", "game", "map", "teams", "players", "postload", "contexts", "start", "done")
		std::string error;
		int turn;              // game turn after the load
		int mismatches;        // DLL Read calls that did not consume their section exactly (logged as warnings)
		int luaErrors;         // Lua errors during the contexts and the game start
		double msRead, msStart;  // wall time of the DLL Reads + post-load tail, and of contexts + L8 + L9 (informational only)
		LoadGameStatus() : turn(-1), mismatches(0), luaErrors(0), msRead(0), msStart(0) {}
	};
	// The gamecore build a save must come from (header "build_id"): rt_save.h says what it is made of.
	std::string BuildId() const;
	// The setup JSON of the running game (gc_new_game's, or the loaded save's copy).
	const std::string& SetupJson() const { return m_setupJson; }
	// Why the game cannot be saved right now; empty = at a save point (the human is waiting, nothing is queued, no leader message is open).
	std::string SavePointProblem();
	// The running game as GCS1 bytes (rt_save.h). abiState is stored as the "abi" section. False + error when there is no game.
	bool SaveGame(const std::vector<uint8_t>& abiState, std::vector<uint8_t>& out, std::string& error);
	// Checks a GCS1 buffer without touching the game: magic, format version, length, checksum, section table, build id. False + the reason
	// (the ABI maps it to GC_ERR_BAD_DATA). Needs Loaded().
	bool CheckSave(const uint8_t* p, size_t n, std::string& error) const;
	// Loads a checked GCS1 buffer instead of NewGame, once per instance (a second NewGame / LoadGame is refused). The exe's load order
	// then contexts, DoGameStarted and SequenceGameInitComplete as after a new game. abiState gets the "abi" section.
	bool LoadGame(const gc_host& host, const uint8_t* p, size_t n, std::vector<uint8_t>& abiState, LoadGameStatus& status);
	// Whether a NewGame or LoadGame was attempted (the instance's one game is taken).
	bool GameTried() const { return m_newGameTried; }
	// B4 gc-replica (gc_resync): the running game is replaced by a GCS1 save in place, as the exe's MP resync does (Resync_LoadGameFromStream):
	// the Lua state and its contexts start fresh, the loopback is emptied, then the load path of LoadGame runs. False + status as LoadGame.
	bool ResyncGame(const gc_host& host, const uint8_t* p, size_t n, std::vector<uint8_t>& abiState, LoadGameStatus& status);

private:
	friend struct RtSaveAccess;  // rt_save.cpp (G4a): the runtime state a save carries
	RtSession(const RtSession&);
	RtSession& operator=(const RtSession&);

	bool ReadGameFile(const gc_host& host, const char* path, std::vector<unsigned char>& out);
	void Unload();
	bool LaunchToMap(NewGameStatus& st);
	void StartGame(NewGameStatus& st);
	void SummarizeMap(NewGameStatus& st);
	// After UpdateTestEndTurn: the host's canEndTurn for the human's active turn.
	void UpdateHostCanEndTurn();
	bool NetworkFrame();
	void PushDiploEvents(size_t first);
	// Why a turn does not end (STUCK reason): per turn-active player end-turn / diplomacy / busy / ready / turn-complete flags.
	std::string StuckText();

	CvDllGameContext* m_ctx;
	RtEngine m_engine;
	RtUserInterface m_ui;
	RtUiReplica m_uiRep;
	RtDiplo m_diplo;
	RtEventQueue m_events;
	RtDatabase m_db;
	RtVfs m_vfs;
	RtScriptSystem* m_scripts;
	RtSetup m_setup;
	RtAutoChoice m_autochoice;
	bool m_loaded;
	bool m_attached;
	bool m_inGame;
	bool m_newGameTried;
	uint32_t m_turnSlices;
	bool m_noHumanInput;
	bool m_noHumanInputSet;
	int m_humanPhaseTurn;      // the turn for which HUMAN_PHASE_STARTED was pushed (network game; not saved)
	std::string m_setupJson;   // the game's setup JSON (saved with the game)
	uint64_t m_dataHash;       // xxh64 of db/gameplay.db as gc_create read it (part of BuildId)
};

}  // namespace lekmod_portable
