//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstDllHost.cpp
//!  \brief     D1-D2 (exe LoadGameCoreDll RVA 0x81730) and I1-I8 (exe ReloadGameCoreDLL RVA 0x7b760 / 0x7b1d0,
//!				SetActiveDLCandMods RVA 0x2b8e50).
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#include "HstCommon.h"
#include "dllhost/HstDllHost.h"
#include "engine/HstEngineUtility.h"
#include "engine/HstUserInterface.h"
#include "data/HstDatabase.h"
#include "platform/HstPlatform.h"
#include "platform/HstDetHeap.h"

typedef ICvGameContext1* (*PFN_DllGetGameContext)();

// Game binaries the gamecore DLL imports, in dependency order (approved read-only loads).
static const wchar_t* s_awszGameDlls[] =
{
	L"lua51_Win32.dll",
	L"CvGameDatabaseWin32Final Release.dll",
	L"CvLocalizationWin32Final Release.dll",
};

static std::wstring HstModulePath(HMODULE hModule)
{
	wchar_t wszPath[MAX_PATH];
	DWORD dwLen = GetModuleFileNameW(hModule, wszPath, MAX_PATH);
	return std::wstring(wszPath, dwLen);
}

HstDllHost::HstDllHost()
	: m_hDll(NULL)
	, m_pkCtx(NULL)
	, m_pkCtx3(NULL)
{
}

bool HstDllHost::Load(const std::wstring& wstrDllPath)
{
	// Load the dependencies explicitly first so their IATs are sandboxed before the gamecore DLL runs any code.
	for(size_t i = 0; i < HST_ARRAY_SIZE(s_awszGameDlls); ++i)
	{
		std::wstring wstrPath = HstJoinPath(HstGetGameDir(), s_awszGameDlls[i]);
		HMODULE hModule = LoadLibraryW(wstrPath.c_str());
		if(!hModule)
		{
			HstLog("dll: LoadLibrary(%s) failed, error %u", HstToUtf8(wstrPath).c_str(), (unsigned int)GetLastError());
			return false;
		}
		int iPatched = HstSandboxPatchModule(hModule);
		int iHeap = HstDetHeapPatchModule(hModule) + HstRelativePathPatchModule(hModule);
		HstLog("dll: loaded %s (sandbox: %d IAT slots, heap: %d)", HstToUtf8(HstModulePath(hModule)).c_str(), iPatched, iHeap);
	}

	// D1. The DllMain of the gamecore DLL creates the context singleton (CvGameCoreDLL.cpp:42).
	std::wstring wstrFull = HstFullPath(wstrDllPath);
	m_hDll = LoadLibraryW(wstrFull.c_str());
	if(!m_hDll)
	{
		HstLog("dll: LoadLibrary(%s) failed, error %u", HstToUtf8(wstrFull).c_str(), (unsigned int)GetLastError());
		return false;
	}
	int iPatched = HstSandboxPatchModule(m_hDll);
	// Virtual clocks + deterministic CoCreateGuid (the map GUID is serialized by CvMap::Write) + deterministic heap
	// (MSVCR90 is patched in HstRun before anything from the game loads; this covers the DLL's private HeapCreate heap).
	int iDeterminism = HstDeterminismPatchModule(m_hDll) + HstDetHeapPatchModule(m_hDll);
	HstLog("dll: loaded %s at 0x%08x (sandbox: %d IAT slots, determinism: %d)", HstToUtf8(HstModulePath(m_hDll)).c_str(),
		(unsigned int)(DWORD_PTR)m_hDll, iPatched, iDeterminism);

	PFN_DllGetGameContext pfnGetContext = (PFN_DllGetGameContext)GetProcAddress(m_hDll, "DllGetGameContext");
	if(!pfnGetContext)
	{
		HstLog("dll: GetProcAddress('DllGetGameContext') failed");
		return false;
	}
	m_pkCtx = pfnGetContext();
	if(!m_pkCtx)
	{
		HstLog("dll: DllGetGameContext returned NULL");
		return false;
	}
	// D2. QI returns this (CvDllContext.cpp:99-110).
	m_pkCtx3 = m_pkCtx->QueryInterface<ICvGameContext3>();
	if(!m_pkCtx3)
	{
		HstLog("dll: QueryInterface(ICvGameContext3) failed");
		return false;
	}
	return true;
}

