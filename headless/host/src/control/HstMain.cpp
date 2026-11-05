//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstMain.cpp
//!  \brief     civhost.exe entry point: CLI, selftests, exit codes; --control stdio hands over to HstControl.
//!
//!		Exit codes: 0 ok, 1 setup/usage error, 2 check failed, 3 crash (SEH, minidump written),
//!		4 sim engine method without implementation, 5 unexpected engine call under --strict,
//!		6 Lua error under --strict, 7 turn stuck.
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#include "HstCommon.h"
#include "HstApi.h"
#include "control/HstSession.h"
#include "control/HstControl.h"
#include "platform/HstPlatform.h"
#include "platform/HstDetHeap.h"
#include "data/HstDatabase.h"
#include "engine/HstExeExports.h"
#include "script/HstVfs.h"
#include "script/HstScriptSystem.h"
#include "game/HstScenario.h"
#include "game/HstJson.h"

#include <io.h>
#include <fcntl.h>

struct HstCliOptions
{
	HstSessionOptions kSession;
	std::wstring wstrScenario;
	std::wstring wstrLoad;			// --load <checkpoint>
	std::wstring wstrImport;		// --import-save <dir from `python -m civsim.saveio extract`>
	bool bImportKeepHumans;			// --import-keep-humans
	std::string strStage;			// "load" (M0) | "init" (M1) | "map" (M2) | "turns" (M3+)
	std::string strActivePlayer;	// --active-player <slot|observer> (overrides the scenario)
	std::string strGameType;		// --game-type single|network
	std::string strGameOptionsPreset;	// --game-options-preset db|lekmod-mp (overrides the scenario's game_options_preset)
	std::string strControl;			// --control stdio
	bool bHelp;						// --help printed the usage text
	HstRunRequest kRun;

	HstCliOptions() : bImportKeepHumans(false), strStage("init"), bHelp(false) {}
};

