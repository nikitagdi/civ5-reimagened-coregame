//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstDetHeap.cpp
//!  \brief     Deterministic, zero-filling heap behind the HeapAlloc family (see HstDetHeap.h).
//!
//!		Threading: the sim thread (the one that installs the heap) owns the free lists and the bump pointer and
//!		takes no lock. Every other thread is "foreign": its allocations come from the Windows heap (zero-filled, like a
//!		paused sim thread), its frees of deterministic blocks are dropped (the block leaks, counted), so a foreign thread
//!		can never touch the lock-free state and never changes the deterministic layout.
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#include "HstCommon.h"
#include "platform/HstDetHeap.h"
#include "platform/HstPlatform.h"

typedef LPVOID (WINAPI* PFN_HeapAlloc)(HANDLE, DWORD, SIZE_T);
typedef BOOL (WINAPI* PFN_HeapFree)(HANDLE, DWORD, LPVOID);
typedef LPVOID (WINAPI* PFN_HeapReAlloc)(HANDLE, DWORD, LPVOID, SIZE_T);
typedef SIZE_T (WINAPI* PFN_HeapSize)(HANDLE, DWORD, LPCVOID);
typedef BOOL (WINAPI* PFN_HeapValidate)(HANDLE, DWORD, LPCVOID);

static PFN_HeapAlloc s_pfnHeapAlloc = NULL;
static PFN_HeapFree s_pfnHeapFree = NULL;
static PFN_HeapReAlloc s_pfnHeapReAlloc = NULL;
static PFN_HeapSize s_pfnHeapSize = NULL;
static PFN_HeapValidate s_pfnHeapValidate = NULL;

enum { HST_DH_CLASSES = 160, HST_DH_COMMIT = 4 * 1024 * 1024, HST_DH_HEADER = 8 };

struct HstDetBlockHeader
{
	unsigned int uiSize;	// requested bytes (HeapSize)
	unsigned int uiClass;
};

// Per-thread state in ONE word so the hot path is a single TLS load and compare: 0 = foreign thread, HST_DH_SIM = the sim
// thread, running; HST_DH_SIM + n = the sim thread inside n nested pause scopes.
enum { HST_DH_SIM = 0x40000000 };
static __declspec(thread) int s_iTls = 0;

static bool s_bInstalled = false;
static bool s_bFixedBase = false;
static BYTE* s_pBase = NULL;
static BYTE* s_pCur = NULL;
static BYTE* s_pCommitted = NULL;
static BYTE* s_pEnd = NULL;
static size_t s_uiLive = 0;
static unsigned int s_uiAllocs = 0;
static bool s_bOutOfMemoryLogged = false;
static void* s_apFree[HST_DH_CLASSES];

// Perturbation axes (determinism lab). Defaults = the canonical deterministic heap.
static BYTE s_ucFill = 0;					// --heap-fill: pattern of fresh (non HEAP_ZERO_MEMORY) allocations
static unsigned int s_uiShift = 0;			// --heap-shift: offset of the first block
static unsigned int s_uiJitter = 0;			// --heap-jitter: seed of the per-request size padding (0 = off)
static unsigned int s_uiJitterState = 0;

// Statistics (sim thread only, except the foreign counters).
static unsigned int s_uiBumpAllocs = 0;
static volatile LONG s_lForeignAllocs = 0;
static volatile LONG s_lForeignFrees = 0;
static volatile LONG s_lForeignRealloc = 0;

void HstDetHeapPause()
{
	++s_iTls;
}

void HstDetHeapResume()
{
	--s_iTls;
}

void HstDetHeapSetFill(unsigned char ucFill)
{
	s_ucFill = ucFill;
}

void HstDetHeapSetShift(unsigned int uiBytes)
{
	s_uiShift = (uiBytes + 7) & ~7u;
}

void HstDetHeapSetJitter(unsigned int uiSeed)
{
	s_uiJitter = uiSeed;
	s_uiJitterState = uiSeed * 2654435761u + 0x9E3779B9u;
}

