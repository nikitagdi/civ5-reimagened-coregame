#pragma once
// OPT-0080 (flag LEKMOD_HEADLESS_OPT_PAR_CLOSEST_OP_UNIT): a minimal persistent thread pool for intra-turn work items that
// are pure with respect to shared game state (for the conditions
// a work item has to meet). Only code under a LEKMOD_HEADLESS_OPT_PAR_* flag includes this header.
//
//  - T participating threads = the calling (simulation) thread + T-1 persistent workers created on first use. T comes from
//    the environment variable LEKMOD_HEADLESS_PAR_THREADS (1..HEADLESS_PAR_MAX_THREADS), else from the define
//    LEKMOD_HEADLESS_PAR_THREADS_DEFAULT, else min(4, logical processors - 1). T == 1 never creates a thread: callers take
//    their sequential (golden) path.
//  - Run() splits [0, n) into T static contiguous chunks, chunk t = [n*t/T, n*(t+1)/T). Worker t (0 <= t < T-1) always runs
//    chunk t and gets iThread = t; the calling thread runs the last chunk (t = T-1) and gets iThread = -1 (also when the
//    pool runs everything on the caller: T == 1 or the workers could not be started). The mapping does not depend on timing.
//  - One auto-reset start event per worker, one auto-reset done event per batch (the last worker to finish sets it).
//  - Every worker loads the caller's x87 control word (fnstcw on the caller per batch, fldcw on the worker) before it runs a
//    chunk, and zeroes 32 KB of its own stack before every chunk (worker stacks are not wiped by civhost).
//  - Plain Win32 threads (CreateThread): no CRT per-thread data is set up on the simulation thread, so the deterministic
//    heap of civhost sees no extra allocation from it. Workers that allocate get the Windows heap in civhost (HstDetHeap's
//    foreign-thread path) and the thread-safe CRT/FireWorks heap in the game.
//  - Shutdown() (from CvGlobals::uninit) stops and joins the workers.

#define HEADLESS_PAR_MAX_THREADS 16

typedef void (*HeadlessParChunkFn)(void* pContext, int iThread, int iBegin, int iEnd);

class CvHeadlessParPool
{
public:
	static CvHeadlessParPool& Get()
	{
		static CvHeadlessParPool s_kPool;	// first use is on the simulation thread
		return s_kPool;
	}

	int GetNumThreads()
	{
		if(m_iNumThreads == 0)
			m_iNumThreads = ConfiguredThreads();
		return m_iNumThreads;
	}

	/// Runs fn over [0, iNumItems) in GetNumThreads() static contiguous chunks; returns when all chunks are done.
	void Run(HeadlessParChunkFn fn, void* pContext, int iNumItems)
	{
		const int iT = GetNumThreads();
		if(iT <= 1 || !EnsureWorkers())
		{
			fn(pContext, -1, 0, iNumItems);
			return;
		}
		unsigned short usControlWord = 0;
		__asm fnstcw usControlWord
		m_usControlWord = usControlWord;
		m_pfnChunk = fn;
		m_pContext = pContext;
		m_iNumItems = iNumItems;
		m_lPending = iT - 1;
		for(int i = 0; i < iT - 1; ++i)
			SetEvent(m_ahStart[i]);
		fn(pContext, -1, ChunkBegin(iT - 1, iT, iNumItems), ChunkBegin(iT, iT, iNumItems));
		WaitForSingleObject(m_hDone, INFINITE);
	}

	void Shutdown()
	{
		if(m_iNumWorkers == 0)
			return;
		m_bQuit = true;
		for(int i = 0; i < m_iNumWorkers; ++i)
			SetEvent(m_ahStart[i]);
		WaitForMultipleObjects(m_iNumWorkers, m_ahThread, TRUE, INFINITE);
		for(int i = 0; i < m_iNumWorkers; ++i)
		{
			CloseHandle(m_ahThread[i]);
			CloseHandle(m_ahStart[i]);
		}
		CloseHandle(m_hDone);
		m_iNumWorkers = 0;
		m_bQuit = false;
	}

	static int ChunkBegin(int iChunk, int iNumChunks, int iNumItems)
	{
		return (int)(((__int64)iNumItems * iChunk) / iNumChunks);
	}

private:
	struct WorkerArg
	{
		CvHeadlessParPool* pPool;
		int iIndex;
	};

