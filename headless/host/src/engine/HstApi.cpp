//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstApi.cpp
//!  \brief     Unimplemented / unexpected engine call handling and call counters.
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#include "HstCommon.h"
#include "HstApi.h"
#include "platform/HstPlatform.h"

static bool s_bStrict = false;

void HstApiSetStrict(bool bStrict)
{
	s_bStrict = bStrict;
}

void HstUnimplemented(HstApiId eId)
{
	const HstApiInfo& kInfo = g_akHstApiInfo[eId];
	HstFatal(4, "civhost: FATAL sim method without implementation: %s::%s (%s) - add an override in engine/Hst*.cpp",
		kInfo.szFamily, kInfo.szMethod, kInfo.szIface);
}

void HstUnexpected(HstApiId eId)
{
	const HstApiInfo& kInfo = g_akHstApiInfo[eId];
	if(s_bStrict)
		HstFatal(5, "civhost: FATAL (--strict) unexpected engine call %s::%s - classify it in api/engine-api.csv", kInfo.szFamily, kInfo.szMethod);
	if(g_auiHstApiCalls[eId] == 1)
		HstLog("civhost: WARNING unexpected engine call %s::%s (not in engine-api.csv)", kInfo.szFamily, kInfo.szMethod);
}

void HstApiDumpCounters(FILE* pkFile, bool bNonZeroOnly)
{
	for(int i = 0; i < NUM_HST_API; ++i)
	{
		if(bNonZeroOnly && g_auiHstApiCalls[i] == 0)
			continue;
		fprintf(pkFile, "%s::%s\t%s\t%u\n", g_akHstApiInfo[i].szFamily, g_akHstApiInfo[i].szMethod, g_akHstApiInfo[i].szClass, g_auiHstApiCalls[i]);
	}
}