std::string HstDllHost::GetVersion() const
{
	const char* sz = m_pkCtx ? m_pkCtx->GetDLLVersion() : NULL;
	return sz ? std::string(sz) : std::string("?");
}

bool HstDllHost::Init(HstEngineUtility* pkEngine, HstUserInterface* pkUI, HstDatabase* pkDb)
{
	// I1. Cache folder = run dir (CvGame::reset recreates <cache>Civ5SavedGameDatabase.db inside GC.init()). Given as
	// ".\" (CWD = run dir) so the DLL's path strings, and with them the deterministic heap layout, do not depend on
	// where the run dir is.
	pkEngine->SetCacheFolder(".\\");

	// I2. The DLL QIs ICvEngineUtility4 and later deletes the old pointer; Destroy() is a no-op (host-owned).
	if(!m_pkCtx->SetDLLIFace(pkEngine))
	{
		HstLog("init: SetDLLIFace failed");
		return false;
	}
	// I3.
	m_pkCtx->SetGameDatabase(pkDb->GetConnection());
	// I4. GC.init(): CvGame/CvMap, PerformDatabasePostProcessing, CacheGameDatabaseData.
	HstLog("init: ctx->Init() ...");
	DWORD dwStart = GetTickCount();
	m_pkCtx->Init();
	HstLog("init: ctx->Init() returned (%u ms)", (unsigned int)(GetTickCount() - dwStart));
	// GC.init() installs the DLL's AUI_MINIDUMPS unhandled-exception filter (full-memory dumps); take it back.
	HstInstallUnhandledFilter();

	// I5. Logging off (the exe reads these from config.ini right after Init: Net_ReadDebugConfig RVA 0x291ea0).
	m_pkCtx->SetLogging(false);
	m_pkCtx->SetRandLogging(0);
	m_pkCtx->SetAILogging(false);
	m_pkCtx->SetAIPerfLogging(false);
	m_pkCtx->SetBuilderAILogging(false);
	m_pkCtx->SetPlayerAndCityAILogSplit(false);
	m_pkCtx->SetTutorialLogging(false);
	m_pkCtx->SetTutorialDebugging(false);
	m_pkCtx->SetPostTurnAutosaves(false);

	// I6. After Init in the exe.
	m_pkCtx3->SetEngineUserInterface(pkUI);

	// I7. "Refresh Gameplay DLL Data" (redundant with I4; kept for fidelity with SetActiveDLCandMods).
	{
		std::auto_ptr<ICvDLLDatabaseUtility1> pkLoader(m_pkCtx->GetDatabaseLoadUtility());
		dwStart = GetTickCount();
		bool bFlush = pkLoader->FlushGameDatabaseData();
		bool bPost = pkLoader->PerformDatabasePostProcessing();
		bool bCache = pkLoader->CacheGameDatabaseData();
		HstLog("init: database utility flush=%d postprocess=%d cache=%d (%u ms)", bFlush, bPost, bCache, (unsigned int)(GetTickCount() - dwStart));
		if(!bCache)
			return false;
	}

	// I8. DLC allowed set from DownloadableContent (exe SetActiveDLCandMods tail).
	{
		std::vector<GUID> aDlc;
		pkDb->GetActiveDlc(aDlc);
		std::auto_ptr<ICvPreGame1> pkPreGame(m_pkCtx->GetPreGame());
		pkPreGame->clearDLCAllowed();
		for(size_t i = 0; i < aDlc.size(); ++i)
			pkPreGame->setDLCAllowed(aDlc[i], true);
		HstLog("init: %d DLC packages allowed", (int)aDlc.size());
	}
	return true;
}
