//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstSession.h
//!  \brief     One civhost process = one DLL + data profile; games run inside it (CLI or --control stdio).
//!
//!		Boot: platform (run dir, log, sandbox), Lua arena, deterministic heap, exe exports, D1-D2, B1-B4, I1-I8,
//!		Lua master state. NewGame: G1-G9, L1-L6, L7 gameplay contexts, L8, L9 Events.SequenceGameInitComplete.
//!		LoadGame / SaveGame: checkpoints (HstCheckpoint.h). Run: turn loop + per-turn digests + checkpoints.
//!		Host bookkeeping that depends on paths or the control channel runs with the deterministic heap paused, so
//!		the CLI and the control channel produce the same digests.
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#pragma once
#ifndef HSTSESSION_H
#define HSTSESSION_H

#include "HstCommon.h"
#include "diag/HstDigest.h"
#include "diag/HstRngLog.h"
#include "game/HstGameRunner.h"
#include "platform/HstPlatform.h"

class HstArenaAlloc;
class HstDatabase;
class HstDllHost;
class HstCheckpointFile;
class HstEngineUtility;
class HstUserInterface;
class HstScriptSystem;
class HstScenario;
class HstVfs;

#define HST_HOST_VERSION "civhost 0.5 (M5)"

// Minimal checks (--selftest) shared by the session and HstMain.
extern int g_iHstCheckFailures;
#define HST_CHECK(cond, ...) \
	do { if(!(cond)) { ++g_iHstCheckFailures; HstLog("CHECK FAILED: " __VA_ARGS__); } else { HstLog("check ok: " __VA_ARGS__); } } while(0)

struct HstSessionOptions
{
	std::wstring wstrDll;
	std::wstring wstrGame;
	std::wstring wstrData;
	std::wstring wstrOut;
	std::wstring wstrLuaOverlay;	// --lua-overlay: Lua folder that takes precedence over the installed Lekmod DLC
	std::wstring wstrLuaDlc;		// --lua-dlc: game-relative or absolute Lekmod DLC folder (default: auto)
	std::wstring wstrLaunchDir;		// CWD at launch: relative paths in control commands resolve against it
	std::string strLanguage;
	int iMaxSlices;
	bool bStrict;
	bool bSelfTest;
	bool bTraceExceptions;
	bool bNoSandbox;
	bool bDllLogs;
	bool bNoDetHeap;
	bool bNoContexts;				// --no-lua-contexts: M3 behaviour (map scripts only)
	bool bCheckpointVerify;			// --ckpt-verify: after a load, compare the re-serialized state with the checkpoint
	bool bAllowWarm;				// --allow-warm: a second game in one process (new_game / load / import_save after a game ran) is allowed

	// Perturbation axes of the determinism lab. Defaults reproduce the canonical run exactly.
	int iHeapFill;					// --heap-fill <byte>: fresh det-heap allocations get this pattern instead of zero (-1 = zero)
	int iStackFill;					// --stack-fill <byte>: HstWipeStack writes this instead of zero (-1 = zero)
	unsigned int uiHeapShift;		// --heap-shift <bytes>: first det-heap block starts this far into the region
	unsigned int uiHeapJitter;		// --heap-jitter <seed>: pseudo-random per-request padding (0 = off)
	int iFpuPc;						// --fpu-pc 24|53|64: x87 precision control of the Update slices (0 = platform default, 53)
	int iFpuPcGlobal;				// --fpu-pc-global 24|53|64: same from process start (map generation, Lua, loads included)
	bool bFpuCheck;					// --fpu-check: log changes of the x87 control word (DLL / Lua)
	std::string strPerturbNote;		// where the axes came from (CLI / scenario env), for the log and run.json
	bool bTraceWrites;				// --trace-writes: state dumps also get <segment>.trace + sites.tsv (field attribution)
	HstRngLogOptions kRngLog;		// --rng-digest / --rng-log A-B (diag DLLs only)

