//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstArenaAlloc.cpp
//!  \brief     Fixed-base bump arena with LIFO size-class free lists (deterministic addresses).
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#include "HstCommon.h"
#include "script/HstArenaAlloc.h"

HstArenaAlloc::HstArenaAlloc()
	: m_pBase(NULL)
	, m_pCur(NULL)
	, m_pCommitted(NULL)
	, m_pEnd(NULL)
	, m_bFixedBase(false)
	, m_uiLive(0)
{
	memset(m_apFree, 0, sizeof(m_apFree));
}

bool HstArenaAlloc::Init(size_t uiReserveBytes)
{
	// Candidate bases between the gamecore DLL (0x10000000) and the system DLLs; civhost is /DYNAMICBASE:NO.
	static const DWORD s_adwBases[] = { 0x30000000, 0x40000000, 0x50000000, 0x60000000, 0x20000000 };
	for(size_t i = 0; i < HST_ARRAY_SIZE(s_adwBases) && !m_pBase; ++i)
	{
		m_pBase = (BYTE*)VirtualAlloc((void*)(DWORD_PTR)s_adwBases[i], uiReserveBytes, MEM_RESERVE, PAGE_READWRITE);
		m_bFixedBase = (m_pBase != NULL);
	}
	if(!m_pBase)
		m_pBase = (BYTE*)VirtualAlloc(NULL, uiReserveBytes, MEM_RESERVE, PAGE_READWRITE);
	if(!m_pBase)
		return false;
	m_pCur = m_pCommitted = m_pBase;
	m_pEnd = m_pBase + uiReserveBytes;
	return true;
}

//! Classes: 8-byte steps up to 256 bytes, then 4 steps per power of two.
int HstArenaAlloc::GetClass(size_t uiSize, size_t& uiBlockOut)
{
	if(uiSize <= 256)
	{
		size_t uiIdx = (uiSize + 7) / 8;
		if(uiIdx == 0)
			uiIdx = 1;
		uiBlockOut = uiIdx * 8;
		return (int)uiIdx - 1;
	}
	int iPow = 8;
	while(((size_t)1 << (iPow + 1)) < uiSize)
		++iPow;
	const size_t uiBase = (size_t)1 << iPow;	// uiBase < uiSize <= 2*uiBase
	const size_t uiStep = uiBase / 4;
	size_t uiQ = (uiSize - uiBase + uiStep - 1) / uiStep;	// 1..4
	uiBlockOut = uiBase + uiQ * uiStep;
	return 32 + (iPow - 8) * 4 + (int)uiQ - 1;
}

void* HstArenaAlloc::Alloc(size_t uiSize)
{
	size_t uiBlock = 0;
	int iClass = GetClass(uiSize, uiBlock);
	if(iClass < 0 || iClass >= NUM_CLASSES)
		return NULL;
	m_uiLive += uiBlock;
	void* p = m_apFree[iClass];
	if(p)
	{
		m_apFree[iClass] = *(void**)p;
		return p;
	}
	if(m_pCur + uiBlock > m_pEnd)
	{
		m_uiLive -= uiBlock;
		return NULL;
	}
	while(m_pCur + uiBlock > m_pCommitted)
	{
		size_t uiCommit = COMMIT_CHUNK;
		if(m_pCommitted + uiCommit > m_pEnd)
			uiCommit = (size_t)(m_pEnd - m_pCommitted);
		if(!VirtualAlloc(m_pCommitted, uiCommit, MEM_COMMIT, PAGE_READWRITE))
		{
			m_uiLive -= uiBlock;
			return NULL;
		}
		m_pCommitted += uiCommit;
	}
	p = m_pCur;
	m_pCur += uiBlock;
	return p;
}

void HstArenaAlloc::Free(void* p, size_t uiSize)
{
	if(!p)
		return;
	size_t uiBlock = 0;
	int iClass = GetClass(uiSize, uiBlock);
	m_uiLive -= uiBlock;
	*(void**)p = m_apFree[iClass];
	m_apFree[iClass] = p;
}

void* HstArenaAlloc::Realloc(void* p, size_t uiOldSize, size_t uiNewSize)
{
	if(!p)
		return Alloc(uiNewSize);
	size_t uiOldBlock = 0, uiNewBlock = 0;
	if(GetClass(uiOldSize, uiOldBlock) == GetClass(uiNewSize, uiNewBlock))
		return p;
	void* pNew = Alloc(uiNewSize);
	if(!pNew)
		return NULL;
	memcpy(pNew, p, uiOldSize < uiNewSize ? uiOldSize : uiNewSize);
	Free(p, uiOldSize);
	return pNew;
}

void* HstArenaAlloc::LuaAlloc(void* pUserData, void* p, size_t uiOldSize, size_t uiNewSize)
{
	HstArenaAlloc* pkArena = (HstArenaAlloc*)pUserData;
	if(uiNewSize == 0)
	{
		pkArena->Free(p, uiOldSize);
		return NULL;
	}
	return pkArena->Realloc(p, uiOldSize, uiNewSize);
}