static inline bool HstDetOwns(LPCVOID p)
{
	return (const BYTE*)p >= s_pBase && (const BYTE*)p < s_pEnd;
}

//! Classes (block size incl. header): 8-byte steps up to 256 bytes, then 4 steps per power of two.
static inline int HstDetClass(size_t uiBytes, size_t& uiBlockOut)
{
	if(uiBytes <= 256)
	{
		size_t uiIdx = (uiBytes + 7) / 8;
		if(uiIdx == 0)
			uiIdx = 1;
		uiBlockOut = uiIdx * 8;
		return (int)uiIdx - 1;
	}
	int iPow = 8;
	while(((size_t)1 << (iPow + 1)) < uiBytes)
		++iPow;
	const size_t uiBase = (size_t)1 << iPow;
	const size_t uiStep = uiBase / 4;
	const size_t uiQ = (uiBytes - uiBase + uiStep - 1) / uiStep;
	uiBlockOut = uiBase + uiQ * uiStep;
	return 32 + (iPow - 8) * 4 + (int)uiQ - 1;
}

static size_t HstDetBlockBytes(unsigned int uiClass)
{
	if(uiClass < 32)
		return (uiClass + 1) * 8;
	const int iPow = 8 + (int)(uiClass - 32) / 4;
	const size_t uiQ = (uiClass - 32) % 4 + 1;
	const size_t uiBase = (size_t)1 << iPow;
	return uiBase + uiQ * (uiBase / 4);
}

//! memset without the CRT call for the small blocks that dominate (the DLL's std::vector/string/SQLite traffic).
static inline void HstDetFill(BYTE* p, size_t uiBytes, BYTE ucFill)
{
	if(uiBytes > 96)
	{
		memset(p, ucFill, uiBytes);
		return;
	}
	const unsigned int uiWord = ucFill * 0x01010101u;
	BYTE* const pEnd = p + uiBytes;
	while(p + 4 <= pEnd)
	{
		*(unsigned int*)p = uiWord;
		p += 4;
	}
	while(p < pEnd)
		*p++ = ucFill;
}

//! Sim thread only. Returns the user pointer (filled for uiSize bytes: zero when bZero, else the heap-fill pattern) or NULL.
static void* HstDetAlloc(size_t uiSize, bool bZero)
{
	if(uiSize > 0x7fff0000)
		return NULL;
	size_t uiClassBytes = uiSize + HST_DH_HEADER;
	if(s_uiJitter)
	{
		// Pad the request by 0..3 granules so size classes, free-list reuse and with it the relative order of live blocks change.
		s_uiJitterState = s_uiJitterState * 1664525u + 1013904223u;
		uiClassBytes += ((s_uiJitterState >> 24) & 3) * 8;
	}
	size_t uiBlock = 0;
	const int iClass = HstDetClass(uiClassBytes, uiBlock);
	if(iClass < 0 || iClass >= HST_DH_CLASSES)
		return NULL;
	BYTE* pBlock = (BYTE*)s_apFree[iClass];
	bool bFresh = false;
	if(pBlock)
	{
		s_apFree[iClass] = *(void**)pBlock;
	}
	else
	{
		if(s_pCur + uiBlock > s_pEnd)
		{
			if(!s_bOutOfMemoryLogged)
			{
				s_bOutOfMemoryLogged = true;
				HstLog("detheap: FATAL region exhausted (%u MB used); allocation of %u bytes fails", (unsigned int)((s_pCur - s_pBase) >> 20), (unsigned int)uiSize);
			}
			return NULL;
		}
		while(s_pCur + uiBlock > s_pCommitted)
		{
			size_t uiCommit = HST_DH_COMMIT;
			if(s_pCommitted + uiCommit > s_pEnd)
				uiCommit = (size_t)(s_pEnd - s_pCommitted);
			if(!VirtualAlloc(s_pCommitted, uiCommit, MEM_COMMIT, PAGE_READWRITE))
				return NULL;
			s_pCommitted += uiCommit;
		}
		pBlock = s_pCur;
		s_pCur += uiBlock;
		bFresh = true;
	}
	HstDetBlockHeader* pkHeader = (HstDetBlockHeader*)pBlock;
	pkHeader->uiSize = (unsigned int)uiSize;
	pkHeader->uiClass = (unsigned int)iClass;
	s_uiLive += uiBlock;
	++s_uiAllocs;
	BYTE* pUser = pBlock + HST_DH_HEADER;
	const BYTE ucFill = bZero ? 0 : s_ucFill;
	if(bFresh)
	{
		++s_uiBumpAllocs;
		if(ucFill)	// committed pages come zero-filled and the bump pointer never goes back: nothing to do for zero
			HstDetFill(pUser, uiSize, ucFill);
	}
	else
		HstDetFill(pUser, uiSize, ucFill);
	return pUser;
}