	HstSessionOptions() : strLanguage("en_US"), iMaxSlices(20000), bStrict(false), bSelfTest(false), bTraceExceptions(false),
		bNoSandbox(false), bDllLogs(false), bNoDetHeap(false), bNoContexts(false), bCheckpointVerify(false), bAllowWarm(false),
		iHeapFill(-1), iStackFill(-1), uiHeapShift(0), uiHeapJitter(0), iFpuPc(0), iFpuPcGlobal(0), bFpuCheck(false), bTraceWrites(false) {}
	bool HasPerturbation() const { return iHeapFill >= 0 || iStackFill >= 0 || uiHeapShift || uiHeapJitter || iFpuPc || iFpuPcGlobal; }
};

struct HstTurnRecord
{
	HstTurnResult kTurn;
	bool bDigest;					// digest computed for this turn
	double dDigestMs;
	unsigned int uiDigestBytes;
	HstHash64 ui64Combined;
	std::string strCheckpoint;		// checkpoint written after this turn (UTF-8 path) or empty
};

struct HstRunRequest
{
	int iTurns;						// turns to run (ignored when iUntilTurn >= 0)
	int iUntilTurn;					// run until GetGameTurn() == iUntilTurn
	int iDigestEvery;				// 0 = no digests, 1 = every turn, N = turns divisible by N
	int iCheckpointEvery;			// 0 = off; else <out>\checkpoints\turn-NNNN.ckpt when turn % N == 0
	int iDumpStateAt;				// -1 = off
	bool bStopOnLuaError;
	bool bReloadCheckpoints;		// load every checkpoint back right after writing it (same process, same digest file):
									// the reference run for "fresh process --load" comparisons (the DLL's Read/Write is
									// not round-trip exact, so a load is part of the trajectory)

	HstRunRequest() : iTurns(0), iUntilTurn(-1), iDigestEvery(1), iCheckpointEvery(0), iDumpStateAt(-1), bStopOnLuaError(false),
		bReloadCheckpoints(false) {}
};

struct HstRunResult
{
	int iStartTurn;
	int iEndTurn;
	int iTurnsDone;
	std::string strReason;			// "done" | "stuck" | "lua_error" | "no_game"
	double dTotalMs;
	int iExitCode;					// 0, 7 (stuck)
};

//! One lockstep frame (peer mode, control "step"): deliver the inbox, run one Update slice.
struct HstStepRequest
{
	int iDigestEvery;				// digest when the turn changes (0 = never, N = turns divisible by N)
	int iDumpStateAt;				// dump the raw segments when the turn changes to this turn (-1 = off)
	int iGrantTurn;					// coordinator's turn-barrier grant for CanAdvanceTurn (-1 = none)
	HstStepRequest() : iDigestEvery(1), iDumpStateAt(-1), iGrantTurn(-1) {}
};

struct HstStepResult
{
	int iFrame;						// frames run in this game so far
	int iFromTurn;					// turn being played when the frame started
	int iTurn;						// turn after the frame
	bool bTurnChanged;
	HstTurnRecord kRecord;			// filled when bTurnChanged (slices = frames of that turn)
	double dMs;						// wall time of the frame
	bool bMasked;					// masked digest computed (HstDigest::SetMask patterns set)
	HstHash64 ui64Masked;			// combined masked digest (digest-masked.bin record)
	unsigned int uiMaskedBytes;		// bytes the mask dropped
	HstStepResult() : iFrame(0), iFromTurn(0), iTurn(0), bTurnChanged(false), dMs(0.0), bMasked(false), ui64Masked(0), uiMaskedBytes(0) {}
};

//! Receives per-turn records during Run (control channel: {"event":"turn"} / {"event":"alive"}).
class HstTurnListener
{
public:
	virtual ~HstTurnListener() {}
	virtual void OnTurn(const HstTurnRecord& kRecord) = 0;
	virtual void OnSlice() {}		// called before every Update(); cheap
};

class HstSession
{
public:
	HstSession();

	//! Run dir, log, sandbox, exception tracing. Returns an exit code (0 ok).
	int BootPlatform(const HstSessionOptions& kOpt);
	//! Arena, det heap, exports, DLL (bStopAfterLoad: M0 stage), database, Init, Lua master state, VFS.
	int BootEngine(bool bStopAfterLoad);