static void HstUsage(bool bFull = false)
{
	// --help goes to stdout (stderr output of the argument parser is lost in this process for a reason not chased yet).
	FILE* pkOut = bFull ? stdout : stderr;
	fprintf(pkOut, "%s\n"
		"usage: civhost.exe [--dll <gamecore dll>] [--game <Civ5 root>] [--data <dir with Civ5DebugDatabase.db,\n"
		"                   Localization-Merged.db>] [--out <run dir>] [--lang en_US] [--stage load|init|map|turns]\n"
		"                   [--scenario <json> | --load <checkpoint>] [--turns N] [--until-turn T] [--digest-every N]\n"
		"                   [--checkpoint-every N] [--dump-state-at T] [--max-slices 20000]\n"
		"                   [--lua-overlay <dir>] [--lua-dlc <dir>] [--no-lua-contexts]\n"
		"                   [--active-player <slot|observer>] [--game-type single|network] [--control stdio]\n"
		"                   [--game-options-preset db|lekmod-mp] [--allow-warm]\n"
		"                   [--strict] [--selftest] [--trace-exceptions] [--no-sandbox] [--dll-logs] [--no-det-heap]\n"
		"                   [--heap-fill <byte>] [--stack-fill <byte>] [--heap-shift <bytes>] [--heap-jitter <seed>]\n"
		"                   [--fpu-pc 24|53|64] [--fpu-pc-global 24|53|64] [--fpu-check [0|1]] [--trace-writes]\n"
		"                   [--rng-digest] [--rng-log <turnA-turnB>] [--import-save <dir>] [--import-keep-humans]\n"
		"exit: 0 ok, 1 setup, 2 check failed, 3 crash, 4 sim method missing, 5 unexpected (--strict),\n"
		"      6 Lua error (--strict), 7 turn stuck (no turn advance within --max-slices)\n", HST_HOST_VERSION);
	if(!bFull)
		return;
	fprintf(pkOut,
		"\nperturbation axes (determinism lab, civsim.detlab perturb --axis heap_fill=0x00,0xFF ...). Defaults are the canonical\n"
		"run; every axis changes exactly one source of entropy that must not influence the simulation. Numbers: decimal or 0x hex.\n"
		"The same keys (underscores) are read from the scenario's \"env\" object when the flag is not given.\n"
		"  --heap-fill <byte>    pattern of fresh deterministic-heap allocations instead of zero (HEAP_ZERO_MEMORY / calloc stay\n"
		"                        zero). Exposes reads of uninitialized heap memory (class C2).\n"
		"  --stack-fill <byte>   pattern HstWipeStack writes (256 KB below the stack pointer before every DLL entry / Update slice)\n"
		"                        instead of zero. Exposes reads of uninitialized stack memory (C2).\n"
		"  --heap-shift <bytes>  the first deterministic-heap block starts <bytes> (rounded up to 8) into the region: all heap\n"
		"                        addresses move by the same amount. Exposes addresses leaking into state (C3).\n"
		"  --heap-jitter <seed>  pseudo-random 0..24 byte padding per allocation request: size classes, free-list reuse and the\n"
		"                        relative order of live blocks change. Exposes pointer-ordered containers (C3). 0 = off.\n"
		"  --fpu-pc 24|53|64     x87 precision control set before every Update slice (the turn loop) and restored to the default (53)\n"
		"                        after it: the sensitivity of the simulation itself. Default: 53 bit (CRT default; the real\n"
		"                        game-core thread starts there). Axis for C4 / fidelity risk D10 (DX9 vs DX11 exes).\n"
		"  --fpu-pc-global 24|53|64  same, but from process start on: boot, map generation (Lua map script), gameplay contexts\n"
		"                        and loads run at that precision too (--fpu-pc then still wins inside the Update slices).\n"
		"  --fpu-check [0|1]     log every change of the x87 control word seen after an Update slice, a session entry or a Lua\n"
		"                        call (DLL / Lua modifying it); the count goes to run.json (perturbation.fpu_changes).\n"
		"  --trace-writes        (with --dump-state-at) record which DLL code wrote each byte range of every segment: <segment>.trace\n"
		"                        and sites.tsv (return address -> function, file, line from the DLL's PDB, which must sit next\n"
		"                        to the DLL). Turns a differing byte offset into the `kStream << m_xxx` line. Digests unchanged.\n"
		"\nRNG call log (determinism lab; needs a diag / optimized-diag DLL, which exports HeadlessDiagInstall - for other DLLs the\n"
		"flags log one line and record nothing; digest.bin is unchanged either way):\n"
		"  --rng-digest          <run>\\rng-digest.bin: per phase (pregame, then every turn) and RNG kind (jon, map, async, other)\n"
		"                        the call count, reseed count, first/last seed and an address-independent hash of the\n"
		"                        (seed, range, result) stream. Two runs of one scenario must agree turn by turn.\n"
		"  --rng-log <A-B>       <run>\\rng.log.bin: every RNG call of the turns A..B inclusive (also: A, pre, pre-B; pre =\n"
		"                        game creation / checkpoint load), 20 bytes per call, plus <run>\\rng.sites.tsv: per call site the\n"
		"                        pszLog text and the caller resolved to function+offset, file:line through the DLL's PDB.\n"
		"                        civsim.detlab bisect uses it to name the first differing call.\n"
		"\nGame options and warm starts:\n"
		"  --game-options-preset <name>  db (default) | lekmod-mp. db: only the scenario's own game_options are set and every other\n"
		"                        option answers with the GameOptions table default, as in the real game (its saves list 5-26 options).\n"
		"                        lekmod-mp: also sets the option list of a typical Lekmod multiplayer lobby (DYNAMIC_TURNS 0,\n"
		"                        SIMULTANEOUS_TURNS 1, QUICK_COMBAT 1, QUICK_MOVEMENT 1, END_TURN_TIMER_ENABLED 0, AI_OPTIONS 1,\n"
		"                        AI_GIMP_ALWAYS_WHITE_PEACE 0, ADDITIONAL 1, TWEAKED_SCORING 1). Scenario key: game_options_preset.\n"
		"                        Combine it with --game-type network: a single-player game drops the MP turn modes in CvGame::init.\n"
		"  --allow-warm          allow a second game in one process (control channel: new_game / load / import_save after a game\n"
		"                        ran). Refused by default: the gamecore DLL is not reloaded between games, so game 2 starts from what\n"
		"                        game 1 left in the DLL and differs from a fresh-process game (determinism class C8).\n"
		"\nSave import (instead of --scenario / --load):\n"
		"  --import-save <dir>   load a real v34.15 .Civ5Save: <dir> is the output of `python -m civsim.saveio extract <save> --out <dir>`\n"
		"                        (pregame.bin + body.bin). Loaded like a checkpoint in the exe's order (pregame, game, map, 64 teams,\n"
		"                        64 players, supporting data); the log reports how many body bytes the DLL Reads consumed.\n"
		"                        Human slots (SS_TAKEN) become AI (SS_COMPUTER) unless --import-keep-humans (then the turn handshake\n"
		"                        waits for a human nobody plays). --turns N continues from the saved turn.\n"
		"  --help                this text (exit 0)\n");
}

//! Number from the command line: decimal or 0x hex. false when not a clean integer.
static bool HstParseNumber(const wchar_t* wsz, long& lOut)
{
	wchar_t* pwszEnd = NULL;
	lOut = wcstol(wsz, &pwszEnd, 0);
	return pwszEnd && pwszEnd != wsz && *pwszEnd == 0;
}