static void HstDetFree(void* pUser)
{
	BYTE* pBlock = (BYTE*)pUser - HST_DH_HEADER;
	const unsigned int uiClass = ((HstDetBlockHeader*)pBlock)->uiClass;
	s_uiLive -= HstDetBlockBytes(uiClass);
	*(void**)pBlock = s_apFree[uiClass];
	s_apFree[uiClass] = pBlock;
}

// ---- hooks -----------------------------------------------------------------------------------------------------------

//! Paused sim thread or foreign thread: Windows heap, always zero-filled (host-owned memory has no layout contract).
static LPVOID HstDetSystemAlloc(HANDLE hHeap, DWORD dwFlags, SIZE_T uiBytes)
{
	if(!(s_iTls & HST_DH_SIM))
		InterlockedIncrement(&s_lForeignAllocs);
	return s_pfnHeapAlloc(hHeap, dwFlags | HEAP_ZERO_MEMORY, uiBytes);
}

static LPVOID WINAPI HstDetHook_HeapAlloc(HANDLE hHeap, DWORD dwFlags, SIZE_T uiBytes)
{
	if(s_iTls != HST_DH_SIM)
		return HstDetSystemAlloc(hHeap, dwFlags, uiBytes);
	void* p = HstDetAlloc(uiBytes, (dwFlags & HEAP_ZERO_MEMORY) != 0);
	if(!p)
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
	return p;
}

static BOOL WINAPI HstDetHook_HeapFree(HANDLE hHeap, DWORD dwFlags, LPVOID pMem)
{
	if(!pMem)
		return TRUE;
	if(!HstDetOwns(pMem))
		return s_pfnHeapFree(hHeap, dwFlags, pMem);
	if(!(s_iTls & HST_DH_SIM))
	{
		InterlockedIncrement(&s_lForeignFrees);	// a foreign thread never touches the lock-free lists: the block leaks
		return TRUE;
	}
	HstDetFree(pMem);
	return TRUE;
}

