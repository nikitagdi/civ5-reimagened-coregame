//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstSession.cpp
//!  \brief     Boot (design 1.1-1.5), new game (1.6-1.7), checkpoints (M4b), turn loop (1.8), reports.
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#include "HstCommon.h"
#include "HstApi.h"
#include "control/HstSession.h"
#include "platform/HstPlatform.h"
#include "platform/HstDetHeap.h"
#include "dllhost/HstDllHost.h"
#include "data/HstDatabase.h"
#include "engine/HstEngineUtility.h"
#include "engine/HstUserInterface.h"
#include "engine/HstExeExports.h"
#include "script/HstArenaAlloc.h"
#include "script/HstVfs.h"
#include "script/HstScriptSystem.h"
#include "script/HstExeLua.h"
#include "game/HstScenario.h"
#include "game/HstCheckpoint.h"

int g_iHstCheckFailures = 0;

static const unsigned int s_uiHstWipeBytes = 256 * 1024;

double HstNowMs()
{
	LARGE_INTEGER kFreq, kNow;
	QueryPerformanceFrequency(&kFreq);
	QueryPerformanceCounter(&kNow);
	return (double)kNow.QuadPart * 1000.0 / (double)kFreq.QuadPart;
}

std::string HstJsonEscape(const std::string& str)
{
	std::string strOut;
	for(size_t i = 0; i < str.size(); ++i)
	{
		const char c = str[i];
		if(c == '\\' || c == '"')
		{
			strOut += '\\';
			strOut += c;
		}
		else if((unsigned char)c < 0x20)
		{
			char szBuf[8];
			_snprintf_s(szBuf, sizeof(szBuf), _TRUNCATE, "\\u%04x", (unsigned int)(unsigned char)c);
			strOut += szBuf;
		}
		else
			strOut += c;
	}
	return strOut;
}

//! xxh64 of a whole file (0 when unreadable). Host-only: paused det heap.
static HstHash64 HstHashFile(const std::wstring& wstrPath)
{
	HstDetHeapPauseScope kPause;
	std::string strData;
	if(!HstVfs::ReadFile(wstrPath, strData))
		return 0;
	return HstXxh64(strData.data(), strData.size());
}

HstSession::HstSession()
	: m_pkArena(NULL)
	, m_pkHost(NULL)
	, m_pkDb(NULL)
	, m_pkEngine(NULL)
	, m_pkUI(NULL)
	, m_pkScriptSystem(NULL)
	, m_pkMapVfs(NULL)
	, m_pkContextVfs(NULL)
	, m_pkRunner(NULL)
	, m_pkDigest(NULL)
	, m_bInGame(false)
	, m_iGameId(0)
	, m_iStartTurn(0)
	, m_ui64MapDigest(0)
	, m_dLaunchMs(0.0)
	, m_uiFpuStart(0)
	, m_ui64DllHash(0)
	, m_ui64DataHash(0)
	, m_uiBootVirtualMs(0)
	, m_uiBootGuidCounter(0)
	, m_pkStepGame(NULL)
	, m_pkMaskDigestFile(NULL)
	, m_iFrame(0)
	, m_pkBotThread(NULL)
{
	memset(&m_kStepTurn, 0, sizeof(m_kStepTurn));
}

ICvGameContext1* HstSession::GetContext() const
{
	return m_pkHost ? m_pkHost->GetContext() : NULL;
}

std::string HstSession::GetDllVersion() const
{
	return m_pkHost ? m_pkHost->GetVersion() : std::string("?");
}

std::wstring HstSession::ResolveUserPath(const std::string& strUtf8) const
{
	HstDetHeapPauseScope kPause;
	std::wstring wstr = HstFromUtf8(strUtf8);
	for(size_t i = 0; i < wstr.size(); ++i)
		if(wstr[i] == L'/')
			wstr[i] = L'\\';
	const bool bAbsolute = wstr.size() > 1 && (wstr[1] == L':' || (wstr[0] == L'\\' && wstr[1] == L'\\'));
	if(bAbsolute || m_kOpt.wstrLaunchDir.empty())
		return HstFullPath(wstr);
	return HstFullPath(HstJoinPath(m_kOpt.wstrLaunchDir, wstr));
}

std::wstring HstSession::RunPath(const std::wstring& wstrName)
{
	HstDetHeapPauseScope kPause;
	return HstJoinPath(HstGetRunDir(), wstrName);
}

// =====================================================================================================================
// Boot
// =====================================================================================================================
int HstSession::BootPlatform(const HstSessionOptions& kOpt)
{
	m_kOpt = kOpt;
	HstApiSetStrict(kOpt.bStrict);
	// P1-P3 + logging + sandbox before anything from the game is loaded.
	if(!HstPlatformInit(kOpt.wstrOut, kOpt.wstrGame))
	{
		fprintf(stderr, "civhost: cannot set up run dir %s / game dir %s (error %u)\n", HstToUtf8(kOpt.wstrOut).c_str(), HstToUtf8(kOpt.wstrGame).c_str(),
			(unsigned int)GetLastError());
		return 1;
	}
	HstLogOpen(HstJoinPath(HstGetRunDir(), L"civhost.log"));
	HstLog("%s", HST_HOST_VERSION);
	HstLog("run dir : %s", HstToUtf8(HstGetRunDir()).c_str());
	HstLog("game dir: %s", HstToUtf8(HstGetGameDir()).c_str());
	HstLog("dll     : %s", HstToUtf8(kOpt.wstrDll).c_str());
	HstLog("data    : %s", HstToUtf8(kOpt.wstrData).c_str());
	if(!kOpt.wstrLuaOverlay.empty())
		HstLog("lua overlay: %s", HstToUtf8(kOpt.wstrLuaOverlay).c_str());
	// Determinism-lab axes: stack pattern and x87 precision are process-wide from here on (heap axes: BootEngineImpl).
	if(kOpt.iStackFill >= 0)
		HstSetStackFill((unsigned char)kOpt.iStackFill);
	HstDigest::SetTraceWrites(kOpt.bTraceWrites);
	HstFpuConfigure(kOpt.iFpuPc, kOpt.iFpuPcGlobal, kOpt.bFpuCheck);
	HstLog("fpu cw  : 0x%04x", HstGetFpuControlWord());
	if(kOpt.HasPerturbation() || kOpt.bFpuCheck)
		HstLog("perturb : heap-fill=%d stack-fill=%d heap-shift=%u heap-jitter=%u fpu-pc=%d fpu-pc-global=%d fpu-check=%d (%s)", kOpt.iHeapFill, kOpt.iStackFill,
			kOpt.uiHeapShift, kOpt.uiHeapJitter, kOpt.iFpuPc, kOpt.iFpuPcGlobal, (int)kOpt.bFpuCheck, kOpt.strPerturbNote.c_str());
	int iStackProbe = 0;
	HstLog("stack   : %p", (void*)&iStackProbe);
	if(kOpt.bNoSandbox)
		HstLog("civhost: WARNING --no-sandbox: file APIs are not checked");
	else
	{
		if(!HstSandboxInit(HstGetRunDir()))
		{
			HstLog("civhost: sandbox init failed");
			return 1;
		}
		HstSandboxPatchKnownModules();
	}
	if(kOpt.bTraceExceptions)
		HstInstallFirstChanceLogger();
	size_t uiSlash = kOpt.wstrDll.find_last_of(L"\\/");
	if(uiSlash != std::wstring::npos)
		HstAddSymbolPath(kOpt.wstrDll.substr(0, uiSlash));
	return 0;
}

