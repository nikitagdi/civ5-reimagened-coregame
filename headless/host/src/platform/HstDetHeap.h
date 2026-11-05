//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstDetHeap.h
//!  \brief     Deterministic process heap for the game modules (design 5/R6, extended to the C heap).
//!
//!		Windows randomizes heap bases per process even for a /DYNAMICBASE:NO exe (measured: the process heap
//!		and HeapCreate heaps move between runs). The gamecore DLL serializes uninitialized memory (raw std::string
//!		objects with SSO garbage, CvNotifications::Notification fields never set by Clear() that hold stale stack
//!		copies of heap pointers, ...) and keeps pointer-ordered containers, so heap addresses leak into the state.
//!
//!		HstDetHeap replaces HeapAlloc/HeapFree/HeapReAlloc/HeapSize/HeapValidate in the import tables of MSVCR90
//!		(malloc/new of every /MD module: gamecore, MSVCP90 std::string, database/localization DLLs, civhost) and
//!		of the gamecore DLL (its private HeapCreate heap). Blocks come from a fixed-base VirtualAlloc region with
//!		size classes and LIFO free lists, and are zero-filled, so every address and every "uninitialized" byte is
//!		a pure function of the allocation sequence. Blocks allocated before Install() stay with their original heap.
//!
//!		Deviation from the real game: there these bytes are garbage and the addresses differ per machine
//!		(itself a desync class; see determinism-lab).
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#pragma once
#ifndef HSTDETHEAP_H
#define HSTDETHEAP_H

#include "HstCommon.h"

//! Perturbation axes of the determinism lab: set BEFORE HstDetHeapInstall; defaults give the
//! canonical heap. They change only what an uninitialized read or an address-dependent computation of the DLL can see.
//! Fill: byte pattern of fresh allocations that did not ask for zeroed memory (HEAP_ZERO_MEMORY / calloc stay zero).
void HstDetHeapSetFill(unsigned char ucFill);
//! Shift: the first block starts this many bytes (rounded up to 8) into the region: every address moves by the same amount.
void HstDetHeapSetShift(unsigned int uiBytes);
//! Jitter: seed (0 = off) of a pseudo-random 0..24 byte padding per request: size classes, free-list reuse and the relative
//! order of live blocks change (pointer-ordered containers).
void HstDetHeapSetJitter(unsigned int uiSeed);

//! Reserve the region (fixed base preferred) and patch MSVCR90. Call before anything from the game is loaded.
//! The calling thread becomes the sim thread: its allocation path takes no lock. Other threads allocate from the Windows
//! heap and never free into this heap (their frees of deterministic blocks leak, see HstDetHeapGetForeignCounts).
bool HstDetHeapInstall(size_t uiReserveBytes);
//! Patch one more module's kernel32 heap imports (the gamecore DLL after LoadLibrary). Returns slots patched.
int HstDetHeapPatchModule(HMODULE hModule);
bool HstDetHeapIsInstalled();
//! Host-only bookkeeping whose sizes depend on the machine/run (paths, log text) must not shift the deterministic
//! layout: while paused (per thread, nestable) new blocks come from the Windows heap. Frees route by address.
void HstDetHeapPause();
void HstDetHeapResume();
struct HstDetHeapPauseScope
{
	HstDetHeapPauseScope() { HstDetHeapPause(); }
	~HstDetHeapPauseScope() { HstDetHeapResume(); }
};
bool HstDetHeapIsFixedBase();
void* HstDetHeapGetBase();
size_t HstDetHeapGetUsedBytes();		// bump pointer - base
size_t HstDetHeapGetLiveBytes();		// sum of live block sizes
unsigned int HstDetHeapGetAllocCount();
unsigned int HstDetHeapGetBumpCount();	// allocations that took fresh memory from the bump pointer (the rest reused a freed block)
//! Calls from threads other than the sim thread (all stay out of the deterministic layout).
void HstDetHeapGetForeignCounts(unsigned int& uiAllocs, unsigned int& uiFrees, unsigned int& uiReallocs);

#endif // HSTDETHEAP_H
