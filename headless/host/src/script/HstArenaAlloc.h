//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstArenaAlloc.h
//!  \brief     Deterministic allocator for the Lua master state (design 5/R6).
//!
//!		Lua `pairs` over tables keyed by tables/functions follows pointer order. A fixed-base VirtualAlloc arena
//!		with size-class free lists makes every address a pure function of the allocation sequence, so two
//!		processes running the same scenario see the same addresses (and the same iteration order).
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#pragma once
#ifndef HSTARENAALLOC_H
#define HSTARENAALLOC_H

#include "HstCommon.h"

class HstArenaAlloc
{
public:
	HstArenaAlloc();

	//! Reserve uiReserveBytes at the first free preferred base (falls back to any address: IsFixedBase() == false).
	bool Init(size_t uiReserveBytes);
	bool IsFixedBase() const { return m_bFixedBase; }
	void* GetBase() const { return m_pBase; }
	size_t GetUsedBytes() const { return (size_t)(m_pCur - m_pBase); }
	size_t GetLiveBytes() const { return m_uiLive; }

	void* Alloc(size_t uiSize);
	void Free(void* p, size_t uiSize);
	void* Realloc(void* p, size_t uiOldSize, size_t uiNewSize);

	//! lua_Alloc: ud = HstArenaAlloc*.
	static void* LuaAlloc(void* pUserData, void* p, size_t uiOldSize, size_t uiNewSize);

protected:
	enum { NUM_CLASSES = 160, COMMIT_CHUNK = 1024 * 1024 };
	static int GetClass(size_t uiSize, size_t& uiBlockOut);

	BYTE* m_pBase;
	BYTE* m_pCur;
	BYTE* m_pCommitted;
	BYTE* m_pEnd;
	bool m_bFixedBase;
	size_t m_uiLive;
	void* m_apFree[NUM_CLASSES];
};

#endif // HSTARENAALLOC_H
