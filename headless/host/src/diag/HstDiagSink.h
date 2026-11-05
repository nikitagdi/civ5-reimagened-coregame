//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstDiagSink.h
//!  \brief     Host side of the diag-DLL hook interface (LEKMOD_DLL/CvGameCoreDLL_Expansion2/HeadlessDiag.h).
//!
//!		The diag / optimized-diag gamecore DLLs export
//!			extern "C" int __cdecl HeadlessDiagInstall(ICvHeadlessDiagSink1* pSink);   // returns HEADLESS_DIAG_VERSION
//!		and call the sink synchronously on the sim thread from inside RNG / zone code. HeadlessDiag.h is only usable inside
//!		the DLL (it is compiled out without LEKMOD_HEADLESS_DIAG and needs the DLL's PCH), so the pure interface is repeated
//!		here. Keep method order and signatures identical: the vtable is the contract. Version 1.
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#pragma once
#ifndef HSTDIAGSINK_H
#define HSTDIAGSINK_H

#define HST_DIAG_VERSION 1

enum HstDiagRngKind
{
	HST_RNG_JON = 0,			// CvGame::m_jonRand (synchronous)
	HST_RNG_MAP = 1,			// CvGame::m_mapRand (synchronous)
	HST_RNG_ASYNC = 2,			// GC.getASyncRand()
	HST_RNG_OTHER_SYNC = 3,
	HST_RNG_OTHER_ASYNC = 4,
};

//! Mirror of ICvHeadlessDiagSink1 (HeadlessDiag.h). Do not reorder.
class IHstDiagSink1
{
public:
	virtual unsigned int GetVersion() = 0;
	virtual void OnRandCall(int eKind, const void* pRng, unsigned long ulSeedBefore, unsigned long ulCallCountBefore,
		unsigned int uiRange, unsigned int uiResult, const char* pszLog, const void* pRetAddr) = 0;
	virtual void OnRandReseed(int eKind, const void* pRng, unsigned long ulNewSeed) = 0;
	virtual void OnZoneEnter(int eZone) = 0;
	virtual void OnZoneLeave(int eZone) = 0;
	virtual void OnFixTrigger(int iFixId) = 0;
	virtual void OnOptVerifyMismatch(int iOptId, const char* pszWhat) = 0;
};

#endif // HSTDIAGSINK_H