	//! G1-G9, L1-L6, map summary/digest, L7 contexts, L8, L9. kScenario.m_iActivePlayer is updated for observers.
	bool NewGame(HstScenario& kScenario, std::string& strError);
	//! Checkpoint -> running game (exe load order), then L7-L9 as after a real load. bContinueRun: same game id, turn
	//! records and digest file (in-process reload, HstRunRequest::bReloadCheckpoints).
	bool LoadGame(const std::wstring& wstrPath, std::string& strError, bool bContinueRun = false);
	//! Real .Civ5Save (v34.15) from `python -m civsim.saveio extract` (pregame.bin + body.bin in wstrDir), loaded like a
	//! checkpoint in the exe's order. Human slots (SS_TAKEN) become AI (SS_COMPUTER) unless bKeepHumans: nobody answers
	//! the turn handshake of a human in a headless run.
	bool ImportSave(const std::wstring& wstrDir, bool bKeepHumans, std::string& strError);
	//! Checkpoint of the current state (between turns).
	bool SaveGame(const std::wstring& wstrPath, unsigned int& uiBytes, HstHash64& ui64Hash, std::string& strError);
	//! Turn loop; appends to the game's digest file.
	HstRunResult Run(const HstRunRequest& kReq, HstTurnListener* pkListener);
	//! Lockstep peer frame: akInbox in coordinator order, then one Update slice; on a turn change
	//! the turn record and digest are written as Run does. Returns false (strError) without a game.
	bool Step(const std::vector<struct HstNetMessage>& akInbox, const HstStepRequest& kReq, HstStepResult& kResult, std::string& strError);
	//! Lockstep bot: a Lua thread "HarnessBot" running strInit then the file, on this
	//! peer only (the local UI of a human). BotTick calls HarnessBotTick(frame, mode) with the deterministic heap paused and
	//! returns its string result (a short action log). mode "play" or "ui".
	bool BotLoad(const std::wstring& wstrFile, const std::string& strInit, std::string& strError);
	std::string BotTick(int iFrame, const char* szMode);
	bool HasBot() const { return m_pkBotThread != NULL; }
	//! Digest of the current state: "turn" (combined hash, text), "subsystem" (one digest.bin record), "full" (segments).
	bool WriteDigest(const std::string& strLevel, const std::wstring& wstrPath, HstHash64& ui64Combined, std::string& strError);
	//! Drop contexts and the save-data connection; the next NewGame/LoadGame starts from a clean host state.
	void EndGame();
	//! <run>\run.json for the current game.
	void WriteRunJson(const std::string& strStatus);
	//! <run>\events-registered.json and api-calls.tsv.
	void WriteReports();