//! Scenario "env" values: number, bool or string ("0xFF").
static bool HstEnvNumber(const HstJson* pkValue, long& lOut)
{
	if(!pkValue)
		return false;
	if(pkValue->GetType() == HstJson::JSON_NUMBER)
	{
		lOut = (long)pkValue->AsNumber();
		return true;
	}
	if(pkValue->GetType() == HstJson::JSON_BOOL)
	{
		lOut = pkValue->AsBool() ? 1 : 0;
		return true;
	}
	if(pkValue->GetType() == HstJson::JSON_STRING)
		return HstParseNumber(HstFromUtf8(pkValue->AsString()).c_str(), lOut);
	return false;
}

//! Fills the axes the command line left open from the scenario's "env" object (simrunner passes both; flags win).
static void HstApplyScenarioEnv(const std::wstring& wstrScenario, HstCliOptions& kOpt)
{
	HstJson kRoot;
	std::string strError;
	if(!HstJson::ParseFile(wstrScenario, kRoot, strError))
		return;
	const HstJson* pkEnv = kRoot.Get("env");
	if(!pkEnv || pkEnv->GetType() != HstJson::JSON_OBJECT)
		return;
	HstSessionOptions& kS = kOpt.kSession;
	long l = 0;
	std::string strUsed;
	if(kS.iHeapFill < 0 && HstEnvNumber(pkEnv->Get("heap_fill"), l) && l >= 0 && l <= 255)
	{
		kS.iHeapFill = (int)l;
		strUsed += " heap_fill";
	}
	if(kS.iStackFill < 0 && HstEnvNumber(pkEnv->Get("stack_fill"), l) && l >= 0 && l <= 255)
	{
		kS.iStackFill = (int)l;
		strUsed += " stack_fill";
	}
	if(!kS.uiHeapShift && HstEnvNumber(pkEnv->Get("heap_shift"), l) && l > 0)
	{
		kS.uiHeapShift = (unsigned int)l;
		strUsed += " heap_shift";
	}
	if(!kS.uiHeapJitter && HstEnvNumber(pkEnv->Get("heap_jitter"), l) && l > 0)
	{
		kS.uiHeapJitter = (unsigned int)l;
		strUsed += " heap_jitter";
	}
	if(!kS.iFpuPc && HstEnvNumber(pkEnv->Get("fpu_pc"), l) && (l == 24 || l == 53 || l == 64))
	{
		kS.iFpuPc = (int)l;
		strUsed += " fpu_pc";
	}
	if(!kS.iFpuPcGlobal && HstEnvNumber(pkEnv->Get("fpu_pc_global"), l) && (l == 24 || l == 53 || l == 64))
	{
		kS.iFpuPcGlobal = (int)l;
		strUsed += " fpu_pc_global";
	}
	if(!kS.bFpuCheck && HstEnvNumber(pkEnv->Get("fpu_check"), l) && l != 0)
	{
		kS.bFpuCheck = true;
		strUsed += " fpu_check";
	}
	if(!strUsed.empty())
		kS.strPerturbNote += (kS.strPerturbNote.empty() ? "" : "; ") + std::string("scenario env:") + strUsed;
}

