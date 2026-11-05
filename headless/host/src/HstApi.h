//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstApi.h
//!  \brief     Engine API call accounting shared by the generated stubs (gen/) and hand-written overrides.
//!
//!		Every engine method the DLL can call has an id (gen/HstApiIds.h) and a class from api/engine-api.csv:
//!		sink (no-op), query (constant), sim (hand-written, HstUnimplemented if missing), gen, unexpected.
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#pragma once
#ifndef HSTAPI_H
#define HSTAPI_H

#include "HstCommon.h"
#include "engine/gen/HstApiIds.h"

struct HstApiInfo
{
	const char* szFamily;
	const char* szMethod;
	const char* szIface;
	const char* szClass;
};

extern unsigned int g_auiHstApiCalls[NUM_HST_API];
extern const HstApiInfo g_akHstApiInfo[NUM_HST_API];

#define HST_API(id) (++g_auiHstApiCalls[id])

//! A sim-class method without a hand-written override was called: log with the caller stack and abort.
void HstUnimplemented(HstApiId eId);
//! A method missing from engine-api.csv was called: log once; abort under --strict.
void HstUnexpected(HstApiId eId);
//! Write "family::method class count" for every called method.
void HstApiDumpCounters(FILE* pkFile, bool bNonZeroOnly);
//! Under --strict, unexpected calls abort.
void HstApiSetStrict(bool bStrict);

template<class T> inline T HstZero() { return T(); }

#endif // HSTAPI_H