static LPVOID WINAPI HstDetHook_HeapReAlloc(HANDLE hHeap, DWORD dwFlags, LPVOID pMem, SIZE_T uiBytes)
{
	if(!pMem)
		return HstDetHook_HeapAlloc(hHeap, dwFlags, uiBytes);
	if(!(s_iTls & HST_DH_SIM))
	{
		InterlockedIncrement(&s_lForeignRealloc);
		if(!HstDetOwns(pMem))
			return s_pfnHeapReAlloc(hHeap, dwFlags, pMem, uiBytes);
		// A deterministic block resized by a foreign thread: move it to the Windows heap, leak the old block.
		if(dwFlags & HEAP_REALLOC_IN_PLACE_ONLY)
			return NULL;
		const size_t uiOldForeign = ((const HstDetBlockHeader*)((const BYTE*)pMem - HST_DH_HEADER))->uiSize;
		void* pForeign = s_pfnHeapAlloc(hHeap, HEAP_ZERO_MEMORY, uiBytes);
		if(pForeign)
			memcpy(pForeign, pMem, uiOldForeign < uiBytes ? uiOldForeign : uiBytes);
		return pForeign;
	}
	if(!HstDetOwns(pMem))
	{
		// A block from before Install() (or from a pause): move it into the deterministic heap (unless in-place only).
		if((dwFlags & HEAP_REALLOC_IN_PLACE_ONLY) || s_iTls != HST_DH_SIM)
			return s_pfnHeapReAlloc(hHeap, dwFlags, pMem, uiBytes);
		const SIZE_T uiOld = s_pfnHeapSize(hHeap, 0, pMem);
		if(uiOld == (SIZE_T)-1)
			return NULL;
		void* pNew = HstDetAlloc(uiBytes, (dwFlags & HEAP_ZERO_MEMORY) != 0);
		if(!pNew)
			return NULL;
		memcpy(pNew, pMem, uiOld < uiBytes ? uiOld : uiBytes);
		s_pfnHeapFree(hHeap, 0, pMem);
		return pNew;
	}
	HstDetBlockHeader* pkHeader = (HstDetBlockHeader*)((BYTE*)pMem - HST_DH_HEADER);
	const size_t uiOld = pkHeader->uiSize;
	size_t uiBlock = 0;
	const int iNewClass = HstDetClass(uiBytes + HST_DH_HEADER, uiBlock);
	void* pResult = NULL;
	if(iNewClass == (int)pkHeader->uiClass && !s_uiJitter)
	{
		if(uiBytes > uiOld)
			memset((BYTE*)pMem + uiOld, (dwFlags & HEAP_ZERO_MEMORY) ? 0 : s_ucFill, uiBytes - uiOld);
		pkHeader->uiSize = (unsigned int)uiBytes;
		pResult = pMem;
	}
	else if(!(dwFlags & HEAP_REALLOC_IN_PLACE_ONLY))
	{
		pResult = HstDetAlloc(uiBytes, (dwFlags & HEAP_ZERO_MEMORY) != 0);
		if(pResult)
		{
			memcpy(pResult, pMem, uiOld < uiBytes ? uiOld : uiBytes);
			HstDetFree(pMem);
		}
	}
	if(!pResult)
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
	return pResult;
}

static SIZE_T WINAPI HstDetHook_HeapSize(HANDLE hHeap, DWORD dwFlags, LPCVOID pMem)
{
	if(!HstDetOwns(pMem))
		return s_pfnHeapSize(hHeap, dwFlags, pMem);
	return ((const HstDetBlockHeader*)((const BYTE*)pMem - HST_DH_HEADER))->uiSize;
}

static BOOL WINAPI HstDetHook_HeapValidate(HANDLE hHeap, DWORD dwFlags, LPCVOID pMem)
{
	if(pMem && HstDetOwns(pMem))
		return TRUE;
	return s_pfnHeapValidate(hHeap, dwFlags, pMem);
}

// ---- API ---------------------------------------------------------------------------------------------------------------

int HstDetHeapPatchModule(HMODULE hModule)
{
	if(!s_bInstalled || !hModule)
		return 0;
	int iPatched = 0;
	iPatched += HstPatchImport(hModule, "kernel32", "HeapAlloc", (void*)HstDetHook_HeapAlloc);
	iPatched += HstPatchImport(hModule, "kernel32", "HeapFree", (void*)HstDetHook_HeapFree);
	iPatched += HstPatchImport(hModule, "kernel32", "HeapReAlloc", (void*)HstDetHook_HeapReAlloc);
	iPatched += HstPatchImport(hModule, "kernel32", "HeapSize", (void*)HstDetHook_HeapSize);
	iPatched += HstPatchImport(hModule, "kernel32", "HeapValidate", (void*)HstDetHook_HeapValidate);
	return iPatched;
}