static bool HstParseArgs(int argc, wchar_t** argv, HstCliOptions& kOpt)
{
	HstSessionOptions& kS = kOpt.kSession;
	// Defaults relative to headless\out\host\Release\civhost.exe.
	std::wstring wstrRepo = HstFullPath(HstJoinPath(HstExeDir(), L"..\\..\\..\\.."));
	kS.wstrDll = HstJoinPath(wstrRepo, L"headless\\out\\dll\\golden\\CvGameCore_Expansion2.dll");
	kS.wstrData = HstJoinPath(wstrRepo, L"headless\\out\\oracle\\cache-2026-10-17");
	kS.wstrGame = L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Sid Meier's Civilization V";
	wchar_t wszEnv[MAX_PATH];
	if(GetEnvironmentVariableW(L"CIV5_GAME_DIR", wszEnv, MAX_PATH))
		kS.wstrGame = wszEnv;
	SYSTEMTIME kTime;
	GetLocalTime(&kTime);
	wchar_t wszRun[64];
	// Fixed width: keeps run dir names uniform (the DLL itself only sees ".\" as its cache folder).
	swprintf_s(wszRun, L"%04d%02d%02d-%02d%02d%02d-%06u", kTime.wYear, kTime.wMonth, kTime.wDay, kTime.wHour, kTime.wMinute, kTime.wSecond,
		(unsigned int)GetCurrentProcessId() % 1000000);
	kS.wstrOut = HstJoinPath(wstrRepo, std::wstring(L"headless\\out\\runs\\") + wszRun);
	wchar_t wszCwd[MAX_PATH];
	if(GetCurrentDirectoryW(MAX_PATH, wszCwd))
		kS.wstrLaunchDir = wszCwd;
	bool bTurnsGiven = false;
	std::string strCliAxes;

	for(int i = 1; i < argc; ++i)
	{
		std::wstring wstrArg = argv[i];
		bool bHasValue = i + 1 < argc;
		if(wstrArg == L"--dll" && bHasValue)
			kS.wstrDll = argv[++i];
		else if(wstrArg == L"--game" && bHasValue)
			kS.wstrGame = argv[++i];
		else if(wstrArg == L"--data" && bHasValue)
			kS.wstrData = argv[++i];
		else if(wstrArg == L"--out" && bHasValue)
			kS.wstrOut = argv[++i];
		else if(wstrArg == L"--lang" && bHasValue)
			kS.strLanguage = HstToUtf8(argv[++i]);
		else if(wstrArg == L"--scenario" && bHasValue)
			kOpt.wstrScenario = argv[++i];
		else if(wstrArg == L"--load" && bHasValue)
			kOpt.wstrLoad = argv[++i];
		else if(wstrArg == L"--import-save" && bHasValue)
			kOpt.wstrImport = argv[++i];
		else if(wstrArg == L"--import-keep-humans")
			kOpt.bImportKeepHumans = true;
		else if(wstrArg == L"--stage" && bHasValue)
			kOpt.strStage = HstToUtf8(argv[++i]);
		else if(wstrArg == L"--turns" && bHasValue)
		{
			kOpt.kRun.iTurns = _wtoi(argv[++i]);
			bTurnsGiven = true;
		}
		else if(wstrArg == L"--until-turn" && bHasValue)
		{
			kOpt.kRun.iUntilTurn = _wtoi(argv[++i]);
			bTurnsGiven = true;
		}
		else if(wstrArg == L"--digest-every" && bHasValue)
			kOpt.kRun.iDigestEvery = _wtoi(argv[++i]);
		else if(wstrArg == L"--checkpoint-every" && bHasValue)
			kOpt.kRun.iCheckpointEvery = _wtoi(argv[++i]);
		else if(wstrArg == L"--dump-state-at" && bHasValue)
			kOpt.kRun.iDumpStateAt = _wtoi(argv[++i]);
		else if(wstrArg == L"--max-slices" && bHasValue)
			kS.iMaxSlices = _wtoi(argv[++i]);
		else if(wstrArg == L"--lua-overlay" && bHasValue)
			kS.wstrLuaOverlay = argv[++i];
		else if(wstrArg == L"--lua-dlc" && bHasValue)
			kS.wstrLuaDlc = argv[++i];
		else if(wstrArg == L"--no-lua-contexts")
			kS.bNoContexts = true;
		else if(wstrArg == L"--ckpt-verify")
			kS.bCheckpointVerify = true;
		else if(wstrArg == L"--reload-checkpoints")
			kOpt.kRun.bReloadCheckpoints = true;
		else if(wstrArg == L"--active-player" && bHasValue)
			kOpt.strActivePlayer = HstToUtf8(argv[++i]);
		else if(wstrArg == L"--game-type" && bHasValue)
			kOpt.strGameType = HstToUtf8(argv[++i]);
		else if(wstrArg == L"--game-options-preset" && bHasValue)
			kOpt.strGameOptionsPreset = HstToUtf8(argv[++i]);
		else if(wstrArg == L"--allow-warm")
			kS.bAllowWarm = true;
		else if(wstrArg == L"--control" && bHasValue)
			kOpt.strControl = HstToUtf8(argv[++i]);
		else if(wstrArg == L"--strict")
			kS.bStrict = true;
		else if(wstrArg == L"--selftest")
			kS.bSelfTest = true;
		else if(wstrArg == L"--trace-exceptions")
			kS.bTraceExceptions = true;
		else if(wstrArg == L"--no-sandbox")
			kS.bNoSandbox = true;
		else if(wstrArg == L"--dll-logs")
			kS.bDllLogs = true;
		else if(wstrArg == L"--no-det-heap")
			kS.bNoDetHeap = true;
		else if(wstrArg == L"--help" || wstrArg == L"-h" || wstrArg == L"/?")
		{
			kOpt.bHelp = true;
			HstUsage(true);
			return true;
		}
		else if((wstrArg == L"--heap-fill" || wstrArg == L"--stack-fill") && bHasValue)
		{
			long lByte = 0;
			if(!HstParseNumber(argv[i + 1], lByte) || lByte < 0 || lByte > 255)
			{
				fprintf(stderr, "civhost: %ls needs a byte value (0..255 or 0x00..0xFF), got '%ls'\n", wstrArg.c_str(), argv[i + 1]);
				return false;
			}
			++i;
			(wstrArg == L"--heap-fill" ? kS.iHeapFill : kS.iStackFill) = (int)lByte;
			strCliAxes += (wstrArg == L"--heap-fill" ? " heap-fill" : " stack-fill");
		}
		else if((wstrArg == L"--heap-shift" || wstrArg == L"--heap-jitter") && bHasValue)
		{
			long lValue = 0;
			if(!HstParseNumber(argv[i + 1], lValue) || lValue < 0)
			{
				fprintf(stderr, "civhost: %ls needs a non-negative number, got '%ls'\n", wstrArg.c_str(), argv[i + 1]);
				return false;
			}
			++i;
			(wstrArg == L"--heap-shift" ? kS.uiHeapShift : kS.uiHeapJitter) = (unsigned int)lValue;
			strCliAxes += (wstrArg == L"--heap-shift" ? " heap-shift" : " heap-jitter");
		}
		else if((wstrArg == L"--fpu-pc" || wstrArg == L"--fpu-pc-global") && bHasValue)
		{
			long lBits = 0;
			if(!HstParseNumber(argv[i + 1], lBits) || (lBits != 24 && lBits != 53 && lBits != 64))
			{
				fprintf(stderr, "civhost: %ls needs 24, 53 or 64, got '%ls'\n", wstrArg.c_str(), argv[i + 1]);
				return false;
			}
			++i;
			(wstrArg == L"--fpu-pc" ? kS.iFpuPc : kS.iFpuPcGlobal) = (int)lBits;
			strCliAxes += (wstrArg == L"--fpu-pc" ? " fpu-pc" : " fpu-pc-global");
		}
		else if(wstrArg == L"--trace-writes")
			kS.bTraceWrites = true;
		else if(wstrArg == L"--rng-digest")
			kS.kRngLog.bDigest = true;
		else if(wstrArg == L"--rng-log" && bHasValue)
		{
			int iFrom = 0, iTo = 0;
			if(!HstRngLog::ParseWindow(HstToUtf8(argv[i + 1]), iFrom, iTo))
			{
				fprintf(stderr, "civhost: --rng-log needs <turnA-turnB>, <turn>, pre or pre-<turn>, got '%ls'\n", argv[i + 1]);
				return false;
			}
			++i;
			kS.kRngLog.bLog = true;
			kS.kRngLog.iLogFrom = iFrom;
			kS.kRngLog.iLogTo = iTo;
		}
		else if(wstrArg == L"--fpu-check")
		{
			kS.bFpuCheck = true;
			if(bHasValue && argv[i + 1][0] != L'-')	// optional value: 0/1/true/false (simrunner passes env booleans that way)
			{
				const std::wstring wstrValue = argv[++i];
				kS.bFpuCheck = !(wstrValue == L"0" || _wcsicmp(wstrValue.c_str(), L"false") == 0);
			}
		}
		else
		{
			HstUsage();
			return false;
		}
	}
	if(!strCliAxes.empty())
		kS.strPerturbNote = "cli:" + strCliAxes;
	// Resolve relative paths against the launch CWD before the run dir becomes the CWD.
	kS.wstrDll = HstFullPath(kS.wstrDll);
	kS.wstrGame = HstFullPath(kS.wstrGame);
	kS.wstrData = HstFullPath(kS.wstrData);
	kS.wstrOut = HstFullPath(kS.wstrOut);
	if(!kS.wstrLuaOverlay.empty())
		kS.wstrLuaOverlay = HstFullPath(kS.wstrLuaOverlay);
	if(!kS.wstrLuaDlc.empty() && kS.wstrLuaDlc.find(L':') != std::wstring::npos)
		kS.wstrLuaDlc = HstFullPath(kS.wstrLuaDlc);
	if(!kOpt.wstrLoad.empty())
		kOpt.wstrLoad = HstFullPath(kOpt.wstrLoad);
	if(!kOpt.wstrImport.empty())
		kOpt.wstrImport = HstFullPath(kOpt.wstrImport);
	if(!kOpt.wstrScenario.empty() || !kOpt.wstrLoad.empty() || !kOpt.wstrImport.empty())
	{
		if(!kOpt.wstrScenario.empty())
		{
			kOpt.wstrScenario = HstFullPath(kOpt.wstrScenario);
			HstApplyScenarioEnv(kOpt.wstrScenario, kOpt);
		}
		if(kOpt.strStage == "init")
			kOpt.strStage = bTurnsGiven ? "turns" : "map";
	}
	if(!kOpt.strControl.empty() && kOpt.strControl != "stdio")
	{
		fprintf(stderr, "civhost: --control supports only 'stdio'\n");
		return false;
	}
	if(!kOpt.strGameOptionsPreset.empty() && (!kOpt.strControl.empty() || !kOpt.wstrLoad.empty() || !kOpt.wstrImport.empty()))
	{
		fprintf(stderr, "civhost: --game-options-preset applies to --scenario runs (control channel: the \"game_options_preset\" key of new_game; a save carries its own options)\n");
		return false;
	}
	if(kOpt.strStage == "turns" && kOpt.wstrScenario.empty() && kOpt.wstrLoad.empty() && kOpt.wstrImport.empty())
	{
		fprintf(stderr, "civhost: --turns needs --scenario, --load or --import-save\n");
		return false;
	}
	if(kS.iMaxSlices <= 0)
		kS.iMaxSlices = 20000;
	return true;
}

