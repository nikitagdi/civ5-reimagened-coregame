#pragma once
// Headless platform: diagnostic hooks for civhost.
// Everything below is compiled only with LEKMOD_HEADLESS_DIAG (variants diag / optimized-diag). In golden and
// optimized the macros expand to nothing, so this header never changes their code.
//
// Contract with the host (headless/host): the DLL exports
//     extern "C" int __cdecl HeadlessDiagInstall(ICvHeadlessDiagSink1* pSink);   // returns HEADLESS_DIAG_VERSION
// pSink == NULL uninstalls. No STL objects cross the boundary.
//
// ---- CONSUMER NOTE (host) -------------------------------------------------------------------------------------
// * Availability: only the diag / optimized-diag DLLs export HeadlessDiagInstall; golden / optimized do not
//   (GetProcAddress returns NULL there, which is the normal "no hooks" case).
// * Install order: LoadLibrary(CvGameCore_Expansion2.dll) -> GetProcAddress("HeadlessDiagInstall") ->
//   HeadlessDiagInstall(pSink) -> only then DllGetGameContext() / game init / load. Installing before the game objects
//   exist means the initial reseeds are reported; installing later is legal but earlier RNG state changes are not.
//   The return value is always the DLL's HEADLESS_DIAG_VERSION: compare it with the host's compiled-in value. The DLL
//   refuses to store a sink whose GetVersion() differs (stays uninstalled), so a stale host cannot crash it.
//   Install / uninstall only while no host->DLL call is in progress (never from inside a sink callback).
// * Threading: single thread. Every sink method is called synchronously on the thread that executes the gamecore
//   (the host's sim thread), never concurrently and never from another thread. The DLL does no locking.
// * Sink restrictions: methods are invoked from inside RNG / zone code with the DLL mid-call, so a sink must not call
//   back into the DLL (no CvRandom use, no game queries), must not throw, must be cheap (OnRandCall fires millions of
//   times per turn) and must not use floating point without saving/restoring the x87 control word (the DLL is built
//   /fp:strict and its determinism depends on the control word).
// * Strings (pszLog, pszWhat) are valid only during the call: pszLog of getJonRandNumVA points to a stack buffer.
//   Copy or hash them if they are kept.
// * Zones nest and may re-enter the same id (Lua hook -> DLL -> Lua hook); enter/leave are strictly LIFO (RAII, also on
//   exceptions). Keep a stack on the sink side.
// * Hooks never change RNG results, call counts or any game state. Without a sink every hook costs one pointer test.
// * Functions that carry hooks (the only places where diag code may differ from golden, besides inlining effects):
//   CvRandom::reset/reseed/read/get/getBinom; CvGame::get{Map,Jon,Async}RandNum, getJonRandNumBinom, getJonRandNumVA;
//   one zone each at the entry of the functions named in HeadlessZone below; new export HeadlessDiagInstall.
// ---------------------------------------------------------------------------------------------------------------

#define HEADLESS_DIAG_VERSION 1

// RNG instance kinds reported by OnRandCall / OnRandReseed.
enum HeadlessRngKind
{
	HEADLESS_RNG_JON = 0,		// CvGame::m_jonRand (synchronous, getJonRandNum)
	HEADLESS_RNG_MAP = 1,		// CvGame::m_mapRand (synchronous, getMapRandNum)
	HEADLESS_RNG_ASYNC = 2,		// GC.getASyncRand() (getAsyncRandNum)
	HEADLESS_RNG_OTHER_SYNC = 3,	// any other CvRandom with m_bSynchronous
	HEADLESS_RNG_OTHER_ASYNC = 4,
};

// Perf / phase zones (perf-lab L2, determinism-lab OnPhase). Keep ids stable: they are written into metrics files.
// Each zone is an RAII scope at the entry of the listed function (inclusive of everything it calls).
enum HeadlessZone
{
	HEADLESS_ZONE_GAME_DOTURN = 0,		// CvGame::doTurn
	HEADLESS_ZONE_GAME_UPDATEMOVES,		// CvGame::updateMoves
	HEADLESS_ZONE_PLAYER_DOTURN,		// CvPlayer::doTurn
	HEADLESS_ZONE_PLAYER_DOTURN_POST,	// CvPlayer::doTurnPostDiplomacy
	HEADLESS_ZONE_PLAYER_UNITS,			// CvPlayer::doTurnUnits
	HEADLESS_ZONE_AI_UNITPROCESSING,	// CvPlayerAI::AI_unitUpdate
	HEADLESS_ZONE_AI_TACTICAL,			// CvTacticalAI::Update
	HEADLESS_ZONE_AI_HOMELAND,			// CvHomelandAI::Update
	HEADLESS_ZONE_AI_MILITARY,			// CvMilitaryAI::DoTurn
	HEADLESS_ZONE_AI_DIPLOMACY,			// CvDiplomacyAI::DoTurn (once per target player)
	HEADLESS_ZONE_AI_DANGER,			// CvDangerPlots::UpdateDanger
	HEADLESS_ZONE_AI_TACTICAL_MAP,		// CvTacticalAnalysisMap::RefreshDataForNextPlayer
	HEADLESS_ZONE_CITY_DOTURN,			// CvCity::doTurn
	HEADLESS_ZONE_CITY_CITIZENS,		// CvCityCitizens::DoReallocateCitizens (any caller, incl. CvCityCitizens::DoTurn)
	HEADLESS_ZONE_UNIT_DOTURN,			// CvUnit::doTurn
	HEADLESS_ZONE_PATH_GENERATE,		// CvAStar::GeneratePath
	HEADLESS_ZONE_LUA_HOOK,				// LuaSupport::CallHook/CallTestAll/CallTestAny/CallAccumulator (int and float)
	HEADLESS_ZONE_COUNT
};