int HstSession::BootEngineImpl(bool bStopAfterLoad)
{
	// R6: reserve the Lua arena at a fixed base before the game DLLs and their heaps take address space.
	m_pkArena = new HstArenaAlloc();
	if(!m_pkArena->Init(768 * 1024 * 1024))
	{
		HstLog("civhost: cannot reserve the Lua arena");
		return 1;
	}
	HstLog("lua arena: base 0x%08x (%s)", (unsigned int)(DWORD_PTR)m_pkArena->GetBase(), m_pkArena->IsFixedBase() ? "fixed" : "NOT fixed: Lua addresses vary per process");
	// Deterministic, zero-filling C heap for everything from here on (HstDetHeap.h).
	if(m_kOpt.iHeapFill >= 0)
		HstDetHeapSetFill((unsigned char)m_kOpt.iHeapFill);
	HstDetHeapSetShift(m_kOpt.uiHeapShift);
	HstDetHeapSetJitter(m_kOpt.uiHeapJitter);
	if(!m_kOpt.bNoDetHeap && !HstDetHeapInstall(1536u * 1024 * 1024))
		HstLog("civhost: WARNING deterministic heap not installed; digests will differ between processes");

	// Exe-provided FireWorks services must exist before the DLL's static initializers run.
	HstExeExportsInit(m_kOpt.bDllLogs);

	// D1-D2 (M0)
	m_pkHost = new HstDllHost();
	if(!m_pkHost->Load(m_kOpt.wstrDll))
		return 1;
	// Diag DLLs: RNG sink before ctx->Init() creates the game objects (HeadlessDiag.h install order).
	if(m_kOpt.kRngLog.Any())
		HstRngLog::Install(m_pkHost->GetModule(), m_kOpt.kRngLog);
	GUID kDllGuid = m_pkHost->GetContext()->GetDLLGUID();
	HstLog("dll version: %s  guid {%08X-%04X-%04X-%02X%02X-...}", m_pkHost->GetVersion().c_str(), (unsigned int)kDllGuid.Data1,
		kDllGuid.Data2, kDllGuid.Data3, kDllGuid.Data4[0], kDllGuid.Data4[1]);
	if(bStopAfterLoad)
		return 0;

	// B1-B4 (M1). Copies in the run dir; the gameplay copy is loaded into memory and never written. Paused det heap:
	// SQLite keeps run-dir-length-dependent allocations (file names) for the connection's lifetime.
	m_pkDb = new HstDatabase();	// lives until process exit (the DLL keeps the connection)
	int iDbCivs = 0, iDbUnits = 0;
	std::string strRome;
	{
		HstDetHeapPauseScope kPause;
		std::wstring wstrGameplay = HstJoinPath(HstGetRunDir(), L"gameplay.db");
		std::wstring wstrLocalization = HstJoinPath(HstGetRunDir(), L"localization.db");
		if(!HstCopyIfChanged(HstJoinPath(m_kOpt.wstrData, L"Civ5DebugDatabase.db"), wstrGameplay) ||
			!HstCopyIfChanged(HstJoinPath(m_kOpt.wstrData, L"Localization-Merged.db"), wstrLocalization))
		{
			HstLog("civhost: cannot copy the data profile from %s", HstToUtf8(m_kOpt.wstrData).c_str());
			return 1;
		}
		DWORD dwStart = GetTickCount();
		// Relative names (CWD = run dir): SQLite re-expands file names on every transaction; see finding 12.
		if(!m_pkDb->OpenGameplay("gameplay.db"))
			return 1;
		iDbCivs = m_pkDb->Count("Civilizations");
		iDbUnits = m_pkDb->Count("Units");
		HstLog("db: gameplay loaded (%u ms): Civilizations=%d Units=%d", (unsigned int)(GetTickCount() - dwStart), iDbCivs, iDbUnits);
		if(!m_pkDb->AttachLocalization("localization.db"))
			return 1;
		if(!m_pkDb->InitLocalization(HstJoinPath(HstGetGameDir(), L"Locale\\icudt44l.dat"), m_kOpt.strLanguage.c_str()))
			return 1;
		strRome = m_pkDb->Lookup("TXT_KEY_CIV_ROME_DESC");
		HstLog("db: localization ok: TXT_KEY_CIV_ROME_DESC = '%s'", strRome.c_str());
		m_ui64DataHash = HstHashFile(wstrGameplay);
	}
	m_ui64DllHash = HstHashFile(m_kOpt.wstrDll);
	if(m_kOpt.bSelfTest)
	{
		HST_CHECK(iDbCivs == 115 && iDbUnits == 230, "gameplay DB counts (Civilizations=115, Units=230)");
		HST_CHECK(!strRome.empty() && strRome != "TXT_KEY_CIV_ROME_DESC", "localization lookup");
	}

	// I1-I8
	m_pkEngine = new HstEngineUtility();
	m_pkUI = new HstUserInterface();
	HstWipeStack(s_uiHstWipeBytes);	// residue of the path-heavy setup above must not reach GC.init()
	if(!m_pkHost->Init(m_pkEngine, m_pkUI, m_pkDb))
		return 1;

	ICvGameContext1* pkCtx = GetContext();
	const int iCivs = pkCtx->GetNumCivilizationInfos();
	const int iUnits = pkCtx->GetNumUnitInfos();
	HstLog("dll: GetNumCivilizationInfos=%d GetNumUnitInfos=%d GetNumMinorCivInfos=%d GetNumLeaderHeadInfos=%d", iCivs, iUnits,
		pkCtx->GetNumMinorCivInfos(), pkCtx->GetNumLeaderHeadInfos());
	// Info arrays are indexed by ID, so their size is MAX(ID)+1 (IDs have gaps): 116 civs for 115 rows, 265 units for 230.
	std::string strMaxCiv, strMaxUnit;
	m_pkDb->QueryText("SELECT MAX(ID)+1 FROM Civilizations", strMaxCiv);
	m_pkDb->QueryText("SELECT MAX(ID)+1 FROM Units", strMaxUnit);
	HST_CHECK(iCivs == atoi(strMaxCiv.c_str()) && iDbCivs == 115, "GetNumCivilizationInfos (%d) == MAX(Civilizations.ID)+1 (%s); %d rows", iCivs, strMaxCiv.c_str(), iDbCivs);
	HST_CHECK(iUnits == atoi(strMaxUnit.c_str()) && iDbUnits == 230, "GetNumUnitInfos (%d) == MAX(Units.ID)+1 (%s); %d rows", iUnits, strMaxUnit.c_str(), iDbUnits);
	{
		HstDetHeapPauseScope kPause;
		HST_CHECK(HstFileExists(HstJoinPath(HstGetRunDir(), L"Civ5SavedGameDatabase.db")), "Civ5SavedGameDatabase.db created in the run dir");
	}
	m_pkEngine->SetContext(pkCtx);
	m_pkUI->SetContext(pkCtx, m_pkEngine);
	g_pkHstLuaEngine = m_pkEngine;
	g_pkHstLuaUI = m_pkUI;
	m_pkRunner = new HstGameRunner(pkCtx);

	// Lua master state (1.5) + VFS roots for map scripts and gameplay contexts.
	SetupVfs();
	m_pkScriptSystem = new HstScriptSystem();
	DWORD dwStart = GetTickCount();
	HstWipeStack(s_uiHstWipeBytes);
	if(!m_pkScriptSystem->Init(m_pkArena, m_pkMapVfs, m_pkDb->GetConnection(), pkCtx, m_kOpt.bStrict))
	{
		HstLog("lua: script system init failed");
		return 2;
	}
	m_pkEngine->SetScriptSystem(m_pkScriptSystem);
	HstLog("lua: master state ready (%u ms, arena %u KB used)", (unsigned int)(GetTickCount() - dwStart), (unsigned int)(m_pkArena->GetUsedBytes() / 1024));
	m_uiBootVirtualMs = HstVirtualTimeMs();
	m_uiBootGuidCounter = HstGuidCounterGet();
	return 0;
}