//! Host-only unit checks (no DLL).
static void HstSelfTestUnits()
{
	GUID kGuid;
	HST_CHECK(HstDatabase::ParseGuid("293C1EE3117644F6AC1F59663826DE74", kGuid) && kGuid.Data1 == 0x293C1EE3 && kGuid.Data2 == 0x1176 &&
		kGuid.Data3 == 0x44F6 && kGuid.Data4[0] == 0xAC && kGuid.Data4[7] == 0x74, "ParseGuid");
	HST_CHECK(!HstDatabase::ParseGuid("1234", kGuid), "ParseGuid rejects short input");
	HST_CHECK(HstZero<int>() == 0 && HstZero<bool>() == false && HstZero<void*>() == NULL && HstZero<CvString>().empty() &&
		HstZero<InterfaceModeTypes>() == (InterfaceModeTypes)0, "HstZero");
	std::wstring wstrInside = HstJoinPath(HstGetRunDir(), L"sub\\x.db");
	std::wstring wstrGame = HstJoinPath(HstGetGameDir(), L"x.db");
	HST_CHECK(HstSandboxIsWriteAllowed(wstrInside.c_str()), "sandbox allows writes in run dir");
	HST_CHECK(HstSandboxIsWriteAllowed(L"relative.txt"), "sandbox allows relative (CWD = run dir) writes");
	HST_CHECK(!HstSandboxIsWriteAllowed(wstrGame.c_str()), "sandbox denies writes to the game dir");
	HST_CHECK(!HstSandboxIsWriteAllowed((HstGetRunDir() + L"-evil\\x").c_str()), "sandbox denies run-dir prefix siblings");
	HST_CHECK(!HstSandboxIsWriteAllowed(L"..\\escape.txt"), "sandbox denies ..");
	HST_CHECK(g_akHstApiInfo[kHstApi_EngineUtility_GetCacheFolderPath].szClass[0] == 's', "api table: GetCacheFolderPath is sim");
	// XXH64 reference vectors (xxHash test suite): "" and "abc" with seed 0.
	HST_CHECK(HstXxh64("", 0) == 0xEF46DB3751D8E999ULL, "xxh64('') = ef46db3751d8e999");
	HST_CHECK(HstXxh64("abc", 3) == 0x44BC2CF5AD770999ULL, "xxh64('abc') = 44bc2cf5ad770999 (got %016I64x)", HstXxh64("abc", 3));
	const char* szLong = "0123456789abcdef0123456789abcdef0123456789";	// 42 bytes: stripe loop + 8/4/1-byte tails
	HST_CHECK(HstXxh64(szLong, 42) != HstXxh64(szLong, 41) && HstXxh64(szLong, 42, 1) != HstXxh64(szLong, 42), "xxh64 length/seed sensitivity");
	std::string strReq;
	HST_CHECK(HstControlSelfTest(strReq), "control channel: request parsing / response formatting (%s)", strReq.c_str());
}

