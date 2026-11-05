/*	-------------------------------------------------------------------------------------------------------
	� 1991-2012 Take-Two Interactive Software and its subsidiaries.  Developed by Firaxis Games.  
	Sid Meier's Civilization V, Civ, Civilization, 2K Games, Firaxis Games, Take-Two Interactive Software 
	and their respective logos are all trademarks of Take-Two interactive Software, Inc.  
	All other marks and trademarks are the property of their respective owners.  
	All rights reserved. 
	------------------------------------------------------------------------------------------------------- */
#include "CvGameCoreDLLPCH.h"
#include "CvGlobals.h"
#include "ICvDLLUserInterface.h"
#include "Win32/FDebugHelper.h"
#include "CvDllContext.h"

// must be included after all other headers
#include "LintFree.h"

//------------------------------------------------------------------------------
extern "C" ICvGameContext1* DllGetGameContext()
{
	return CvDllGameContext::GetSingleton();
}
//------------------------------------------------------------------------------
#if defined(LEKMOD_HEADLESS_DIAG)
// Headless platform: diagnostic sink installed by the host, see HeadlessDiag.h
ICvHeadlessDiagSink1* g_pHeadlessDiag = NULL;
const void* g_pHeadlessRandCaller = NULL;

extern "C" __declspec(dllexport) int __cdecl HeadlessDiagInstall(ICvHeadlessDiagSink1* pSink)
{
	g_pHeadlessDiag = NULL;
	g_pHeadlessRandCaller = NULL;
	// A sink built against another interface version must never be called through our vtable layout:
	// stay uninstalled and let the host compare the returned version with its own.
	if(pSink && pSink->GetVersion() == HEADLESS_DIAG_VERSION)
		g_pHeadlessDiag = pSink;
	return HEADLESS_DIAG_VERSION;
}
#if defined(LEKMOD_HEADLESS_OPT_VERIFY)
// OPT shadow mismatch (HeadlessDiag.h, HEADLESS_OPT_VERIFY): sink if installed, else stderr + exit code 86.
void HeadlessOptVerifyMismatch(int iOptId, const char* pszWhat)
{
	if(g_pHeadlessDiag)
	{
		g_pHeadlessDiag->OnOptVerifyMismatch(iOptId, pszWhat);
		return;
	}
	char szBuf[512];
	_snprintf(szBuf, sizeof(szBuf) - 1, "HEADLESS_OPT_VERIFY mismatch: OPT-%04d %s (game turn %d)\n", iOptId, pszWhat ? pszWhat : "", GC.getGame().getGameTurn());
	szBuf[sizeof(szBuf) - 1] = 0;
	HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
	if(hErr != NULL && hErr != INVALID_HANDLE_VALUE)
	{
		DWORD dwWritten = 0;
		WriteFile(hErr, szBuf, (DWORD)strlen(szBuf), &dwWritten, NULL);
		FlushFileBuffers(hErr);
	}
	OutputDebugStringA(szBuf);
	TerminateProcess(GetCurrentProcess(), 86);
}
#endif
#endif
//------------------------------------------------------------------------------
BOOL APIENTRY DllMain(HANDLE hModule,
                      DWORD  ul_reason_for_call,
                      LPVOID)
{
	switch(ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
	{
		// The DLL is being loaded into the virtual address space of the current process as a result of the process starting up
		OutputDebugString("DLL_PROCESS_ATTACH\n");
		FDebugHelper::GetInstance().LoadSymbols((HMODULE)hModule);
		// set timer precision
#ifdef CVASSERT_ENABLE
		MMRESULT iTimeSet = timeBeginPeriod(1);		// set timeGetTime and sleep resolution to 1 ms, otherwise it's 10-16ms
		DEBUG_VARIABLE(iTimeSet);
		CvAssertMsg(iTimeSet==TIMERR_NOERROR, "failed setting timer resolution to 1 ms");
#else
		timeBeginPeriod(1);		// set timeGetTime and sleep resolution to 1 ms, otherwise it's 10-16ms
#endif
		CvDllGameContext::InitializeSingleton();
	}
	break;
	case DLL_THREAD_ATTACH:
		OutputDebugString("DLL_THREAD_ATTACH\n");
		break;
	case DLL_THREAD_DETACH:
		OutputDebugString("DLL_THREAD_DETACH\n");
		break;
	case DLL_PROCESS_DETACH:
		OutputDebugString("DLL_PROCESS_DETACH\n");
		timeEndPeriod(1);
		CvDllGameContext::DestroySingleton();
		GC.setDLLIFace(NULL);
		break;
	}

	return TRUE;	// success
}