#if defined(LEKMOD_HEADLESS_DIAG)

// _ReturnAddress() without <intrin.h> (VS2008 intrin.h conflicts with the PCH include order).
extern "C" void* _ReturnAddress(void);
#pragma intrinsic(_ReturnAddress)

class ICvHeadlessDiagSink1
{
public:
	virtual unsigned int GetVersion() = 0;

	// Every CvRandom::get / getBinom call. ulSeedBefore/ulCallCountBefore are the values before the call, uiRange is
	// the requested range (get: [0, range), getBinom: number of trials), uiResult the returned value. pszLog may be
	// NULL. pRetAddr: for the CvGame::get{Jon,Map,Async}RandNum* wrappers the return address of the wrapper (i.e. an
	// address inside the game-logic caller), otherwise the return address of CvRandom::get / getBinom.
	virtual void OnRandCall(int eKind, const void* pRng, unsigned long ulSeedBefore, unsigned long ulCallCountBefore,
		unsigned int uiRange, unsigned int uiResult, const char* pszLog, const void* pRetAddr) = 0;
	// CvRandom::reset / reseed / init, and after CvRandom::read (savegame load) with the loaded seed.
	virtual void OnRandReseed(int eKind, const void* pRng, unsigned long ulNewSeed) = 0;

	virtual void OnZoneEnter(int eZone) = 0;
	virtual void OnZoneLeave(int eZone) = 0;

	// A FIX / FIX-VALIDATION branch that changes behaviour was taken (scope-ok / honest-corpus checks).
	virtual void OnFixTrigger(int iFixId) = 0;

	// optimized-diag only (HEADLESS_OPT_VERIFY): an OPT shadow check found the optimized result differs from the
	// reference computation. iOptId is the numeric part of the OPT-NNNN entry id.
	virtual void OnOptVerifyMismatch(int iOptId, const char* pszWhat) = 0;
};

extern ICvHeadlessDiagSink1* g_pHeadlessDiag;
// Set by the RNG wrappers (HEADLESS_RAND_CALLER) just before CvRandom::get, consumed (and cleared) by it.
extern const void* g_pHeadlessRandCaller;

// The sink is captured on entry so enter/leave stay balanced even if it is (un)installed in between.
class CvHeadlessZoneScope
{
public:
	explicit CvHeadlessZoneScope(int eZone) : m_pSink(g_pHeadlessDiag), m_eZone(eZone) { if(m_pSink) m_pSink->OnZoneEnter(eZone); }
	~CvHeadlessZoneScope() { if(m_pSink) m_pSink->OnZoneLeave(m_eZone); }
private:
	ICvHeadlessDiagSink1* m_pSink;
	int m_eZone;
};

#define HEADLESS_ZONE(eZone)			CvHeadlessZoneScope kHeadlessZone_##eZone(eZone)
#define HEADLESS_FIX_TRIGGER(iFixId)	do { if(g_pHeadlessDiag) g_pHeadlessDiag->OnFixTrigger(iFixId); } while(0)
// First statement of a CvGame RNG wrapper: makes OnRandCall report the wrapper's caller instead of the wrapper.
#define HEADLESS_RAND_CALLER()			do { if(g_pHeadlessDiag) g_pHeadlessRandCaller = _ReturnAddress(); } while(0)

// OPT shadow verification (optimized-diag only: LEKMOD_HEADLESS_OPT_VERIFY, set by build-dll for that variant).
// Usage, next to an OPT change (the reference computation is fenced the same way, so it exists only in that build):
//     #if defined(LEKMOD_HEADLESS_OPT_VERIFY)
//         int iRef = <original slow computation>;
//         HEADLESS_OPT_VERIFY(iFast == iRef, 12, "CvPlot::getFoo");
//     #endif
// cond == true means "optimized result equals reference"; it must be free of side effects and is always evaluated in
// optimized-diag. Never alters game state: the optimized value stays the one the game uses.
// A mismatch goes to the sink when one is installed. civhost installs none, so then the first mismatch is written to
// stderr (the runner's stdout.log) and the process ends with exit code 86: the run fails visibly (spec: mismatch = stop).
#if defined(LEKMOD_HEADLESS_OPT_VERIFY)
void HeadlessOptVerifyMismatch(int iOptId, const char* pszWhat);	// CvGameCoreDLL.cpp
#define HEADLESS_OPT_VERIFY(cond, iOptId, pszWhat)	do { if(!(cond)) HeadlessOptVerifyMismatch((iOptId), (pszWhat)); } while(0)
#else
#define HEADLESS_OPT_VERIFY(cond, iOptId, pszWhat)
#endif

#else

#define HEADLESS_ZONE(eZone)
#define HEADLESS_FIX_TRIGGER(iFixId)
#define HEADLESS_RAND_CALLER()
#define HEADLESS_OPT_VERIFY(cond, iOptId, pszWhat)

#endif