//! Script-system checks (design 6.3 minimal tests 1-3): Call* semantics with 0/2 listeners, args pool, include, table.fill,
//! save data, events registry. Run after the turns: their Lua/C allocations would otherwise shift the deterministic heaps.
static void HstSelfTestScript(HstScriptSystem* pkSS, bool bInGame)
{
	lua_State* L = pkSS->GetMasterState();
	bool bValue = true;
	HST_CHECK(!pkSS->CallHook("HstSelfTestNone", NULL, bValue) && !bValue, "CallHook, 0 listeners: returns false, value assigned false");
	bValue = false;
	HST_CHECK(!pkSS->CallTestAll("HstSelfTestNone", NULL, bValue) && bValue, "CallTestAll, 0 listeners: returns false, value true");
	pkSS->RunString(L, "GameEvents.HstSelfTest.Add(function(a, b) return a + b end) GameEvents.HstSelfTest.Add(function(a, b) return a * b end)", "=selftest");
	ICvEngineScriptSystemArgs1* pkArgs = pkSS->CreateArgs();
	pkArgs->Push(3);
	pkArgs->Push(4);
	int iSum = 0;
	HST_CHECK(pkSS->CallAccumulator("HstSelfTest", pkArgs, iSum) && iSum == 19, "CallAccumulator, 2 listeners: 3+4 + 3*4 = 19 (got %d)", iSum);
	pkSS->DestroyArgs(pkArgs);
	ICvEngineScriptSystemArgs1* pkArgs2 = pkSS->CreateArgs();
	HST_CHECK(pkArgs2 == pkArgs && pkArgs2->Count() == 0, "args pool reuses a reset instance");
	pkSS->DestroyArgs(pkArgs2);
	pkSS->RunString(L, "GameEvents.HstSelfTestAll.Add(function() return true end) GameEvents.HstSelfTestAll.Add(function() end)", "=selftest");
	bValue = true;
	HST_CHECK(pkSS->CallTestAll("HstSelfTestAll", NULL, bValue) && !bValue, "CallTestAll, true + nil: false");
	bValue = false;
	HST_CHECK(pkSS->CallTestAny("HstSelfTestAll", NULL, bValue) && bValue, "CallTestAny, true + nil: true");
	bValue = false;
	HST_CHECK(pkSS->CallHook("HstSelfTestAll", NULL, bValue) && bValue, "CallHook, true + nil: true");
	HstVfs* pkVfs = pkSS->GetVfs();
	std::wstring wstrPath;
	if(pkVfs)
		HST_CHECK(pkVfs->ResolveInclude("FLuaVector.lua", wstrPath) && wstrPath.find(L"Gameplay") != std::wstring::npos,
			"include('FLuaVector') resolves through the VFS");
	const int iErrors = pkSS->GetErrorCount();
	pkSS->RunString(L, "local a = table.fill(0, 5) assert(#a == 5 and a[5] == 0 and a[0] == nil) local t = {} table.fill(t, false, 3) assert(t[3] == false)"
		" assert(math.clamp(5, 0, 3) == 3) assert(table.count({ x = 1, 2 }) == 2) assert(GameInfo.Civilizations.CIVILIZATION_ROME.ID ~= nil)"
		" assert(GameDefines.MAX_MAJOR_CIVS == 22) assert(io == nil and require == nil) assert(SlotStatus.SS_TAKEN == 3)"
		" local n = 0 Events.HstSelfTestEvent.Add(function(x) n = n + x end) Events.HstSelfTestEvent(2) assert(n == 2)"
		" assert(Controls.Some.Box:SetHide(true) == nil) assert(UI.GetHeadSelectedCity() == nil)", "=selftest");
	HST_CHECK(pkSS->GetErrorCount() == iErrors, "exe Lua API: table.fill/count, math.clamp, GameInfo, GameDefines, sandbox, Events, auto-stubs");
	if(bInGame)
	{
		pkSS->RunString(L, "local d = Modding.OpenSaveData() d.SetValue('hst_selftest', 42) assert(d:GetValue('hst_selftest') == 42)"
			" d:SetValue('hst_selftest', 'x') assert(d.GetValue('hst_selftest') == 'x') d.SetValue('hst_selftest', nil)", "=selftest");
		HST_CHECK(pkSS->GetErrorCount() == iErrors, "Modding.OpenSaveData: GetValue/SetValue in both call styles");
		// Lekmod_global_dummies.lua (Events.SequenceGameInitComplete): every ever-alive player whose civilization has a
		// Civilization_Dummy_Policies row holds that policy.
		// Lekmod iterates ipairs(Players), which starts at 1: Players[0] (slot 0) never gets its dummy policy (Lekmod bug,
		// same in the real game). Expected = players 1..63; slot 0 is reported separately.
		pkSS->RunString(L, "HstDummyExpected, HstDummyPresent, HstDummySlot0 = 0, 0, 'n/a'"
			" for row in GameInfo.Civilization_Dummy_Policies() do for i = 0, #Players do local p = Players[i]"
			" if p:IsEverAlive() and p:GetCivilizationType() == GameInfoTypes[row.Type] then"
			" local has = p:HasPolicy(GameInfoTypes[row.PolicyType])"
			" if i == 0 then HstDummySlot0 = row.PolicyType .. (has and ' present' or ' missing') else"
			" HstDummyExpected = HstDummyExpected + 1 if has then HstDummyPresent = HstDummyPresent + 1 end end end end end", "=selftest");
		lua_getglobal(L, "HstDummyExpected");
		lua_getglobal(L, "HstDummyPresent");
		lua_getglobal(L, "HstDummySlot0");
		const int iExpected = (int)lua_tointeger(L, -3);
		const int iPresent = (int)lua_tointeger(L, -2);
		const std::string strSlot0 = lua_tostring(L, -1) ? lua_tostring(L, -1) : "?";
		lua_pop(L, 3);
		HST_CHECK(iPresent == iExpected, "Lekmod dummy policies present: %d of %d (players 1..63; slot 0: %s)", iPresent, iExpected, strSlot0.c_str());
	}
}