void HstSession::SetupVfs()
{
	// Map scripts (M2): built-in order until vfs.json exists; LoadFile puts the map script folder first.
	m_pkMapVfs = new HstVfs();
	m_pkMapVfs->SetGameDir(HstGetGameDir());
	m_pkMapVfs->AddIncludeRoot(L"Assets\\DLC\\Expansion2\\Gameplay\\Lua", true);
	m_pkMapVfs->AddIncludeRoot(L"Assets\\DLC\\Expansion\\Gameplay\\Lua", true);
	m_pkMapVfs->AddIncludeRoot(L"Assets\\Gameplay\\Lua", true);
	m_pkMapVfs->AddIncludeRoot(L"Assets\\Maps", true);

	// Gameplay contexts (design 3.4): --lua-overlay, the Lekmod DLC (its Civ5Pkg GameplaySkin lists Art and Lua; both
	// hold context scripts), then the base UI and gameplay folders (FLuaVector, IconSupport, ...). Paths vary per
	// machine, so this VFS indexes with the deterministic heap paused.
	HstDetHeapPauseScope kPause;
	m_pkContextVfs = new HstVfs();
	m_pkContextVfs->SetGameDir(HstGetGameDir());
	m_pkContextVfs->SetPauseDetHeap(true);
	if(!m_kOpt.wstrLuaOverlay.empty())
		m_pkContextVfs->AddIncludeRoot(m_kOpt.wstrLuaOverlay, true);
	std::wstring wstrDlc = m_kOpt.wstrLuaDlc;
	if(wstrDlc.empty())
	{
		wstrDlc = L"Assets\\DLC\\LEKMOD_v34.15";
		if(!HstDirExists(HstJoinPath(HstGetGameDir(), wstrDlc)))
		{
			// Any installed LEKMOD* package (last in name order = newest version string).
			WIN32_FIND_DATAW kData;
			HANDLE hFind = FindFirstFileW(HstJoinPath(HstGetGameDir(), L"Assets\\DLC\\LEKMOD*").c_str(), &kData);
			if(hFind != INVALID_HANDLE_VALUE)
			{
				do
				{
					if(kData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
						wstrDlc = std::wstring(L"Assets\\DLC\\") + kData.cFileName;
				}
				while(FindNextFileW(hFind, &kData));
				FindClose(hFind);
			}
		}
	}
	HstLog("lua: context roots: %s%s, %s, base UI + gameplay", m_kOpt.wstrLuaOverlay.empty() ? "" : "overlay, ", HstToUtf8(wstrDlc).c_str(),
		HstDirExists(HstJoinPath(HstGetGameDir(), wstrDlc)) || HstDirExists(wstrDlc) ? "found" : "MISSING");
	m_pkContextVfs->AddIncludeRoot(wstrDlc, true);
	m_pkContextVfs->AddIncludeRoot(L"Assets\\DLC\\Expansion2\\UI", true);
	m_pkContextVfs->AddIncludeRoot(L"Assets\\DLC\\Expansion\\UI", true);
	m_pkContextVfs->AddIncludeRoot(L"Assets\\UI", true);
	m_pkContextVfs->AddIncludeRoot(L"Assets\\DLC\\Expansion2\\Gameplay\\Lua", true);
	m_pkContextVfs->AddIncludeRoot(L"Assets\\DLC\\Expansion\\Gameplay\\Lua", true);
	m_pkContextVfs->AddIncludeRoot(L"Assets\\Gameplay\\Lua", true);
}

// =====================================================================================================================
// Game start
// =====================================================================================================================

//! Non-commented ContextPtr:LoadNewContext("X") lines with a literal name, in file order (design 3.4).
bool HstSession::ReadContextManifest(std::vector<std::string>& astrNames, std::string& strSource)
{
	HstDetHeapPauseScope kPause;
	std::wstring wstrPath;
	if(!m_pkContextVfs->ResolveInclude("InGame", wstrPath))
		return false;
	strSource = HstToUtf8(wstrPath);
	std::string strText;
	if(!HstVfs::ReadFile(wstrPath, strText))
		return false;
	bool bBlockComment = false;
	size_t uiPos = 0;
	while(uiPos < strText.size())
	{
		size_t uiEnd = strText.find('\n', uiPos);
		if(uiEnd == std::string::npos)
			uiEnd = strText.size();
		std::string strLine = strText.substr(uiPos, uiEnd - uiPos);
		uiPos = uiEnd + 1;
		if(bBlockComment)
		{
			if(strLine.find("]]") != std::string::npos)
				bBlockComment = false;
			continue;
		}
		size_t uiFirst = strLine.find_first_not_of(" \t");
		if(uiFirst == std::string::npos)
			continue;
		if(strLine.compare(uiFirst, 4, "--[[") == 0)
		{
			bBlockComment = strLine.find("]]", uiFirst + 4) == std::string::npos;
			continue;
		}
		if(strLine.compare(uiFirst, 2, "--") == 0)
			continue;
		const char* szCall = "ContextPtr:LoadNewContext(";
		size_t uiCall = strLine.find(szCall, uiFirst);
		if(uiCall == std::string::npos)
			continue;
		size_t uiComment = strLine.find("--", uiFirst);
		if(uiComment != std::string::npos && uiComment < uiCall)
			continue;
		size_t uiQuote = strLine.find_first_not_of(" \t", uiCall + strlen(szCall));
		if(uiQuote == std::string::npos || (strLine[uiQuote] != '"' && strLine[uiQuote] != '\''))
			continue;	// computed names (mod add-ins) are not gameplay contexts
		size_t uiClose = strLine.find(strLine[uiQuote], uiQuote + 1);
		if(uiClose == std::string::npos)
			continue;
		astrNames.push_back(strLine.substr(uiQuote + 1, uiClose - uiQuote - 1));
	}
	return true;
}

void HstSession::StartGame(const HstScenario* pkScenario, bool bLoaded)
{
	// L7: gameplay contexts (GUESS timing: the exe loads InGame during LoadGameState, before NetPlayerReady).
	m_astrContextNames.clear();
	m_strContextSource.clear();
	if(!m_kOpt.bNoContexts)
	{
		if(pkScenario && pkScenario->m_bHasLuaContexts)
		{
			m_astrContextNames = pkScenario->m_aLuaContexts;
			m_strContextSource = "scenario lua_contexts";
		}
		else if(!ReadContextManifest(m_astrContextNames, m_strContextSource))
			HstLog("lua: WARNING InGame.lua not found in the context roots; no gameplay contexts");
	}
	m_pkScriptSystem->SetVfs(m_pkContextVfs);
	const int iErrorsBefore = m_pkScriptSystem->GetErrorCount();
	const double dStart = HstNowMs();
	HstWipeStack(s_uiHstWipeBytes);
	for(size_t i = 0; i < m_astrContextNames.size(); ++i)
		m_pkScriptSystem->LoadContext(m_astrContextNames[i].c_str());
	int iLoaded = 0;
	const std::vector<HstScriptSystem::ContextInfo>& akContexts = m_pkScriptSystem->GetContexts();
	for(size_t i = 0; i < akContexts.size(); ++i)
		iLoaded += akContexts[i].bLoaded ? 1 : 0;
	HstLog("lua: %d/%d gameplay context(s) loaded from %s (%.1f ms, %d Lua error(s))", iLoaded, (int)m_astrContextNames.size(),
		m_strContextSource.empty() ? "-" : m_strContextSource.c_str(), HstNowMs() - dStart, m_pkScriptSystem->GetErrorCount() - iErrorsBefore);

	// L8: DoGameStarted + SendPlayerOptions + Clear*Deltas (exe NetPlayerReady_Execute 0x80c80; also after a load).
	m_pkRunner->StartGame();

	// L9: Events.SequenceGameInitComplete, once per new game and once per load (GUESS: after L8). Lekmod changes state
	// here (dummy policies, Tonga, Georgia, Yugoslavia); in the real game it fires locally on every load.
	HstWipeStack(s_uiHstWipeBytes);
	const int iListeners = m_pkScriptSystem->FireEvent("SequenceGameInitComplete");
	HstLog("lua: Events.SequenceGameInitComplete fired (%d listener(s))%s", iListeners, bLoaded ? " after load" : "");
	m_uiFpuStart = HstGetFpuControlWord();
}

bool HstSession::OpenDigestFile()
{
	HstDetHeapPauseScope kPause;
	if(!m_pkDigest)
		m_pkDigest = new HstDigest(GetContext());
	wchar_t wszName[32];
	if(m_iGameId <= 1)
		wcscpy_s(wszName, L"digest.bin");
	else
		swprintf_s(wszName, L"digest-%d.bin", m_iGameId);
	m_wstrDigestPath = HstJoinPath(HstGetRunDir(), wszName);
	return m_pkDigest->OpenFile(m_wstrDigestPath);
}

bool HstSession::NewGameImpl(HstScenario& kScenario, std::string& strError)
{
	if(m_bInGame)
		EndGameImpl();
	++m_iGameId;
	m_akTurns.clear();
	m_iFrame = 0;
	HstRngLog::BeginGame(m_iGameId);
	HstRngPhaseScope kRngPregame(-1);		// no-op unless the diag sink is installed (--rng-digest / --rng-log)
	m_strScenarioName = kScenario.m_strName;
	m_pkScriptSystem->SetVfs(m_pkMapVfs);
	m_pkScriptSystem->CloseSaveData();
	m_pkEngine->ResetHostState();
	// Warm start: host-owned inputs back to their process-start values (virtual clock, CoCreateGuid counter -> map GUID,
	// Lua math.random / os.clock), so game N in a process starts like game 1 of a fresh process.
	HstVirtualTimeSet(m_uiBootVirtualMs);
	HstGuidCounterSet(m_uiBootGuidCounter);
	HstExeLuaResetGameState();

	ICvGameContext1* pkCtx = GetContext();
	HstWipeStack(s_uiHstWipeBytes);
	if(!kScenario.ApplyPreGame(pkCtx, m_pkDb))
	{
		strError = "scenario: pregame setup failed";
		HstLog("%s", strError.c_str());
		return false;
	}
	HstLog("%s", kScenario.Describe().c_str());

	const double dLaunchStart = HstNowMs();
	const bool bInit2 = m_pkRunner->LaunchToMap();
	m_dLaunchMs = HstNowMs() - dLaunchStart;
	m_pkRunner->LogMapSummary();
	m_ui64MapDigest = m_pkRunner->MapDigest();
	HstLog("map digest: %016I64x", m_ui64MapDigest);
	HstLog("lua: %d error(s), %d auto-stub global(s), arena %u KB used / %u KB live", m_pkScriptSystem->GetErrorCount(), HstExeLuaGetStubCount(),
		(unsigned int)(m_pkArena->GetUsedBytes() / 1024), (unsigned int)(m_pkArena->GetLiveBytes() / 1024));
	{
		HstDetHeapPauseScope kPause;
		FILE* pkDigest = _wfopen(HstJoinPath(HstGetRunDir(), L"map-digest.txt").c_str(), L"w");
		if(pkDigest)
		{
			fprintf(pkDigest, "%016I64x\n", m_ui64MapDigest);
			fclose(pkDigest);
		}
	}
	HST_CHECK(bInit2, "game->Init2() returned true");
	HST_CHECK(m_pkScriptSystem->GetErrorCount() == 0, "no Lua errors during map generation (%d)", m_pkScriptSystem->GetErrorCount());
	if(!bInit2)
	{
		strError = "game->Init2() failed";
		return false;
	}
	StartGame(&kScenario, false);
	if(!OpenDigestFile())
	{
		strError = "cannot create the digest file";
		return false;
	}
	m_iStartTurn = GetGameTurn();
	m_bInGame = true;
	return true;
}

void HstSession::EndGameImpl()
{
	if(!m_bInGame)
		return;
	if(m_pkStepGame)
	{
		HstRngLog::EndPhase();
		delete m_pkStepGame;
		m_pkStepGame = NULL;
	}
	WriteReports();
	HstRngLog::EndGame();
	m_pkScriptSystem->ResetContexts();
	m_pkScriptSystem->CloseSaveData();
	if(m_pkDigest)
	{
		HstDetHeapPauseScope kPause;
		m_pkDigest->CloseFile();
	}
	if(m_pkMaskDigestFile)
	{
		HstDetHeapPauseScope kPause;
		delete m_pkMaskDigestFile;	// closes digest-masked.bin
		m_pkMaskDigestFile = NULL;
	}
	m_bInGame = false;
	HstLog("session: game %d ended", m_iGameId);
}

// =====================================================================================================================
// Checkpoints (M4b)
// =====================================================================================================================
bool HstSession::SaveGameImpl(const std::wstring& wstrPath, unsigned int& uiBytes, HstHash64& ui64Hash, std::string& strError)
{
	if(!m_bInGame)
	{
		strError = "no game";
		return false;
	}
	// The whole save runs with the deterministic heap paused: Write() is const, and the ~19 MB of section copies must
	// not shift the heap layout of the run that continues (a run with checkpoints = the same run without them).
	HstDetHeapPauseScope kPauseAll;
	ICvGameContext1* pkCtx = GetContext();
	std::auto_ptr<ICvGame1> pkGame(pkCtx->GetGame());
	HstCheckpointFile kFile;
	{
		char szHeader[1024];
		GUID kGuid = pkCtx->GetDLLGUID();
		_snprintf_s(szHeader, sizeof(szHeader), _TRUNCATE,
			"host=%s\ndll_version=%s\ndll_guid=%08X-%04X-%04X\ndll_xxh64=%016I64x\ndata_xxh64=%016I64x\nscenario=%s\nturn=%d\n"
			"virtual_ms=%u\nguid_counter=%u\n",
			HST_HOST_VERSION, GetDllVersion().c_str(), (unsigned int)kGuid.Data1, kGuid.Data2, kGuid.Data3, m_ui64DllHash, m_ui64DataHash,
			m_strScenarioName.c_str(), pkGame->GetGameTurn(), HstVirtualTimeMs(), HstGuidCounterGet());
		kFile.Add("header", szHeader, (unsigned int)strlen(szHeader));
	}
	{
		FMemoryStream kStream;
		const unsigned int uiMs = HstVirtualTimeMs();
		const unsigned int uiGuids = HstGuidCounterGet();
		kStream.WriteIt(4, &uiMs);
		kStream.WriteIt(4, &uiGuids);
		m_pkEngine->WriteHostState(kStream);
		kFile.Add("host", kStream);
	}
	{
		std::auto_ptr<ICvPreGame1> pkPreGame(pkCtx->GetPreGame());
		FMemoryStream kStream;
		pkPreGame->write(kStream);
		kFile.Add("pregame", kStream);
	}
	// Same order as the exe's loader reads (HstCheckpoint.h).
	{
		FMemoryStream kStream;
		pkGame->Write(kStream);
		kFile.Add("game", kStream);
	}
	{
		std::auto_ptr<ICvMap1> pkMap(pkCtx->GetMap());
		FMemoryStream kStream;
		pkMap->Write(kStream);
		kFile.Add("map", kStream);
	}
	char szName[32];
	for(int i = 0; i < MAX_TEAMS; ++i)
	{
		std::auto_ptr<ICvTeam1> pkTeam(pkCtx->GetTeam((TeamTypes)i));
		FMemoryStream kStream;
		if(pkTeam.get())
			pkTeam->Write(kStream);
		_snprintf_s(szName, sizeof(szName), _TRUNCATE, "team%02d", i);
		kFile.Add(szName, kStream);
	}
	for(int i = 0; i < MAX_PLAYERS; ++i)
	{
		std::auto_ptr<ICvPlayer1> pkPlayer(pkGame->GetPlayer((PlayerTypes)i));
		FMemoryStream kStream;
		if(pkPlayer.get())
			pkPlayer->Write(kStream);
		_snprintf_s(szName, sizeof(szName), _TRUNCATE, "player%02d", i);
		kFile.Add(szName, kStream);
	}
	{
		FMemoryStream kStream;
		pkGame->WriteSupportingClassData(kStream);
		kFile.Add("supporting", kStream);
	}
	if(!kFile.Write(wstrPath, uiBytes, ui64Hash, strError))
		return false;
	HstLog("checkpoint: wrote %s (%u KB, turn %d, xxh64 %016I64x)", HstToUtf8(wstrPath).c_str(), uiBytes / 1024, pkGame->GetGameTurn(), ui64Hash);
	return true;
}

//! Feed one checkpoint section to a DLL Read; logs when the Read does not consume the section exactly.
template<class T> static void HstReadSection(const HstCheckpointFile& kFile, const char* szName, T& kReader, int& iMismatches)
{
	if(kFile.IsBodyStream() && strcmp(szName, "pregame") != 0)
	{
		// Save import: one body stream in the exe's order; each Read consumes what it needs and the next one continues.
		const std::string& strBody = kFile.GetBody();
		const unsigned int uiPos = kFile.GetBodyPos();
		if(uiPos >= strBody.size())
		{
			HstLog("import: body exhausted before '%s'", szName);
			++iMismatches;
			return;
		}
		FMemoryStream kStream((int)(strBody.size() - uiPos), strBody.data() + uiPos);
		HstWipeStack(s_uiHstWipeBytes);
		kReader(kStream);
		kFile.AddSpan(szName, uiPos, kStream.GetPosition());
		kFile.AdvanceBody(kStream.GetPosition());
		return;
	}
	const HstCheckpointFile::Section* pkSection = kFile.Find(szName);
	if(!pkSection)
	{
		HstLog("checkpoint: section '%s' missing", szName);
		++iMismatches;
		return;
	}
	FMemoryStream kStream((int)pkSection->strData.size(), pkSection->strData.data());
	HstWipeStack(s_uiHstWipeBytes);
	kReader(kStream);
	if(kStream.GetPosition() != pkSection->strData.size())
	{
		HstLog("checkpoint: '%s' read %u of %u bytes", szName, kStream.GetPosition(), (unsigned int)pkSection->strData.size());
		++iMismatches;
	}
}

struct HstReadGame { ICvGame1* p; void operator()(FDataStream& s) { p->Read(s); } };
struct HstReadSupporting { ICvGame1* p; void operator()(FDataStream& s) { p->ReadSupportingClassData(s); } };
struct HstReadMap { ICvMap1* p; void operator()(FDataStream& s) { p->Read(s); } };
struct HstReadTeam { ICvTeam1* p; void operator()(FDataStream& s) { if(p) p->Read(s); } };
struct HstReadPlayer { ICvPlayer1* p; void operator()(FDataStream& s) { if(p) p->Read(s); } };
struct HstReadPreGame { ICvPreGame1* p; void operator()(FDataStream& s) { p->read(s, true); } };

//! Debug (--ckpt-verify): serialize the current state and compare it section by section with a checkpoint.
static void HstCompareWithCheckpoint(ICvGameContext1* pkCtx, const HstCheckpointFile& kFile, const char* szLabel)
{
	HstDetHeapPauseScope kPause;
	std::auto_ptr<ICvGame1> pkGame(pkCtx->GetGame());
	std::vector<std::pair<std::string, std::string> > akNow;
	{
		FMemoryStream k;
		pkGame->Write(k);
		akNow.push_back(std::make_pair(std::string("game"), std::string(k.GetBuf(), k.GetEOF())));
	}
	{
		std::auto_ptr<ICvMap1> pkMap(pkCtx->GetMap());
		FMemoryStream k;
		pkMap->Write(k);
		akNow.push_back(std::make_pair(std::string("map"), std::string(k.GetBuf(), k.GetEOF())));
	}
	char szName[32];
	for(int i = 0; i < MAX_TEAMS; ++i)
	{
		std::auto_ptr<ICvTeam1> pkTeam(pkCtx->GetTeam((TeamTypes)i));
		FMemoryStream k;
		if(pkTeam.get())
			pkTeam->Write(k);
		_snprintf_s(szName, sizeof(szName), _TRUNCATE, "team%02d", i);
		akNow.push_back(std::make_pair(std::string(szName), std::string(k.GetBuf(), k.GetEOF())));
	}
	for(int i = 0; i < MAX_PLAYERS; ++i)
	{
		std::auto_ptr<ICvPlayer1> pkPlayer(pkGame->GetPlayer((PlayerTypes)i));
		FMemoryStream k;
		if(pkPlayer.get())
			pkPlayer->Write(k);
		_snprintf_s(szName, sizeof(szName), _TRUNCATE, "player%02d", i);
		akNow.push_back(std::make_pair(std::string(szName), std::string(k.GetBuf(), k.GetEOF())));
	}
	{
		FMemoryStream k;
		pkGame->WriteSupportingClassData(k);
		akNow.push_back(std::make_pair(std::string("supporting"), std::string(k.GetBuf(), k.GetEOF())));
	}
	std::wstring wstrDir = HstJoinPath(HstGetRunDir(), HstFromUtf8(std::string("ckpt-verify-") + szLabel));
	HstCreateDirs(wstrDir);
	int iDiffering = 0;
	for(size_t i = 0; i < akNow.size(); ++i)
	{
		const HstCheckpointFile::Section* pkSection = kFile.Find(akNow[i].first);
		const std::string& strNow = akNow[i].second;
		std::string strWasBody;						// save import: the object's bytes in the body stream, cut by the Read spans
		const std::string* pstrWas = pkSection ? &pkSection->strData : NULL;
		if(!pstrWas && kFile.IsBodyStream())
		{
			const HstCheckpointFile::Span* pkSpan = kFile.FindSpan(akNow[i].first);
			if(pkSpan && pkSpan->uiOffset + pkSpan->uiSize <= kFile.GetBody().size())
			{
				strWasBody = kFile.GetBody().substr(pkSpan->uiOffset, pkSpan->uiSize);
				pstrWas = &strWasBody;
			}
		}
		if(pstrWas && *pstrWas == strNow)
			continue;
		++iDiffering;
		size_t uiFirst = 0;
		unsigned int uiDiffBytes = 0;
		if(pstrWas)
		{
			while(uiFirst < strNow.size() && uiFirst < pstrWas->size() && strNow[uiFirst] == (*pstrWas)[uiFirst])
				++uiFirst;
			for(size_t k = uiFirst; k < strNow.size() && k < pstrWas->size(); ++k)
				uiDiffBytes += strNow[k] != (*pstrWas)[k] ? 1 : 0;
		}
		HstLog("ckpt-verify[%s]: %s differs: %u -> %u bytes, first difference at %u, %u differing byte(s) in the common part", szLabel, akNow[i].first.c_str(),
			pstrWas ? (unsigned int)pstrWas->size() : 0, (unsigned int)strNow.size(), (unsigned int)uiFirst, uiDiffBytes);
		FILE* pkOut = _wfopen(HstJoinPath(wstrDir, HstFromUtf8(akNow[i].first + ".bin")).c_str(), L"wb");
		if(pkOut)
		{
			fwrite(strNow.data(), 1, strNow.size(), pkOut);
			fclose(pkOut);
		}
	}
	HstLog("ckpt-verify[%s]: %d of %d segment(s) differ from the checkpoint", szLabel, iDiffering, (int)akNow.size());
}

bool HstSession::LoadGameImpl(const std::wstring& wstrPath, std::string& strError, bool bContinueRun, const HstCheckpointFile* pkImport,
	bool bHumansToAI)
{
	HstCheckpointFile kLocalFile;
	if(!pkImport)
	{
		HstDetHeapPauseScope kPause;
		if(!kLocalFile.Read(wstrPath, strError))
			return false;
	}
	const HstCheckpointFile& kFile = pkImport ? *pkImport : kLocalFile;
	const HstCheckpointFile::Section* pkHeader = kFile.Find("header");
	const HstCheckpointFile::Section* pkHost = kFile.Find("host");
	if(!pkHeader || !pkHost)
	{
		strError = "checkpoint has no header/host section";
		return false;
	}
	std::string strDllHash = HstCheckpointFile::HeaderValue(pkHeader->strData, "dll_xxh64");
	char szHash[32];
	_snprintf_s(szHash, sizeof(szHash), _TRUNCATE, "%016I64x", m_ui64DllHash);
	if(strDllHash != szHash)
		HstLog("checkpoint: WARNING written by another DLL (%s, this one %s)", strDllHash.c_str(), szHash);
	if(bContinueRun && m_bInGame)
	{
		// In-process reload: the exe tears the game down (contexts and all) and loads; the run's records continue.
		m_pkScriptSystem->ResetContexts();
		m_bInGame = false;
	}
	else
	{
		bContinueRun = false;
		if(m_bInGame)
			EndGameImpl();
		++m_iGameId;
		m_akTurns.clear();
		m_ui64MapDigest = 0;	// the map digest is taken after Init2 only; a loaded game has none
		HstRngLog::BeginGame(m_iGameId);
	}
	HstRngPhaseScope kRngPregame(-1);		// the load reseeds the RNGs (CvRandom::read): part of the pregame phase
	m_strScenarioName = HstCheckpointFile::HeaderValue(pkHeader->strData, "scenario");
	// CvGame::Read -> reset() deletes and recreates Civ5SavedGameDatabase.db, then writes the embedded copy back.
	m_pkScriptSystem->CloseSaveData();
	m_pkScriptSystem->SetVfs(m_pkMapVfs);
	m_pkEngine->ResetHostState();

	// Host state: virtual clock, GUID counter, loopback.
	{
		FMemoryStream kStream((int)pkHost->strData.size(), pkHost->strData.data());
		unsigned int uiMs = 0, uiGuids = 0;
		kStream.ReadIt(4, &uiMs);
		kStream.ReadIt(4, &uiGuids);
		HstVirtualTimeSet(uiMs);
		HstGuidCounterSet(uiGuids);
		if(!m_pkEngine->ReadHostState(kStream))
		{
			strError = "checkpoint host state is corrupt";
			return false;
		}
	}

	ICvGameContext1* pkCtx = GetContext();
	int iMismatches = 0;
	const double dStart = HstNowMs();
	// Exe Resync_LoadGameFromStream 0x10ae10: CacheGameDatabaseData, then Resync_LoadGameFromStreamImpl 0x2b6230.
	{
		std::auto_ptr<ICvDLLDatabaseUtility1> pkLoader(pkCtx->GetDatabaseLoadUtility());
		pkLoader->CacheGameDatabaseData();
	}
	std::auto_ptr<ICvPreGame1> pkPreGame(pkCtx->GetPreGame());
	{
		HstReadPreGame kReader = { pkPreGame.get() };
		HstReadSection(kFile, "pregame", kReader, iMismatches);
	}
	// 0x31bfe0: game, map, teams, players, RecalculateLandmasses, supporting data, Validate, LogGameState(true).
	std::auto_ptr<ICvGame1> pkGame(pkCtx->GetGame());
	std::auto_ptr<ICvMap1> pkMap(pkCtx->GetMap());
	{
		HstReadGame kReader = { pkGame.get() };
		HstReadSection(kFile, "game", kReader, iMismatches);
	}
	{
		HstReadMap kReader = { pkMap.get() };
		HstReadSection(kFile, "map", kReader, iMismatches);
	}
	char szName[32];
	for(int i = 0; i < MAX_TEAMS; ++i)
	{
		std::auto_ptr<ICvTeam1> pkTeam(pkCtx->GetTeam((TeamTypes)i));
		HstReadTeam kReader = { pkTeam.get() };
		_snprintf_s(szName, sizeof(szName), _TRUNCATE, "team%02d", i);
		HstReadSection(kFile, szName, kReader, iMismatches);
	}
	for(int i = 0; i < MAX_PLAYERS; ++i)
	{
		std::auto_ptr<ICvPlayer1> pkPlayer(pkGame->GetPlayer((PlayerTypes)i));
		HstReadPlayer kReader = { pkPlayer.get() };
		_snprintf_s(szName, sizeof(szName), _TRUNCATE, "player%02d", i);
		HstReadSection(kFile, szName, kReader, iMismatches);
	}
	HstWipeStack(s_uiHstWipeBytes);
	pkMap->RecalculateLandmasses();
	{
		HstReadSupporting kReader = { pkGame.get() };
		HstReadSection(kFile, "supporting", kReader, iMismatches);
	}
	if(kFile.IsBodyStream())
	{
		// The tail of a real save is zero padding (520..2288 bytes in the corpus); anything else means a Read consumed too little.
		const std::string& strBody = kFile.GetBody();
		unsigned int uiNonZero = 0;
		for(size_t i = kFile.GetBodyPos(); i < strBody.size(); ++i)
			uiNonZero += strBody[i] != 0 ? 1 : 0;
		HstLog("import: the DLL Reads consumed %u of %u body bytes; %u byte(s) left, %u of them non-zero", kFile.GetBodyPos(), (unsigned int)strBody.size(),
			(unsigned int)strBody.size() - kFile.GetBodyPos(), uiNonZero);
		if(uiNonZero)
			++iMismatches;
	}
	if(m_kOpt.bCheckpointVerify)
		HstCompareWithCheckpoint(pkCtx, kFile, "after-read");
	HstWipeStack(s_uiHstWipeBytes);
	pkMap->Validate();
	pkGame->LogGameState(true);
	// 0x2b6230 tail: re-set civilization/leader of slots 0..62 (rebinds the package keys).
	for(int i = 0; i < 63; ++i)
	{
		const PlayerTypes eSlot = (PlayerTypes)i;
		pkPreGame->setCivilization(eSlot, pkPreGame->civilization(eSlot));
		pkPreGame->setLeaderHead(eSlot, pkPreGame->leaderHead(eSlot));
	}
	// Save import: nobody can answer the turn handshake of a human slot, so SS_TAKEN becomes SS_COMPUTER after the Reads
	// (the DLL sees the real slots while it reads) and before the contexts / Lua events run.
	if(bHumansToAI)
	{
		int iConverted = 0;
		for(int i = 0; i < MAX_MAJOR_CIVS; ++i)
		{
			const PlayerTypes eSlot = (PlayerTypes)i;
			if(pkPreGame->slotStatus(eSlot) == SS_TAKEN)
			{
				pkPreGame->setSlotStatus(eSlot, SS_COMPUTER);
				++iConverted;
			}
		}
		HstLog("import: %d human slot(s) turned into AI (SS_COMPUTER)", iConverted);
	}
	// 0x10ae10 tail: hot-seat / single-player loads drop the MP turn modes; then (game started) GAME_LOADED,
	// InitScoreCalculation (+0xa0) and SetFinalInitialized(true) (+0x100).
	if(pkPreGame->isHotSeat() || pkPreGame->gameType() == GAME_SINGLE_PLAYER)
	{
		pkPreGame->SetGameOption("GAMEOPTION_SIMULTANEOUS_TURNS", 0);
		pkPreGame->SetGameOption("GAMEOPTION_DYNAMIC_TURNS", 0);
		pkPreGame->SetGameOption("GAMEOPTION_PITBOSS", 0);
	}
	if(pkPreGame->gameStarted())
	{
		pkPreGame->setGameStartType(GAME_LOADED);
		HstWipeStack(s_uiHstWipeBytes);
		pkGame->InitScoreCalculation();
		pkGame->SetFinalInitialized(true);
	}
	// Not serialized: the tactical analysis map (L5b). GUESS: the exe re-inits it during LoadGameState.
	m_pkRunner->InitTacticalMap();
	HstLog("checkpoint: loaded %s (turn %d, %.1f ms, %d section size mismatch(es))", pkImport ? "imported save" : HstToUtf8(wstrPath).c_str(),
		pkGame->GetGameTurn(), HstNowMs() - dStart, iMismatches);
	if(iMismatches)
		HstLog("checkpoint: WARNING some DLL Read calls did not consume their section exactly");

	if(m_kOpt.bCheckpointVerify)
		HstCompareWithCheckpoint(pkCtx, kFile, "after-postload");
	StartGame(NULL, true);
	if(m_kOpt.bCheckpointVerify)
		HstCompareWithCheckpoint(pkCtx, kFile, "after-start");
	if(!bContinueRun)
	{
		if(!OpenDigestFile())
		{
			strError = "cannot create the digest file";
			return false;
		}
		m_iStartTurn = GetGameTurn();
	}
	m_bInGame = true;
	return true;
}

static bool HstReadFileToString(const std::wstring& wstrPath, std::string& strOut)
{
	FILE* pkFile = _wfopen(wstrPath.c_str(), L"rb");
	if(!pkFile)
		return false;
	fseek(pkFile, 0, SEEK_END);
	const long lSize = ftell(pkFile);
	fseek(pkFile, 0, SEEK_SET);
	strOut.resize(lSize > 0 ? lSize : 0);
	const size_t uiRead = lSize > 0 ? fread(&strOut[0], 1, lSize, pkFile) : 0;
	fclose(pkFile);
	return (long)uiRead == lSize && lSize > 0;
}

bool HstSession::ImportSaveImpl(const std::wstring& wstrDir, bool bKeepHumans, std::string& strError)
{
	HstCheckpointFile kFile;
	{
		HstDetHeapPauseScope kPause;
		std::string strPregame, strBody;
		if(!HstReadFileToString(HstJoinPath(wstrDir, L"pregame.bin"), strPregame))
		{
			strError = "cannot read pregame.bin in " + HstToUtf8(wstrDir) + " (python -m civsim.saveio extract <save> --out <dir>)";
			return false;
		}
		if(!HstReadFileToString(HstJoinPath(wstrDir, L"body.bin"), strBody))
		{
			strError = "cannot read body.bin in " + HstToUtf8(wstrDir) + " (a save without a body is a game configuration, not a game)";
			return false;
		}
		std::string strName = HstToUtf8(wstrDir);
		while(!strName.empty() && (strName[strName.size() - 1] == '\\' || strName[strName.size() - 1] == '/'))
			strName.erase(strName.size() - 1);
		const size_t uiSlash = strName.find_last_of("\\/");
		if(uiSlash != std::string::npos)
			strName = strName.substr(uiSlash + 1);
		char szHeader[1024];
		_snprintf_s(szHeader, sizeof(szHeader), _TRUNCATE, "host=%s\ndll_xxh64=%016I64x\ndata_xxh64=%016I64x\nscenario=import:%s\n", HST_HOST_VERSION, m_ui64DllHash,
			m_ui64DataHash, strName.c_str());
		kFile.Add("header", szHeader, (unsigned int)strlen(szHeader));
		// Host state of a fresh game: virtual clock and GUID counter at their post-boot values, empty loopback.
		m_pkEngine->ResetHostState();
		FMemoryStream kHost;
		const unsigned int uiMs = m_uiBootVirtualMs;
		const unsigned int uiGuids = m_uiBootGuidCounter;
		kHost.WriteIt(4, &uiMs);
		kHost.WriteIt(4, &uiGuids);
		m_pkEngine->WriteHostState(kHost);
		kFile.Add("host", kHost);
		kFile.Add("pregame", strPregame.data(), (unsigned int)strPregame.size());
		HstLog("import: %s: pregame %u bytes, body %u bytes", strName.c_str(), (unsigned int)strPregame.size(), (unsigned int)strBody.size());
		// The mod version that wrote the save (envelope.json of the extractor, best effort): only saves of this DLL's own
		// version load (the corpus holds v34.15 and v33.11 saves; the older ones read past the end of the body).
		m_strImportGameName.clear();
		std::string strEnvelope;
		if(HstReadFileToString(HstJoinPath(wstrDir, L"envelope.json"), strEnvelope))
		{
			const size_t uiFields = strEnvelope.find("\"fields\"");		// the key also occurs in "spans" (as a name, without a string value)
			size_t uiAt = uiFields != std::string::npos ? strEnvelope.find("\"archive.game_name\"", uiFields) : std::string::npos;
			if(uiAt != std::string::npos)
			{
				uiAt = strEnvelope.find('"', uiAt + 19 + 1);			// opening quote of the value (skips the ':')
				const size_t uiEnd = uiAt != std::string::npos ? strEnvelope.find('"', uiAt + 1) : std::string::npos;
				if(uiEnd != std::string::npos)
					m_strImportGameName = strEnvelope.substr(uiAt + 1, uiEnd - uiAt - 1);
			}
		}
		HstLog("import: written by '%s'", m_strImportGameName.empty() ? "?" : m_strImportGameName.c_str());
		kFile.SetBodyStream(strBody);
	}
	try
	{
		return LoadGameImpl(std::wstring(), strError, false, &kFile, !bKeepHumans);
	}
	catch(...)
	{
		// FDataStream::Read past the end (C++ exception from the DLL): the stream layout of the save is not the one of this DLL.
		strError = "import failed: a DLL Read ran past the end of the save body, so the save was not written by this DLL's mod version (it says '" +
			(m_strImportGameName.empty() ? std::string("?") : m_strImportGameName) + "'; only saves of the DLL's own version load, v34.15 for the golden DLLs)";
		HstLog("%s", strError.c_str());
		m_bInGame = false;
		return false;
	}
}

bool HstSession::WriteCheckpointForTurn(int iTurn, std::string& strPath)
{
	std::wstring wstrPath;
	{
		HstDetHeapPauseScope kPause;
		std::wstring wstrDir = HstJoinPath(HstGetRunDir(), L"checkpoints");
		HstCreateDirs(wstrDir);
		wchar_t wszName[32];
		swprintf_s(wszName, L"turn-%04d.ckpt", iTurn);
		wstrPath = HstJoinPath(wstrDir, wszName);
		strPath = HstToUtf8(wstrPath);
	}
	unsigned int uiBytes = 0;
	HstHash64 ui64Hash = 0;
	std::string strError;
	if(!SaveGameImpl(wstrPath, uiBytes, ui64Hash, strError))
	{
		HstLog("checkpoint: FAILED at turn %d: %s", iTurn, strError.c_str());
		strPath.clear();
		return false;
	}
	return true;
}

// =====================================================================================================================
// Turns
// =====================================================================================================================
// Queries the control channel makes on top of the CLI flow run paused: the DLL's per-call interface wrappers are heap
// objects, and extra allocations would shift the deterministic heap layout between the two modes.
int HstSession::GetGameTurn()
{
	HstDetHeapPauseScope kPause;
	return m_pkRunner ? m_pkRunner->GetGameTurn() : 0;
}

int HstSession::GetNumPlots()
{
	HstDetHeapPauseScope kPause;
	std::auto_ptr<ICvMap1> pkMap(GetContext()->GetMap());
	return pkMap->NumPlots();
}

int HstSession::GetNumAlivePlayers()
{
	HstDetHeapPauseScope kPause;
	std::auto_ptr<ICvGame1> pkGame(GetContext()->GetGame());
	int iAlive = 0;
	for(int i = 0; i < MAX_PLAYERS; ++i)
	{
		std::auto_ptr<ICvPlayer1> pkPlayer(pkGame->GetPlayer((PlayerTypes)i));
		if(pkPlayer.get() && pkPlayer->IsAlive())
			++iAlive;
	}
	return iAlive;
}

HstHash64 HstSession::ComputeCombinedDigest()
{
	HstDetHeapPauseScope kPause;	// Write() is const; see GetGameTurn
	if(!m_pkDigest)
		m_pkDigest = new HstDigest(GetContext());
	std::vector<HstHash64> aui64Hashes;
	unsigned int uiBytes = 0;
	m_pkDigest->Compute(aui64Hashes, std::wstring(), uiBytes);
	return HstDigest::Combine(aui64Hashes);
}

struct HstSliceThunk
{
	static void Call(void* pUser) { static_cast<HstTurnListener*>(pUser)->OnSlice(); }
};

HstRunResult HstSession::RunImpl(const HstRunRequest& kReq, HstTurnListener* pkListener)
{
	HstRunResult kResult;
	kResult.iStartTurn = GetGameTurn();
	kResult.iEndTurn = kResult.iStartTurn;
	kResult.iTurnsDone = 0;
	kResult.dTotalMs = 0.0;
	kResult.iExitCode = 0;
	kResult.strReason = "done";
	if(!m_bInGame)
	{
		kResult.strReason = "no_game";
		return kResult;
	}
	const int iTurns = kReq.iUntilTurn >= 0 ? std::max(0, kReq.iUntilTurn - kResult.iStartTurn) : kReq.iTurns;
	std::vector<HstHash64> aui64Hashes;
	if(kReq.iDumpStateAt == kResult.iStartTurn)
	{
		std::wstring wstrDump;
		{
			HstDetHeapPauseScope kPause;
			wchar_t wszDir[32];
			swprintf_s(wszDir, L"state-%d", kResult.iStartTurn);
			wstrDump = HstJoinPath(HstGetRunDir(), wszDir);
		}
		unsigned int uiBytes = 0;
		m_pkDigest->Compute(aui64Hashes, wstrDump, uiBytes);
		HstLog("turns: state at turn %d dumped (%u bytes), digest %016I64x", kResult.iStartTurn, uiBytes, HstDigest::Combine(aui64Hashes));
	}
	HstLog("turns: running %d turn(s) from turn %d (max %d slices per turn, digest every %d, checkpoint every %d)", iTurns, kResult.iStartTurn,
		m_kOpt.iMaxSlices, kReq.iDigestEvery, kReq.iCheckpointEvery);
	const int iLuaErrorsStart = m_pkScriptSystem->GetErrorCount();
	for(int i = 0; i < iTurns; ++i)
	{
		HstTurnRecord kRecord;
		kRecord.bDigest = false;
		kRecord.dDigestMs = 0.0;
		kRecord.uiDigestBytes = 0;
		kRecord.ui64Combined = 0;
		kRecord.kTurn = m_pkRunner->RunTurn(m_pkEngine, m_kOpt.iMaxSlices, pkListener ? &HstSliceThunk::Call : NULL, pkListener);
		if(kRecord.kTurn.bStuck)
		{
			HstLog("turns: STUCK: turn %d did not advance within %u slices", kRecord.kTurn.iFromTurn, kRecord.kTurn.uiSlices);
			kResult.strReason = "stuck";
			kResult.iExitCode = 7;
			break;
		}
		if(kRecord.kTurn.uiFpuWord != m_uiFpuStart)
			HstLog("turns: WARNING x87 control word changed during turn %d: 0x%04x (start 0x%04x)", kRecord.kTurn.iToTurn, kRecord.kTurn.uiFpuWord, m_uiFpuStart);
		const int iTurn = kRecord.kTurn.iToTurn;
		std::wstring wstrDump;
		if(kReq.iDumpStateAt == iTurn)
		{
			HstDetHeapPauseScope kPause;
			wchar_t wszDir[32];
			swprintf_s(wszDir, L"state-%d", iTurn);
			wstrDump = HstJoinPath(HstGetRunDir(), wszDir);
		}
		if((kReq.iDigestEvery > 0 && iTurn % kReq.iDigestEvery == 0) || !wstrDump.empty())
		{
			const double dStart = HstNowMs();
			m_pkDigest->Compute(aui64Hashes, wstrDump, kRecord.uiDigestBytes);
			kRecord.dDigestMs = HstNowMs() - dStart;
			kRecord.ui64Combined = HstDigest::Combine(aui64Hashes);
			kRecord.bDigest = true;
			HstDetHeapPauseScope kPause;
			m_pkDigest->AppendRecord((unsigned int)iTurn, aui64Hashes);
		}
		bool bReload = false;
		if(kReq.iCheckpointEvery > 0 && iTurn % kReq.iCheckpointEvery == 0)
			bReload = WriteCheckpointForTurn(iTurn, kRecord.strCheckpoint) && kReq.bReloadCheckpoints;
		{
			HstDetHeapPauseScope kPause;
			m_akTurns.push_back(kRecord);
			HstLog("turn %3d: %5u slices %8.1f ms  net %d  digest %016I64x (%u KB, %.1f ms)%s%s", iTurn, kRecord.kTurn.uiSlices, kRecord.kTurn.dMs,
				kRecord.kTurn.iNetMessages, kRecord.ui64Combined, kRecord.uiDigestBytes / 1024, kRecord.dDigestMs, wstrDump.empty() ? "" : "  [state dumped]",
				kRecord.strCheckpoint.empty() ? "" : "  [checkpoint]");
		}
		if(pkListener)
			pkListener->OnTurn(kRecord);
		kResult.dTotalMs += kRecord.kTurn.dMs;
		kResult.iEndTurn = iTurn;
		++kResult.iTurnsDone;
		if(kReq.bStopOnLuaError && m_pkScriptSystem->GetErrorCount() != iLuaErrorsStart)
		{
			kResult.strReason = "lua_error";
			break;
		}
		if(bReload)
		{
			// The public Run() reloads at the fixed stack position and continues.
			HstDetHeapPauseScope kPause;
			m_strPendingReload = kRecord.strCheckpoint;
			kResult.strReason = "reload";
			break;
		}
	}
	HstLog("turns: %d/%d turn(s) completed in %.1f ms (mean %.1f ms/turn), %d net message(s)", kResult.iTurnsDone, iTurns, kResult.dTotalMs,
		kResult.iTurnsDone ? kResult.dTotalMs / kResult.iTurnsDone : 0.0, m_pkEngine->GetNumNetMessagesDelivered());
	if(HstDetHeapIsInstalled())
	{
		HstLog("detheap: %u MB used, %u MB live, %u allocations", (unsigned int)(HstDetHeapGetUsedBytes() >> 20), (unsigned int)(HstDetHeapGetLiveBytes() >> 20),
			HstDetHeapGetAllocCount());
		unsigned int uiForeignAllocs = 0, uiForeignFrees = 0, uiForeignReallocs = 0;
		HstDetHeapGetForeignCounts(uiForeignAllocs, uiForeignFrees, uiForeignReallocs);
		HstLog("detheap: %u fresh-block allocations; foreign threads: %u allocs, %u frees dropped, %u reallocs", HstDetHeapGetBumpCount(), uiForeignAllocs,
			uiForeignFrees, uiForeignReallocs);
	}
	return kResult;
}

// Lockstep peer frame. Mirrors RunTurn + the per-turn tail of RunImpl, one slice per call: the game wrapper is taken at the
// first frame of a turn and released after the turn changed (RunTurn holds one per turn), the RNG phase spans the turn.
bool HstSession::StepImpl(const std::vector<HstNetMessage>& akInbox, const HstStepRequest& kReq, HstStepResult& kResult, std::string& strError)
{
	if(!m_bInGame)
	{
		strError = "no game (new_game first)";
		return false;
	}
	if(!m_pkStepGame)
	{
		m_pkStepGame = GetContext()->GetGame();
		memset(&m_kStepTurn, 0, sizeof(m_kStepTurn));
		m_kStepTurn.iFromTurn = m_pkStepGame->GetGameTurn();
		m_kStepTurn.iToTurn = m_kStepTurn.iFromTurn;
		HstRngLog::BeginPhase(m_kStepTurn.iFromTurn);
	}
	if(kReq.iGrantTurn >= 0)
		m_pkEngine->GrantAdvance(kReq.iGrantTurn);
	const double dMs = m_pkRunner->RunSlice(m_pkStepGame, m_pkEngine, akInbox);
	++m_iFrame;
	++m_kStepTurn.uiSlices;
	m_kStepTurn.dMs += dMs;
	m_kStepTurn.iNetMessages += (int)akInbox.size();
	const int iTurn = m_pkStepGame->GetGameTurn();
	kResult.iFrame = m_iFrame;
	kResult.iFromTurn = m_kStepTurn.iFromTurn;
	kResult.iTurn = iTurn;
	kResult.bTurnChanged = iTurn != m_kStepTurn.iFromTurn;
	kResult.dMs = dMs;
	kResult.kRecord.bDigest = false;
	kResult.kRecord.dDigestMs = 0.0;
	kResult.kRecord.uiDigestBytes = 0;
	kResult.kRecord.ui64Combined = 0;
	if(!kResult.bTurnChanged)
		return true;

	m_kStepTurn.iToTurn = iTurn;
	m_kStepTurn.bStuck = false;
	m_kStepTurn.uiFpuWord = HstGetFpuControlWord();
	HstRngLog::EndPhase();
	delete m_pkStepGame;
	m_pkStepGame = NULL;
	if(m_kStepTurn.uiFpuWord != m_uiFpuStart)
		HstLog("turns: WARNING x87 control word changed during turn %d: 0x%04x (start 0x%04x)", iTurn, m_kStepTurn.uiFpuWord, m_uiFpuStart);

	HstTurnRecord& kRecord = kResult.kRecord;
	kRecord.kTurn = m_kStepTurn;
	std::vector<HstHash64> aui64Hashes;
	std::wstring wstrDump;
	if(kReq.iDumpStateAt == iTurn)
	{
		HstDetHeapPauseScope kPause;
		wchar_t wszDir[32];
		swprintf_s(wszDir, L"state-%d", iTurn);
		wstrDump = HstJoinPath(HstGetRunDir(), wszDir);
	}
	if((kReq.iDigestEvery > 0 && iTurn % kReq.iDigestEvery == 0) || !wstrDump.empty())
	{
		const double dStart = HstNowMs();
		m_pkDigest->Compute(aui64Hashes, wstrDump, kRecord.uiDigestBytes);
		kRecord.dDigestMs = HstNowMs() - dStart;
		kRecord.ui64Combined = HstDigest::Combine(aui64Hashes);
		kRecord.bDigest = true;
		HstDetHeapPauseScope kPause;
		m_pkDigest->AppendRecord((unsigned int)iTurn, aui64Hashes);
	}
	if(kRecord.bDigest && HstDigest::HasMask())
	{
		// Masked digest (mask-only UI state left out); host-side bookkeeping only, the DLL just serializes again.
		std::wstring wstrMaskDump;
		if(!wstrDump.empty())
			wstrMaskDump = wstrDump + L"-masked";
		std::vector<HstHash64> aui64Masked;
		unsigned int uiBytes = 0;
		{
			HstDetHeapPauseScope kPause;	// the masked serialization is control-only work (see GetGameTurn)
			m_pkDigest->ComputeMasked(aui64Masked, wstrMaskDump, uiBytes, kResult.uiMaskedBytes);
			kResult.ui64Masked = HstDigest::Combine(aui64Masked);
			kResult.bMasked = true;
			if(!m_pkMaskDigestFile)
			{
				m_pkMaskDigestFile = new HstDigest(GetContext());
				m_pkMaskDigestFile->OpenFile(HstJoinPath(HstGetRunDir(), L"digest-masked.bin"));
			}
			m_pkMaskDigestFile->AppendRecord((unsigned int)iTurn, aui64Masked);
		}
	}
	HstDetHeapPauseScope kPause;
	m_akTurns.push_back(kRecord);
	HstLog("turn %3d: %5u frames %8.1f ms  net in %d  digest %016I64x (%u KB, %.1f ms)%s [peer %d]%s", iTurn, kRecord.kTurn.uiSlices, kRecord.kTurn.dMs,
		kRecord.kTurn.iNetMessages, kRecord.ui64Combined, kRecord.uiDigestBytes / 1024, kRecord.dDigestMs, wstrDump.empty() ? "" : "  [state dumped]",
		m_pkEngine->GetPeerConfig().iPeerId, kResult.bMasked ? " masked" : "");
	return true;
}

bool HstSession::WriteDigest(const std::string& strLevel, const std::wstring& wstrPath, HstHash64& ui64Combined, std::string& strError)
{
	if(!m_bInGame)
	{
		strError = "no game";
		return false;
	}
	HstDetHeapPauseScope kPause;	// control-only work; see GetGameTurn
	std::vector<HstHash64> aui64Hashes;
	unsigned int uiBytes = 0;
	std::wstring wstrDump;
	if(strLevel == "full")
		wstrDump = wstrPath;
	m_pkDigest->Compute(aui64Hashes, wstrDump, uiBytes);
	ui64Combined = HstDigest::Combine(aui64Hashes);
	if(strLevel == "full")
		return true;
	if(strLevel == "subsystem")
	{
		HstDigest kOne(GetContext());
		if(!kOne.OpenFile(wstrPath))
		{
			strError = "cannot create " + HstToUtf8(wstrPath);
			return false;
		}
		kOne.AppendRecord((unsigned int)m_pkRunner->GetGameTurn(), aui64Hashes);
		kOne.CloseFile();
		return true;
	}
	FILE* pkFile = _wfopen(wstrPath.c_str(), L"w");
	if(!pkFile)
	{
		strError = "cannot create " + HstToUtf8(wstrPath);
		return false;
	}
	fprintf(pkFile, "%d %016I64x\n", m_pkRunner->GetGameTurn(), ui64Combined);
	fclose(pkFile);
	return true;
}

// =====================================================================================================================
// Reports
// =====================================================================================================================
void HstSession::WriteRunJson(const std::string& strStatus)
{
	HstDetHeapPauseScope kPause;	// host-only output (paths, timings)
	FILE* pkFile = _wfopen(HstJoinPath(HstGetRunDir(), L"run.json").c_str(), L"w");
	if(!pkFile)
		return;
	double dTotal = 0.0, dMax = 0.0;
	unsigned int uiSlices = 0;
	HstHash64 ui64Final = 0;
	for(size_t i = 0; i < m_akTurns.size(); ++i)
	{
		dTotal += m_akTurns[i].kTurn.dMs;
		dMax = std::max(dMax, m_akTurns[i].kTurn.dMs);
		uiSlices += m_akTurns[i].kTurn.uiSlices;
		if(m_akTurns[i].bDigest)
			ui64Final = m_akTurns[i].ui64Combined;
	}
	int iLoaded = 0;
	const std::vector<HstScriptSystem::ContextInfo>& akContexts = m_pkScriptSystem->GetContexts();
	for(size_t i = 0; i < akContexts.size(); ++i)
		iLoaded += akContexts[i].bLoaded ? 1 : 0;
	fprintf(pkFile, "{\n");
	fprintf(pkFile, "  \"host\": \"%s\",\n", HST_HOST_VERSION);
	fprintf(pkFile, "  \"status\": \"%s\",\n", strStatus.c_str());
	fprintf(pkFile, "  \"game_id\": %d,\n", m_iGameId);
	fprintf(pkFile, "  \"scenario\": \"%s\",\n", HstJsonEscape(m_strScenarioName).c_str());
	fprintf(pkFile, "  \"dll\": \"%s\",\n", HstJsonEscape(HstToUtf8(m_kOpt.wstrDll)).c_str());
	fprintf(pkFile, "  \"dll_xxh64\": \"%016I64x\",\n", m_ui64DllHash);
	fprintf(pkFile, "  \"data\": \"%s\",\n", HstJsonEscape(HstToUtf8(m_kOpt.wstrData)).c_str());
	fprintf(pkFile, "  \"data_xxh64\": \"%016I64x\",\n", m_ui64DataHash);
	fprintf(pkFile, "  \"lua_overlay\": \"%s\",\n", HstJsonEscape(HstToUtf8(m_kOpt.wstrLuaOverlay)).c_str());
	fprintf(pkFile, "  \"lua_contexts\": %d,\n", (int)m_astrContextNames.size());
	fprintf(pkFile, "  \"lua_contexts_loaded\": %d,\n", iLoaded);
	fprintf(pkFile, "  \"turns_completed\": %d,\n", (int)m_akTurns.size());
	fprintf(pkFile, "  \"start_turn\": %d,\n", m_iStartTurn);
	fprintf(pkFile, "  \"end_turn\": %d,\n", m_akTurns.empty() ? m_iStartTurn : m_akTurns.back().kTurn.iToTurn);
	std::string strDigest = HstToUtf8(m_wstrDigestPath);
	size_t uiSlash = strDigest.find_last_of("\\/");
	fprintf(pkFile, "  \"digest\": \"%s\",\n", HstJsonEscape(uiSlash == std::string::npos ? strDigest : strDigest.substr(uiSlash + 1)).c_str());
	fprintf(pkFile, "  \"digest_segments\": %d,\n", m_pkDigest ? m_pkDigest->GetNumSegments() : 0);
	fprintf(pkFile, "  \"final_digest\": \"%016I64x\",\n", ui64Final);
	fprintf(pkFile, "  \"map_digest\": \"%016I64x\",\n", m_ui64MapDigest);
	fprintf(pkFile, "  \"launch_ms\": %.1f,\n", m_dLaunchMs);
	fprintf(pkFile, "  \"total_turn_ms\": %.1f,\n", dTotal);
	fprintf(pkFile, "  \"mean_turn_ms\": %.2f,\n", m_akTurns.empty() ? 0.0 : dTotal / m_akTurns.size());
	fprintf(pkFile, "  \"max_turn_ms\": %.2f,\n", dMax);
	fprintf(pkFile, "  \"total_slices\": %u,\n", uiSlices);
	fprintf(pkFile, "  \"net_messages\": %d,\n", m_pkEngine->GetNumNetMessagesDelivered());
	fprintf(pkFile, "  \"lua_errors\": %d,\n", m_pkScriptSystem->GetErrorCount());
	if(m_kOpt.HasPerturbation() || m_kOpt.bFpuCheck)
	{
		fprintf(pkFile, "  \"perturbation\": {\"heap_fill\": %d, \"stack_fill\": %d, \"heap_shift\": %u, \"heap_jitter\": %u, \"fpu_pc\": %d, \"fpu_pc_global\": %d, "
			"\"fpu_check\": %s, \"fpu_changes\": %d, \"fpu_cw_final\": %u, \"source\": \"%s\"},\n", m_kOpt.iHeapFill, m_kOpt.iStackFill, m_kOpt.uiHeapShift,
			m_kOpt.uiHeapJitter, m_kOpt.iFpuPc, m_kOpt.iFpuPcGlobal, m_kOpt.bFpuCheck ? "true" : "false", HstFpuGetChangeCount(), HstGetFpuControlWord(),
			HstJsonEscape(m_kOpt.strPerturbNote).c_str());
	}
	fprintf(pkFile, "  \"turns\": [\n");
	for(size_t i = 0; i < m_akTurns.size(); ++i)
	{
		const HstTurnRecord& k = m_akTurns[i];
		char szDigest[32];
		if(k.bDigest)
			_snprintf_s(szDigest, sizeof(szDigest), _TRUNCATE, "\"%016I64x\"", k.ui64Combined);
		else
			strcpy_s(szDigest, "null");
		fprintf(pkFile, "    {\"turn\": %d, \"slices\": %u, \"ms\": %.2f, \"digest_ms\": %.2f, \"state_bytes\": %u, \"net\": %d, \"digest\": %s%s}%s\n",
			k.kTurn.iToTurn, k.kTurn.uiSlices, k.kTurn.dMs, k.dDigestMs, k.uiDigestBytes, k.kTurn.iNetMessages, szDigest,
			k.strCheckpoint.empty() ? "" : ", \"checkpoint\": true", i + 1 < m_akTurns.size() ? "," : "");
	}
	fprintf(pkFile, "  ]\n}\n");
	fclose(pkFile);
}

void HstSession::WriteReports()
{
	if(m_pkScriptSystem)
	{
		std::wstring wstrName;
		{
			HstDetHeapPauseScope kPause;
			wchar_t wszName[48];
			if(m_iGameId <= 1)
				wcscpy_s(wszName, L"events-registered.json");
			else
				swprintf_s(wszName, L"events-registered-%d.json", m_iGameId);
			wstrName = HstJoinPath(HstGetRunDir(), wszName);
		}
		m_pkScriptSystem->WriteEventsRegistered(wstrName);
	}
	HstRngLog::Flush();
	HstDetHeapPauseScope kPause;
	FILE* pkCalls = _wfopen(HstJoinPath(HstGetRunDir(), L"api-calls.tsv").c_str(), L"w");
	if(pkCalls)
	{
		HstApiDumpCounters(pkCalls, true);
		fclose(pkCalls);
	}
}

// =====================================================================================================================
// Public entry points: every game entry runs at the fixed stack position (HstRunAtFixedStack), so the DLL sees the
// same stack addresses from the CLI, the control channel and an in-process reload.
// =====================================================================================================================
struct HstSessionThunks
{
	struct BotArgs { HstSession* p; int iFrame; const char* szMode; std::string* pstrOut; };
	static void Bot(void* pv) { BotArgs* a = (BotArgs*)pv; *a->pstrOut = a->p->BotTickImpl(a->iFrame, a->szMode); }
	struct BootArgs { HstSession* p; bool bStop; int iResult; };
	static void Boot(void* pv) { BootArgs* a = (BootArgs*)pv; a->iResult = a->p->BootEngineImpl(a->bStop); }

	struct NewGameArgs { HstSession* p; HstScenario* pkScenario; std::string* pstrError; bool bResult; };
	static void NewGame(void* pv) { NewGameArgs* a = (NewGameArgs*)pv; a->bResult = a->p->NewGameImpl(*a->pkScenario, *a->pstrError); }

	struct LoadArgs { HstSession* p; const std::wstring* pwstrPath; std::string* pstrError; bool bContinue; bool bResult; };
	static void Load(void* pv) { LoadArgs* a = (LoadArgs*)pv; a->bResult = a->p->LoadGameImpl(*a->pwstrPath, *a->pstrError, a->bContinue); }

	struct ImportArgs { HstSession* p; const std::wstring* pwstrDir; bool bKeep; std::string* pstrError; bool bResult; };
	static void Import(void* pv) { ImportArgs* a = (ImportArgs*)pv; a->bResult = a->p->ImportSaveImpl(*a->pwstrDir, a->bKeep, *a->pstrError); }

	struct SaveArgs { HstSession* p; const std::wstring* pwstrPath; unsigned int* puiBytes; HstHash64* pui64Hash; std::string* pstrError; bool bResult; };
	static void Save(void* pv) { SaveArgs* a = (SaveArgs*)pv; a->bResult = a->p->SaveGameImpl(*a->pwstrPath, *a->puiBytes, *a->pui64Hash, *a->pstrError); }

	struct RunArgs { HstSession* p; const HstRunRequest* pkReq; HstTurnListener* pkListener; HstRunResult kResult; };
	static void Run(void* pv) { RunArgs* a = (RunArgs*)pv; a->kResult = a->p->RunImpl(*a->pkReq, a->pkListener); }

	struct EndArgs { HstSession* p; };
	static void End(void* pv) { ((EndArgs*)pv)->p->EndGameImpl(); }

	struct StepArgs { HstSession* p; const std::vector<HstNetMessage>* pakInbox; const HstStepRequest* pkReq; HstStepResult* pkResult; std::string* pstrError; bool bResult; };
	static void Step(void* pv) { StepArgs* a = (StepArgs*)pv; a->bResult = a->p->StepImpl(*a->pakInbox, *a->pkReq, *a->pkResult, *a->pstrError); }
};

bool HstSession::Step(const std::vector<HstNetMessage>& akInbox, const HstStepRequest& kReq, HstStepResult& kResult, std::string& strError)
{
	HstSessionThunks::StepArgs a = { this, &akInbox, &kReq, &kResult, &strError, false };
	HstRunAtFixedStack(&HstSessionThunks::Step, &a);
	return a.bResult;
}

int HstSession::BootEngine(bool bStopAfterLoad)
{
	HstLog("stack   : game entry points run at 0x%08x", HstFixedStackGet());
	HstSessionThunks::BootArgs a = { this, bStopAfterLoad, 1 };
	HstRunAtFixedStack(&HstSessionThunks::Boot, &a);
	return a.iResult;
}

bool HstSession::RefuseWarmStart(bool bContinueRun, std::string& strError) const
{
	if(m_iGameId < 1 || m_kOpt.bAllowWarm)
		return false;
	if(bContinueRun && m_bInGame)
		return false;		// in-process reload of the running game (--reload-checkpoints): the same game, not a second one
	char szText[640];
	_snprintf_s(szText, sizeof(szText), _TRUNCATE,
		"refused: this process already hosted game %d. The gamecore DLL is not reloaded between games, so a second game starts from "
		"the state the first one left in the DLL (player/team streams grow) and does not reproduce the same "
		"game in a fresh process (determinism class C8). Start one civhost process per game, or pass --allow-warm to accept the warm start.",
		m_iGameId);
	strError = szText;
	HstLog("session: %s", szText);
	return true;
}

bool HstSession::NewGame(HstScenario& kScenario, std::string& strError)
{
	if(RefuseWarmStart(false, strError))
		return false;
	HstSessionThunks::NewGameArgs a = { this, &kScenario, &strError, false };
	HstRunAtFixedStack(&HstSessionThunks::NewGame, &a);
	return a.bResult;
}

bool HstSession::LoadGame(const std::wstring& wstrPath, std::string& strError, bool bContinueRun)
{
	if(RefuseWarmStart(bContinueRun, strError))
		return false;
	HstSessionThunks::LoadArgs a = { this, &wstrPath, &strError, bContinueRun, false };
	HstRunAtFixedStack(&HstSessionThunks::Load, &a);
	return a.bResult;
}

bool HstSession::ImportSave(const std::wstring& wstrDir, bool bKeepHumans, std::string& strError)
{
	if(RefuseWarmStart(false, strError))
		return false;
	HstSessionThunks::ImportArgs a = { this, &wstrDir, bKeepHumans, &strError, false };
	HstRunAtFixedStack(&HstSessionThunks::Import, &a);
	return a.bResult;
}

bool HstSession::SaveGame(const std::wstring& wstrPath, unsigned int& uiBytes, HstHash64& ui64Hash, std::string& strError)
{
	HstSessionThunks::SaveArgs a = { this, &wstrPath, &uiBytes, &ui64Hash, &strError, false };
	HstRunAtFixedStack(&HstSessionThunks::Save, &a);
	return a.bResult;
}

void HstSession::EndGame()
{
	HstSessionThunks::EndArgs a = { this };
	HstRunAtFixedStack(&HstSessionThunks::End, &a);
}

HstRunResult HstSession::Run(const HstRunRequest& kReq, HstTurnListener* pkListener)
{
	HstRunRequest kPart = kReq;
	HstRunResult kTotal;
	bool bFirst = true;
	for(;;)
	{
		HstSessionThunks::RunArgs a;
		a.p = this;
		a.pkReq = &kPart;
		a.pkListener = pkListener;
		HstRunAtFixedStack(&HstSessionThunks::Run, &a);
		if(bFirst)
		{
			kTotal = a.kResult;
			bFirst = false;
		}
		else
		{
			kTotal.iEndTurn = a.kResult.iEndTurn;
			kTotal.iTurnsDone += a.kResult.iTurnsDone;
			kTotal.dTotalMs += a.kResult.dTotalMs;
			kTotal.strReason = a.kResult.strReason;
			kTotal.iExitCode = a.kResult.iExitCode;
		}
		if(a.kResult.strReason != "reload")
			break;
		// In-process reload of the checkpoint just written (bReloadCheckpoints), then continue with the rest.
		std::wstring wstrPath;
		std::string strError;
		{
			HstDetHeapPauseScope kPause;
			wstrPath = HstFromUtf8(m_strPendingReload);
			m_strPendingReload.clear();
		}
		if(!LoadGame(wstrPath, strError, true))
		{
			HstLog("turns: reload of %s failed: %s", HstToUtf8(wstrPath).c_str(), strError.c_str());
			kTotal.strReason = "reload_failed";
			kTotal.iExitCode = 2;
			break;
		}
		kTotal.strReason = "done";
		if(kPart.iUntilTurn < 0)
		{
			kPart.iTurns -= a.kResult.iTurnsDone;
			if(kPart.iTurns <= 0)
				break;
		}
		else if(GetGameTurn() >= kPart.iUntilTurn)
			break;
		kPart.iDumpStateAt = kReq.iDumpStateAt == GetGameTurn() ? -1 : kReq.iDumpStateAt;	// no second dump of the reload turn
	}
	return kTotal;
}

// ---- lockstep bot ------------------------------------------------------------------------------------------------------

bool HstSession::BotLoad(const std::wstring& wstrFile, const std::string& strInit, std::string& strError)
{
	HstDetHeapPauseScope kPause;	// the bot is the local UI of one peer: it must not shift the deterministic layout
	if(!m_bInGame || !m_pkScriptSystem)
	{
		strError = "no game (new_game first)";
		return false;
	}
	if(!m_pkBotThread)
		m_pkBotThread = m_pkScriptSystem->CreateLuaThread("HarnessBot");
	if(!m_pkBotThread)
	{
		strError = "cannot create the bot Lua thread";
		return false;
	}
	if(!strInit.empty() && !m_pkScriptSystem->RunString(m_pkBotThread, strInit.c_str(), "=bot init"))
	{
		strError = "bot init chunk failed (see lua.log)";
		return false;
	}
	if(!m_pkScriptSystem->RunFile(m_pkBotThread, wstrFile, "@bot"))
	{
		strError = "bot script failed (see lua.log)";
		return false;
	}
	HstLog("bot: loaded %s", HstToUtf8(wstrFile).c_str());
	return true;
}

std::string HstSession::BotTickImpl(int iFrame, const char* szMode)
{
	HstDetHeapPauseScope kPause;
	lua_State* L = m_pkBotThread;
	if(!L)
		return std::string();
	const int iTop = lua_gettop(L);
	lua_getglobal(L, "HarnessBotTick");
	if(!lua_isfunction(L, -1))
	{
		lua_settop(L, iTop);
		return std::string();
	}
	lua_pushinteger(L, iFrame);
	lua_pushstring(L, szMode ? szMode : "play");
	HstWipeStack(s_uiHstWipeBytes);
	std::string strOut;
	if(lua_pcall(L, 2, 1, 0) != 0)
	{
		const char* szErr = lua_tostring(L, -1);
		m_pkScriptSystem->ReportError("HarnessBotTick", szErr ? szErr : "?");
		strOut = std::string("error: ") + (szErr ? szErr : "?");
	}
	else if(lua_isstring(L, -1))
		strOut = lua_tostring(L, -1);
	lua_settop(L, iTop);
	HstWipeStack(s_uiHstWipeBytes);
	return strOut;
}

std::string HstSession::BotTick(int iFrame, const char* szMode)
{
	std::string strOut;
	HstSessionThunks::BotArgs a = { this, iFrame, szMode, &strOut };
	HstRunAtFixedStack(&HstSessionThunks::Bot, &a);
	return strOut;
}