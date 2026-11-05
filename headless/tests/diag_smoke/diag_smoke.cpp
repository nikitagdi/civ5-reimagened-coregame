// Smoke test for the LEKMOD_HEADLESS_DIAG RNG hooks (HeadlessDiag.h).
// Loads a gamecore DLL, creates a stand-alone CvRandom through ICvGameContext1 and drives it. Prints a checksum of the
// results (must be identical for golden / diag / diag+sink: hooks never change the RNG) and, in "sink" mode, checks the
// OnRandCall / OnRandReseed stream against the LCG recurrence. "badsink" checks that a sink with a foreign interface
// version is refused. Build + run: headless\tests\diag_smoke\run.ps1
//
// Modes: nosink | sink | badsink.  Usage: diag_smoke <gamecore.dll> <mode> <civ5-game-dir>
#pragma warning(disable : 4100 4800 4355 4996 4244)
#if !defined(_SECURE_SCL)
#define _SECURE_SCL 0
#endif
#if !defined(_HAS_ITERATOR_DEBUGGING)
#define _HAS_ITERATOR_DEBUGGING 0
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <MMSystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <list>
#include <map>
#include <set>
#include <memory>
#include <limits>
#include <algorithm>
typedef unsigned char    byte;
typedef unsigned int     uint;
typedef wchar_t          wchar;
#include <FireWorks/FDefNew.h>
#include <FireWorks/FFireTypes.h>
#include <FireWorks/FAssert.h>
#include <FireWorks/FILogFile.h>
#include <FireWorks/FFreeListArrayBase.h>
#include <FireWorks/FDataStream.h>
#include <FireWorks/FFastList.h>
#include "CvGameDatabase.h"
#include "CvGameCoreDLLUtil.h"
#include "CvDllInterfaces.h"
#include "HeadlessDiag.h"

// The gamecore DLL (CvWorldBuilderMap ctor, run from DllMain) resolves this from the host exe with
// GetProcAddress(GetModuleHandle(NULL), ...); without it LoadLibrary fails with ERROR_DLL_INIT_FAILED (1114).
extern "C" __declspec(dllexport) void* FStringA_GetStringInitData() { static char s_ac[256] = {0}; return s_ac; }

struct RandEv { int kind; const void* rng; unsigned long seed, count; unsigned int range, result; char log[32]; const void* ret; };
struct ReseedEv { int kind; const void* rng; unsigned long seed; };

class TestSink : public ICvHeadlessDiagSink1
{
public:
	unsigned int m_uiVersion;
	std::vector<RandEv> m_calls;
	std::vector<ReseedEv> m_reseeds;
	TestSink(unsigned int v) : m_uiVersion(v) {}
	virtual unsigned int GetVersion() { return m_uiVersion; }
	virtual void OnRandCall(int eKind, const void* pRng, unsigned long s, unsigned long c, unsigned int range, unsigned int res, const char* log, const void* ret)
	{
		RandEv e; e.kind = eKind; e.rng = pRng; e.seed = s; e.count = c; e.range = range; e.result = res; e.ret = ret;
		strncpy(e.log, log ? log : "(null)", 31); e.log[31] = 0;
		m_calls.push_back(e);
	}
	virtual void OnRandReseed(int eKind, const void* pRng, unsigned long s) { ReseedEv e = { eKind, pRng, s }; m_reseeds.push_back(e); }
	virtual void OnZoneEnter(int) {}
	virtual void OnZoneLeave(int) {}
	virtual void OnFixTrigger(int) {}
	virtual void OnOptVerifyMismatch(int, const char*) {}
};

typedef ICvGameContext1* (*PFN_GetCtx)();
typedef int (__cdecl *PFN_Install)(ICvHeadlessDiagSink1*);

static int g_iFail = 0;
#define CHECK(c) do { if(!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++g_iFail; } } while(0)