static int HstRun(int argc, wchar_t** argv)
{
	HstCliOptions kOpt;
	if(!HstParseArgs(argc, argv, kOpt))
		return 1;
	if(kOpt.bHelp)
		return 0;
	const bool bControl = !kOpt.strControl.empty();
	FILE* pkProtocol = NULL;
	if(bControl)
	{
		// The protocol owns the original stdout; CRT stdout (DLL printf, stray output) goes to NUL, HstLog to civhost.log.
		const int fdProtocol = _dup(_fileno(stdout));
		_setmode(fdProtocol, _O_BINARY);
		pkProtocol = _fdopen(fdProtocol, "wb");
		freopen("NUL", "w", stdout);
		HstLogSetConsole(false);
		_setmode(_fileno(stdin), _O_BINARY);
	}

	HstSession kSession;
	int iBoot = kSession.BootPlatform(kOpt.kSession);
	if(iBoot)
		return iBoot;
	if(kOpt.kSession.bSelfTest)
		HstSelfTestUnits();
	iBoot = kSession.BootEngine(kOpt.strStage == "load");
	if(iBoot)
		return iBoot;
	if(kOpt.strStage == "load")
	{
		HstLog("civhost: stage 'load' done");
		return g_iHstCheckFailures ? 2 : 0;
	}

	if(bControl)
	{
		const int iResult = HstControlRun(kSession, pkProtocol, stdin);
		kSession.WriteReports();
		return iResult;
	}

	int iGameResult = 0;
	if(kOpt.strStage == "map" || kOpt.strStage == "turns")
	{
		std::string strError;
		bool bOk = false;
		if(!kOpt.wstrLoad.empty())
			bOk = kSession.LoadGame(kOpt.wstrLoad, strError);
		else if(!kOpt.wstrImport.empty())
			bOk = kSession.ImportSave(kOpt.wstrImport, kOpt.bImportKeepHumans, strError);
		else
		{
			HstScenario kScenario;
			{
				HstDetHeapPauseScope kPause;
				bOk = kScenario.Load(kOpt.wstrScenario, strError);
				if(bOk && !kOpt.strActivePlayer.empty())
					kScenario.SetActivePlayer(kOpt.strActivePlayer);
				if(bOk && !kOpt.strGameType.empty())
					kScenario.SetGameType(kOpt.strGameType);
				if(bOk && !kOpt.strGameOptionsPreset.empty())
					bOk = kScenario.SetGameOptionsPreset(kOpt.strGameOptionsPreset, strError);
			}
			if(!bOk)
				HstLog("scenario: cannot load %s: %s", HstToUtf8(kOpt.wstrScenario).c_str(), strError.c_str());
			else
				bOk = kSession.NewGame(kScenario, strError);
		}
		if(!bOk)
		{
			HstLog("civhost: game start failed: %s", strError.c_str());
			iGameResult = 2;
		}
		else
		{
			const int iErrorsAtStart = kSession.GetScriptSystem()->GetErrorCount();
			HST_CHECK(iErrorsAtStart == 0, "no Lua errors at game start (%d; map script + gameplay contexts + SequenceGameInitComplete)", iErrorsAtStart);
			if(kOpt.strStage == "turns")
			{
				HstRunResult kResult = kSession.Run(kOpt.kRun, NULL);
				kSession.WriteRunJson(kResult.strReason == "done" ? "ok" : kResult.strReason);
				const int iExpected = kOpt.kRun.iUntilTurn >= 0 ? std::max(0, kOpt.kRun.iUntilTurn - kResult.iStartTurn) : kOpt.kRun.iTurns;
				HST_CHECK(kResult.iTurnsDone == iExpected, "%d turn(s) completed", iExpected);
				HST_CHECK(kSession.GetScriptSystem()->GetErrorCount() == 0, "no Lua errors during the turns (%d)", kSession.GetScriptSystem()->GetErrorCount());
				iGameResult = kResult.iExitCode;
			}
			else
				kSession.WriteRunJson("ok");
		}
		kSession.WriteReports();
		if(kOpt.kSession.bSelfTest)
			HstSelfTestScript(kSession.GetScriptSystem(), bOk);
	}
	// Sandbox and API summary
	if(!kOpt.kSession.bNoSandbox)
		HST_CHECK(HstSandboxGetDenied() == 0, "sandbox: %d denied writes outside the run dir by game modules (%d allowed inside; %d dbghelp PDB write-opens refused)",
			HstSandboxGetDenied(), HstSandboxGetWritesAllowed(), HstSandboxGetDeniedTool());
	HstLog("civhost: stage '%s' done, %d check failure(s)", kOpt.strStage.c_str(), g_iHstCheckFailures);
	if(iGameResult != 0 && iGameResult != 2)
		return iGameResult;
	return (g_iHstCheckFailures || iGameResult == 2) ? 2 : 0;
}

int wmain(int argc, wchar_t** argv)
{
	int iResult = 3;
	// Game entry points run 256 KB below this frame (HstRunAtFixedStack); the exe reserves a 4 MB stack (vcproj /STACK).
	HstFixedStackInit(256 * 1024);
	HstInstallUnhandledFilter();
	__try
	{
		iResult = HstRun(argc, argv);
	}
	__except(HstCrashFilter(GetExceptionInformation()))
	{
		iResult = 3;
	}
	HstLog("civhost: exit %d", iResult);
	HstExeExportsFlush();
	HstLogFlush();
	// Skip DLL teardown (DLL_PROCESS_DETACH / static destructors of the gamecore); all outputs are flushed.
	TerminateProcess(GetCurrentProcess(), (UINT)iResult);
	return iResult;
}