	CvHeadlessParPool()
		: m_iNumThreads(0)
		, m_iNumWorkers(0)
		, m_hDone(NULL)
		, m_pfnChunk(NULL)
		, m_pContext(NULL)
		, m_iNumItems(0)
		, m_lPending(0)
		, m_usControlWord(0)
		, m_bQuit(false)
	{
	}
	~CvHeadlessParPool() {}	// process exit: the OS ends the workers; a clean stop is Shutdown()
	CvHeadlessParPool(const CvHeadlessParPool&);
	CvHeadlessParPool& operator=(const CvHeadlessParPool&);

	static int ConfiguredThreads()
	{
		int iThreads = 0;
		char szValue[16];
		DWORD uiLen = GetEnvironmentVariableA("LEKMOD_HEADLESS_PAR_THREADS", szValue, sizeof(szValue));
		if(uiLen > 0 && uiLen < sizeof(szValue))
			iThreads = atoi(szValue);
#if defined(LEKMOD_HEADLESS_PAR_THREADS_DEFAULT)
		if(iThreads <= 0)
			iThreads = LEKMOD_HEADLESS_PAR_THREADS_DEFAULT;
#endif
		if(iThreads <= 0)
		{
			SYSTEM_INFO kInfo;
			GetSystemInfo(&kInfo);
			iThreads = (int)kInfo.dwNumberOfProcessors - 1;
			if(iThreads > 4)
				iThreads = 4;
		}
		if(iThreads < 1)
			iThreads = 1;
		if(iThreads > HEADLESS_PAR_MAX_THREADS)
			iThreads = HEADLESS_PAR_MAX_THREADS;
		return iThreads;
	}

	bool EnsureWorkers()
	{
		const int iWanted = m_iNumThreads - 1;
		if(m_iNumWorkers == iWanted)
			return true;
		m_hDone = CreateEventA(NULL, FALSE, FALSE, NULL);
		if(m_hDone == NULL)
			return false;
		for(int i = 0; i < iWanted; ++i)
		{
			m_akArg[i].pPool = this;
			m_akArg[i].iIndex = i;
			m_ahStart[i] = CreateEventA(NULL, FALSE, FALSE, NULL);
			m_ahThread[i] = (m_ahStart[i] != NULL) ? CreateThread(NULL, 1024 * 1024, WorkerMain, &m_akArg[i], STACK_SIZE_PARAM_IS_A_RESERVATION, NULL) : NULL;
			if(m_ahThread[i] == NULL)
			{
				// could not start the pool: stop what was started and run sequentially from now on
				m_iNumWorkers = i;
				if(m_ahStart[i] != NULL)
					CloseHandle(m_ahStart[i]);
				Shutdown();
				m_iNumThreads = 1;
				return false;
			}
			m_iNumWorkers = i + 1;
		}
		return true;
	}

	static __declspec(noinline) void WipeStack()
	{
		char acBuffer[32 * 1024];
		SecureZeroMemory(acBuffer, sizeof(acBuffer));
	}

	static DWORD WINAPI WorkerMain(LPVOID pArg)
	{
		WorkerArg* pkArg = (WorkerArg*)pArg;
		CvHeadlessParPool* pPool = pkArg->pPool;
		const int iIndex = pkArg->iIndex;
		for(;;)
		{
			WaitForSingleObject(pPool->m_ahStart[iIndex], INFINITE);
			if(pPool->m_bQuit)
				break;
			unsigned short usControlWord = pPool->m_usControlWord;
			__asm fldcw usControlWord
			WipeStack();
			const int iT = pPool->m_iNumThreads;
			pPool->m_pfnChunk(pPool->m_pContext, iIndex, ChunkBegin(iIndex, iT, pPool->m_iNumItems), ChunkBegin(iIndex + 1, iT, pPool->m_iNumItems));
			if(InterlockedDecrement(&pPool->m_lPending) == 0)
				SetEvent(pPool->m_hDone);
		}
		return 0;
	}

	int m_iNumThreads;
	int m_iNumWorkers;
	HANDLE m_ahThread[HEADLESS_PAR_MAX_THREADS];
	HANDLE m_ahStart[HEADLESS_PAR_MAX_THREADS];
	WorkerArg m_akArg[HEADLESS_PAR_MAX_THREADS];
	HANDLE m_hDone;
	HeadlessParChunkFn m_pfnChunk;
	void* m_pContext;
	int m_iNumItems;
	volatile LONG m_lPending;
	unsigned short m_usControlWord;
	volatile bool m_bQuit;
};