int main(int argc, char** argv)
{
	if(argc < 4) { printf("usage: diag_smoke <gamecore.dll> <nosink|sink|badsink> <civ5-game-dir>\n"); return 2; }
	SetDllDirectoryA(argv[3]);		// lua51_Win32.dll, CvGameDatabaseWin32Final Release.dll, CvLocalizationWin32Final Release.dll
	HMODULE h = LoadLibraryA(argv[1]);
	if(!h) { printf("LoadLibrary failed %lu\n", GetLastError()); return 3; }
	PFN_GetCtx pGet = (PFN_GetCtx)GetProcAddress(h, "DllGetGameContext");
	PFN_Install pInstall = (PFN_Install)GetProcAddress(h, "HeadlessDiagInstall");
	printf("install export: %s\n", pInstall ? "yes" : "no");

	TestSink kSink(HEADLESS_DIAG_VERSION);
	TestSink kBad(99);
	std::string mode = argv[2];
	if(mode == "sink" && pInstall)
	{
		int v = pInstall(&kSink);
		CHECK(v == HEADLESS_DIAG_VERSION);
	}
	if(mode == "badsink" && pInstall)
	{
		int v = pInstall(&kBad);
		CHECK(v == HEADLESS_DIAG_VERSION);
	}

	ICvGameContext1* pCtx = pGet();
	CHECK(pCtx != NULL);
	unsigned int idx = pCtx->CreateRandomNumberGenerator();
	ICvRandom1* pRng = pCtx->GetRandomNumberGenerator(idx);
	CHECK(pRng != NULL);

	pRng->Init(12345);
	unsigned int uiHash = 2166136261u;
	const int N = 5000;
	std::vector<unsigned short> res;
	for(int i = 0; i < N; ++i)
	{
		unsigned short r = pRng->Get((unsigned short)(2 + (i * 7) % 4000), "smoke");
		res.push_back(r);
		uiHash = (uiHash ^ r) * 16777619u;
	}
	pRng->Reset(777);
	for(int i = 0; i < 50; ++i) { unsigned short r = pRng->Get(100, NULL); uiHash = (uiHash ^ r) * 16777619u; res.push_back(r); }
	printf("results hash %08x  final seed %lu  n=%u\n", uiHash, pRng->GetSeed(), (unsigned int)res.size());

	if(mode == "sink")
	{
		CHECK(kSink.m_calls.size() == res.size());
		CHECK(kSink.m_reseeds.size() >= 2);
		if(kSink.m_reseeds.size() >= 2)
		{
			// Init(12345) and Reset(777) are the last two reseeds
			CHECK(kSink.m_reseeds[kSink.m_reseeds.size() - 2].seed == 12345);
			CHECK(kSink.m_reseeds[kSink.m_reseeds.size() - 1].seed == 777);
		}
		// Active LCG (AUI_RANDOM_FIX_CONSTANTS_SET_TO_MODULUS_2_POW_32 is commented out in _Defines.h)
		unsigned long ulSeed = 12345, ulCount = 0;
		int iBad = 0;
		for(size_t i = 0; i < kSink.m_calls.size(); ++i)
		{
			const RandEv& e = kSink.m_calls[i];
			if(i == (size_t)N) { ulSeed = 777; }	// Reset reseeds but keeps the call count
			unsigned long ulNew = 1103515245u * ulSeed + 12345u;
			unsigned int uiExpect = (unsigned int)(((ulNew >> 16) & 0xFFFF) * (unsigned long)e.range / 65536);
			// a stand-alone generator is neither CvGame's jon/map RNG nor GC's async one: OTHER_ASYNC (default ctor)
			if(e.seed != ulSeed || e.count != ulCount || e.result != uiExpect || e.result != res[i] || e.kind != HEADLESS_RNG_OTHER_ASYNC || e.rng == NULL)
			{
				if(iBad < 5) printf("bad event %u: seed %lu(exp %lu) count %lu(exp %lu) result %u(exp %u, api %u) kind %d\n", (unsigned int)i, e.seed, ulSeed, e.count, ulCount, e.result, uiExpect, (unsigned int)res[i], e.kind);
				++iBad;
			}
			ulSeed = ulNew;
			++ulCount;
		}
		CHECK(iBad == 0);
		CHECK(strcmp(kSink.m_calls[0].log, "smoke") == 0);
		CHECK(strcmp(kSink.m_calls[N].log, "(null)") == 0);
		CHECK(kSink.m_calls[0].ret != NULL);
		printf("sink events: calls=%u reseeds=%u bad=%d\n", (unsigned int)kSink.m_calls.size(), (unsigned int)kSink.m_reseeds.size(), iBad);
	}
	if(mode == "badsink")
	{
		CHECK(kBad.m_calls.empty());
		CHECK(kBad.m_reseeds.empty());
	}
	printf(g_iFail ? "RESULT: FAIL (%d)\n" : "RESULT: ok\n", g_iFail);
	return g_iFail ? 1 : 0;
}