	bool InGame() const { return m_bInGame; }
	int GetGameId() const { return m_iGameId; }
	int GetGameTurn();
	int GetNumPlots();
	int GetNumAlivePlayers();
	HstHash64 ComputeCombinedDigest();
	unsigned __int64 GetMapDigest() const { return m_ui64MapDigest; }
	double GetLaunchMs() const { return m_dLaunchMs; }
	const std::wstring& GetDigestPath() const { return m_wstrDigestPath; }
	const HstSessionOptions& GetOptions() const { return m_kOpt; }
	//! A path from the control channel: absolute, or relative to the launch directory (the CWD is the run dir).
	std::wstring ResolveUserPath(const std::string& strUtf8) const;
	ICvGameContext1* GetContext() const;
	HstScriptSystem* GetScriptSystem() const { return m_pkScriptSystem; }
	HstEngineUtility* GetEngine() const { return m_pkEngine; }
	HstDatabase* GetDatabase() const { return m_pkDb; }
	std::string GetDllVersion() const;
	//! xxh64 of the DLL file / of the gameplay DB (profile hash for "hello").
	HstHash64 GetDllHash() const { return m_ui64DllHash; }
	HstHash64 GetDataHash() const { return m_ui64DataHash; }
	const std::vector<std::string>& GetContextNames() const { return m_astrContextNames; }

protected:
	// Bodies of the public entry points; the public methods run them at the fixed stack position (HstRunAtFixedStack).
	int BootEngineImpl(bool bStopAfterLoad);
	bool NewGameImpl(HstScenario& kScenario, std::string& strError);
	//! pkImport: an in-memory file built by ImportSaveImpl instead of reading wstrPath (body stream mode).
	bool LoadGameImpl(const std::wstring& wstrPath, std::string& strError, bool bContinueRun, const HstCheckpointFile* pkImport = NULL,
		bool bHumansToAI = false);
	bool ImportSaveImpl(const std::wstring& wstrDir, bool bKeepHumans, std::string& strError);
	bool SaveGameImpl(const std::wstring& wstrPath, unsigned int& uiBytes, HstHash64& ui64Hash, std::string& strError);
	HstRunResult RunImpl(const HstRunRequest& kReq, HstTurnListener* pkListener);
	bool StepImpl(const std::vector<struct HstNetMessage>& akInbox, const HstStepRequest& kReq, HstStepResult& kResult, std::string& strError);
	std::string BotTickImpl(int iFrame, const char* szMode);
	void EndGameImpl();
	//! Warm-start guard: true (and strError set) when this process already hosted a game and --allow-warm is not given. The
	//! gamecore DLL is not reloaded between games, so game N+1 starts from what game N left in it (determinism class C8).
	bool RefuseWarmStart(bool bContinueRun, std::string& strError) const;
	friend struct HstSessionThunks;

	void SetupVfs();
	//! L7: contexts from the scenario list or the InGame.lua manifest; then L8 + L9.
	void StartGame(const HstScenario* pkScenario, bool bLoaded);
	bool ReadContextManifest(std::vector<std::string>& astrNames, std::string& strSource);
	bool OpenDigestFile();
	std::wstring RunPath(const std::wstring& wstrName);
	bool WriteCheckpointForTurn(int iTurn, std::string& strPath);

	HstSessionOptions m_kOpt;
	HstArenaAlloc* m_pkArena;
	HstDllHost* m_pkHost;
	HstDatabase* m_pkDb;
	HstEngineUtility* m_pkEngine;
	HstUserInterface* m_pkUI;
	HstScriptSystem* m_pkScriptSystem;
	HstVfs* m_pkMapVfs;
	HstVfs* m_pkContextVfs;
	HstGameRunner* m_pkRunner;
	HstDigest* m_pkDigest;
	std::wstring m_wstrDigestPath;
	std::vector<HstTurnRecord> m_akTurns;
	std::vector<std::string> m_astrContextNames;
	std::string m_strScenarioName;
	std::string m_strContextSource;
	std::string m_strPendingReload;		// checkpoint RunImpl wants reloaded (bReloadCheckpoints)
	std::string m_strImportGameName;	// archive.game_name of the save being imported (log / error text only)
	bool m_bInGame;
	int m_iGameId;
	int m_iStartTurn;
	unsigned __int64 m_ui64MapDigest;
	double m_dLaunchMs;
	unsigned int m_uiFpuStart;
	HstHash64 m_ui64DllHash;
	HstHash64 m_ui64DataHash;
	unsigned int m_uiBootVirtualMs;		// host clock / GUID counter after boot: every NewGame restarts from these
	unsigned int m_uiBootGuidCounter;
	// Peer mode (Step): the turn in progress, like RunTurn's locals.
	ICvGame1* m_pkStepGame;				// game wrapper held for the whole turn (RunTurn holds one per turn too)
	HstDigest* m_pkMaskDigestFile;		// digest-masked.bin writer (peer mode with a digest mask)
	HstTurnResult m_kStepTurn;
	int m_iFrame;
	struct lua_State* m_pkBotThread;			// lockstep bot thread (BotLoad), NULL without a bot
};

double HstNowMs();
std::string HstJsonEscape(const std::string& str);

#endif // HSTSESSION_H