bool HstDetHeapInstall(size_t uiReserveBytes)
{
	if(s_bInstalled)
		return true;
	HMODULE hKernel = GetModuleHandleW(L"kernel32.dll");
	s_pfnHeapAlloc = (PFN_HeapAlloc)GetProcAddress(hKernel, "HeapAlloc");
	s_pfnHeapFree = (PFN_HeapFree)GetProcAddress(hKernel, "HeapFree");
	s_pfnHeapReAlloc = (PFN_HeapReAlloc)GetProcAddress(hKernel, "HeapReAlloc");
	s_pfnHeapSize = (PFN_HeapSize)GetProcAddress(hKernel, "HeapSize");
	s_pfnHeapValidate = (PFN_HeapValidate)GetProcAddress(hKernel, "HeapValidate");
	if(!s_pfnHeapAlloc || !s_pfnHeapFree || !s_pfnHeapReAlloc || !s_pfnHeapSize || !s_pfnHeapValidate)
		return false;

	// Above 2 GB (civhost is /LARGEADDRESSAWARE): free in a 32-bit process on 64-bit Windows; the Lua arena sits at
	// 0x30000000 and the game DLLs below 0x20000000.
	static const DWORD s_adwBases[] = { 0x80000000, 0x90000000, 0xA0000000, 0x60000000 };
	for(size_t i = 0; i < HST_ARRAY_SIZE(s_adwBases) && !s_pBase; ++i)
		s_pBase = (BYTE*)VirtualAlloc((void*)(DWORD_PTR)s_adwBases[i], uiReserveBytes, MEM_RESERVE, PAGE_READWRITE);
	s_bFixedBase = (s_pBase != NULL);
	if(!s_pBase)
		s_pBase = (BYTE*)VirtualAlloc(NULL, uiReserveBytes, MEM_RESERVE, PAGE_READWRITE);
	if(!s_pBase)
		return false;
	s_pCommitted = s_pBase;
	s_pCur = s_pBase + s_uiShift;
	s_pEnd = s_pBase + uiReserveBytes;
	memset(s_apFree, 0, sizeof(s_apFree));
	// The installing thread is the sim thread: from here on its hot path takes no lock.
	s_iTls = HST_DH_SIM;
	s_bInstalled = true;

	const int iPatched = HstDetHeapPatchModule(GetModuleHandleW(L"msvcr90.dll"));
	HstLog("detheap: %u MB reserved at 0x%08x (%s), MSVCR90 %d IAT slot(s), lock-free sim thread", (unsigned int)(uiReserveBytes >> 20), (unsigned int)(DWORD_PTR)s_pBase,
		s_bFixedBase ? "fixed" : "NOT fixed: addresses vary per process", iPatched);
	if(s_ucFill || s_uiShift || s_uiJitter)
		HstLog("detheap: perturbation fill=0x%02x shift=%u jitter=%u", (unsigned int)s_ucFill, s_uiShift, s_uiJitter);
	return iPatched > 0;
}

bool HstDetHeapIsInstalled()
{
	return s_bInstalled;
}

bool HstDetHeapIsFixedBase()
{
	return s_bFixedBase;
}

void* HstDetHeapGetBase()
{
	return s_pBase;
}

size_t HstDetHeapGetUsedBytes()
{
	return (size_t)(s_pCur - s_pBase);
}

size_t HstDetHeapGetLiveBytes()
{
	return s_uiLive;
}

unsigned int HstDetHeapGetAllocCount()
{
	return s_uiAllocs;
}

unsigned int HstDetHeapGetBumpCount()
{
	return s_uiBumpAllocs;
}

void HstDetHeapGetForeignCounts(unsigned int& uiAllocs, unsigned int& uiFrees, unsigned int& uiReallocs)
{
	uiAllocs = (unsigned int)s_lForeignAllocs;
	uiFrees = (unsigned int)s_lForeignFrees;
	uiReallocs = (unsigned int)s_lForeignRealloc;
}
