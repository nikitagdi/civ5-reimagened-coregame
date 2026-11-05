//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstRngLog.cpp
//!  \brief     Sink for the diag DLL's HeadlessDiag hooks: RNG stream digest per turn, windowed RNG call log, site table.
//!
//!		Constraints (HeadlessDiag.h consumer note): called synchronously on the sim thread from inside RNG / zone code,
//!		so no DLL calls, no exceptions, no floating point, no deterministic-heap allocation (it would change the heap
//!		layout of diag runs against golden runs). Everything the hot path touches is static storage; files are written
//!		with raw Win32 handles from a static buffer; the site table is resolved only in Flush (heap paused).
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#include "HstCommon.h"
#include "diag/HstRngLog.h"
#include "diag/HstDiagSink.h"
#include "platform/HstPlatform.h"
#include "platform/HstDetHeap.h"

#pragma pack(push, 1)
//! rng.log.bin record, 20 bytes (the record with 16-bit turn/slice: the spec's field list adds up to
//! 24 bytes). usTurn 0xFFFF = pregame. ucRng: 0 jon, 1 map, 2 async, 3 other sync, 4 other async; bit 0x80 = reseed record
//! (uiCallIndex = reseed count of the kind in the phase, uiSeed = new seed, range/result 0). ucZone: innermost HeadlessZone
//! (0xFF none). usSite indexes rng.sites.tsv (0xFFFF = table full / reseed).
struct HstRngLogRecord
{
	unsigned short usTurn;
	unsigned short usSlice;
	unsigned char ucRng;
	unsigned char ucZone;
	unsigned short usSite;
	unsigned int uiCallIndex;
	unsigned int uiSeed;
	unsigned short usRange;
	unsigned short usResult;
};
#pragma pack(pop)
typedef char HstRngLogRecordIs20Bytes[sizeof(HstRngLogRecord) == 20 ? 1 : -1];

namespace
{
const int KINDS = 5;
const unsigned int MAX_SITES = 65535;
const unsigned int TABLE_SIZE = 1u << 17;
const unsigned int TEXT_POOL_BYTES = 4u * 1024 * 1024;
const unsigned int LOG_BUF_BYTES = 1u << 20;
const unsigned int MAX_ZONE_DEPTH = 64;
const unsigned __int64 MAX_LOG_BYTES = 1500ULL * 1024 * 1024;

struct KindAcc
{
	unsigned int uiCalls;
	unsigned int uiReseeds;
	unsigned int uiFirstSeed;
	unsigned int uiLastSeed;
	unsigned __int64 ui64Hash;
};

//! rng-digest.bin record: 128 bytes. flags: 1 pregame phase, 2 calls outside any phase.
struct DigestRecord
{
	int iTurn;
	unsigned int uiFlags;
	KindAcc akKinds[KINDS];
};
typedef char HstRngDigestRecordIs128Bytes[sizeof(DigestRecord) == 128 ? 1 : -1];

struct Site
{
	const void* pRet;
	unsigned int uiLogHash;
	unsigned int uiCount;
	unsigned int uiTextOff;
	unsigned int uiKind;
};

HstRngLogOptions s_kOpt;
bool s_bInstalled = false;
DWORD s_dwSimThread = 0;
int s_iGameId = 0;

// phase state
bool s_bInPhase = false;
bool s_bLogging = false;
int s_iPhase = -1;
unsigned int s_uiSlice = 0;
KindAcc s_akAcc[KINDS];
unsigned int s_uiForeignCalls = 0;

// zones
unsigned char s_aucZones[MAX_ZONE_DEPTH];
unsigned int s_uiZoneDepth = 0;

// files
HANDLE s_hDigest = INVALID_HANDLE_VALUE;
HANDLE s_hLog = INVALID_HANDLE_VALUE;
std::wstring* s_pwstrSitesPath = NULL;		// allocated with the heap paused (never freed)
unsigned char s_abLogBuf[LOG_BUF_BYTES];
unsigned int s_uiLogFill = 0;
unsigned __int64 s_ui64LogBytes = 0;
unsigned __int64 s_ui64LogCalls = 0;
unsigned int s_uiDigestRecords = 0;
bool s_bLogFull = false;
bool s_bSitesDirty = false;
unsigned __int64 s_ui64SummaryKey = 0;

// call sites
Site s_akSites[MAX_SITES];
unsigned int s_uiNumSites = 0;
unsigned int s_uiSitesDropped = 0;
unsigned short s_ausTable[TABLE_SIZE];		// site index + 1, 0 = empty
char s_acTextPool[TEXT_POOL_BYTES];
unsigned int s_uiTextFill = 1;				// offset 0 is the empty string

unsigned int s_auiFixTriggers[1024];

inline unsigned __int64 Mix(unsigned __int64 ui64Hash, unsigned __int64 ui64Value)
{
	ui64Hash = (ui64Hash ^ ui64Value) * 0xFF51AFD7ED558CCDULL;
	return ui64Hash ^ (ui64Hash >> 32);
}

void ResetAccumulators()
{
	for(int i = 0; i < KINDS; ++i)
	{
		s_akAcc[i].uiCalls = 0;
		s_akAcc[i].uiReseeds = 0;
		s_akAcc[i].uiFirstSeed = 0;
		s_akAcc[i].uiLastSeed = 0;
		s_akAcc[i].ui64Hash = 0x243F6A8885A308D3ULL;
	}
}

bool AnyCalls()
{
	for(int i = 0; i < KINDS; ++i)
		if(s_akAcc[i].uiCalls || s_akAcc[i].uiReseeds)
			return true;
	return false;
}

void FlushLogBuf()
{
	if(s_hLog != INVALID_HANDLE_VALUE && s_uiLogFill)
	{
		DWORD dwWritten = 0;
		WriteFile(s_hLog, s_abLogBuf, s_uiLogFill, &dwWritten, NULL);
		s_ui64LogBytes += s_uiLogFill;
	}
	s_uiLogFill = 0;
}

inline HstRngLogRecord* NextLogRecord()
{
	if(s_bLogFull)
		return NULL;
	if(s_uiLogFill + sizeof(HstRngLogRecord) > LOG_BUF_BYTES)
	{
		FlushLogBuf();
		if(s_ui64LogBytes >= MAX_LOG_BYTES)
		{
			s_bLogFull = true;
			return NULL;
		}
	}
	HstRngLogRecord* pk = (HstRngLogRecord*)(s_abLogBuf + s_uiLogFill);
	s_uiLogFill += sizeof(HstRngLogRecord);
	++s_ui64LogCalls;
	return pk;
}

inline unsigned short TurnField()
{
	return s_iPhase < 0 ? (unsigned short)0xFFFF : (unsigned short)(s_iPhase > 0xFFFE ? 0xFFFE : s_iPhase);
}

inline unsigned short SliceField()
{
	return (unsigned short)(s_uiSlice > 0xFFFF ? 0xFFFF : s_uiSlice);
}

inline unsigned char ZoneField()
{
	if(!s_uiZoneDepth)
		return 0xFF;
	return s_aucZones[(s_uiZoneDepth > MAX_ZONE_DEPTH ? MAX_ZONE_DEPTH : s_uiZoneDepth) - 1];
}

unsigned short SiteIdFor(int iKind, const void* pRet, const char* pszLog)
{
	unsigned int uiHash = 0;
	if(pszLog)
	{
		uiHash = 2166136261u;
		const unsigned char* puc = (const unsigned char*)pszLog;
		for(unsigned int i = 0; i < 255 && puc[i]; ++i)
		{
			uiHash ^= puc[i];
			uiHash *= 16777619u;
		}
	}
	const unsigned int uiKey = ((unsigned int)(DWORD_PTR)pRet * 2654435761u) ^ (uiHash * 40503u) ^ ((unsigned int)iKind * 0x9E3779B1u);
	unsigned int uiSlot = (uiKey ^ (uiKey >> 15)) & (TABLE_SIZE - 1);
	for(;;)
	{
		const unsigned short usEntry = s_ausTable[uiSlot];
		if(!usEntry)
			break;
		Site& kSite = s_akSites[usEntry - 1];
		if(kSite.pRet == pRet && kSite.uiLogHash == uiHash && kSite.uiKind == (unsigned int)iKind)
		{
			++kSite.uiCount;
			return (unsigned short)(usEntry - 1);
		}
		uiSlot = (uiSlot + 1) & (TABLE_SIZE - 1);
	}
	if(s_uiNumSites >= MAX_SITES)
	{
		++s_uiSitesDropped;
		return 0xFFFF;
	}
	Site& kNew = s_akSites[s_uiNumSites];
	kNew.pRet = pRet;
	kNew.uiLogHash = uiHash;
	kNew.uiCount = 1;
	kNew.uiKind = (unsigned int)iKind;
	kNew.uiTextOff = 0;
	if(pszLog && pszLog[0])
	{
		size_t uiLen = 0;
		while(uiLen < 127 && pszLog[uiLen])
			++uiLen;
		if(s_uiTextFill + uiLen + 1 <= TEXT_POOL_BYTES)
		{
			memcpy(s_acTextPool + s_uiTextFill, pszLog, uiLen);
			s_acTextPool[s_uiTextFill + uiLen] = 0;
			kNew.uiTextOff = s_uiTextFill;
			s_uiTextFill += (unsigned int)uiLen + 1;
		}
	}
	s_ausTable[uiSlot] = (unsigned short)(s_uiNumSites + 1);
	return (unsigned short)s_uiNumSites++;
}

void WriteDigestRecord(int iTurn, unsigned int uiFlags)
{
	if(s_hDigest == INVALID_HANDLE_VALUE)
		return;
	DigestRecord kRecord;
	kRecord.iTurn = iTurn;
	kRecord.uiFlags = uiFlags;
	for(int i = 0; i < KINDS; ++i)
		kRecord.akKinds[i] = s_akAcc[i];
	DWORD dwWritten = 0;
	WriteFile(s_hDigest, &kRecord, sizeof(kRecord), &dwWritten, NULL);
	++s_uiDigestRecords;
}

class HstRngSink : public IHstDiagSink1
{
public:
	virtual unsigned int GetVersion() { return HST_DIAG_VERSION; }

	virtual void OnRandCall(int eKind, const void* pRng, unsigned long ulSeedBefore, unsigned long ulCallCountBefore, unsigned int uiRange,
		unsigned int uiResult, const char* pszLog, const void* pRetAddr)
	{
		if(GetCurrentThreadId() != s_dwSimThread)
		{
			++s_uiForeignCalls;
			return;
		}
		const int iKind = (unsigned int)eKind < (unsigned int)KINDS ? eKind : KINDS - 1;
		KindAcc& kAcc = s_akAcc[iKind];
		if(!kAcc.uiCalls)
			kAcc.uiFirstSeed = (unsigned int)ulSeedBefore;
		++kAcc.uiCalls;
		kAcc.uiLastSeed = (unsigned int)ulSeedBefore;
		kAcc.ui64Hash = Mix(Mix(kAcc.ui64Hash, ((unsigned __int64)uiRange << 32) | uiResult), ulSeedBefore);
		if(s_bLogging)
		{
			HstRngLogRecord* pk = NextLogRecord();
			if(pk)
			{
				pk->usTurn = TurnField();
				pk->usSlice = SliceField();
				pk->ucRng = (unsigned char)iKind;
				pk->ucZone = ZoneField();
				pk->usSite = SiteIdFor(iKind, pRetAddr, pszLog);
				pk->uiCallIndex = (unsigned int)ulCallCountBefore;
				pk->uiSeed = (unsigned int)ulSeedBefore;
				pk->usRange = (unsigned short)(uiRange > 0xFFFF ? 0xFFFF : uiRange);
				pk->usResult = (unsigned short)(uiResult > 0xFFFF ? 0xFFFF : uiResult);
				s_bSitesDirty = true;
			}
		}
	}

	virtual void OnRandReseed(int eKind, const void* pRng, unsigned long ulNewSeed)
	{
		if(GetCurrentThreadId() != s_dwSimThread)
			return;
		const int iKind = (unsigned int)eKind < (unsigned int)KINDS ? eKind : KINDS - 1;
		KindAcc& kAcc = s_akAcc[iKind];
		++kAcc.uiReseeds;
		kAcc.ui64Hash = Mix(kAcc.ui64Hash, 0xD00DFEED00000000ULL | (unsigned int)ulNewSeed);
		if(s_bLogging)
		{
			HstRngLogRecord* pk = NextLogRecord();
			if(pk)
			{
				pk->usTurn = TurnField();
				pk->usSlice = SliceField();
				pk->ucRng = (unsigned char)(iKind | 0x80);
				pk->ucZone = ZoneField();
				pk->usSite = 0xFFFF;
				pk->uiCallIndex = kAcc.uiReseeds;
				pk->uiSeed = (unsigned int)ulNewSeed;
				pk->usRange = 0;
				pk->usResult = 0;
			}
		}
	}

	virtual void OnZoneEnter(int eZone)
	{
		if(s_uiZoneDepth < MAX_ZONE_DEPTH)
			s_aucZones[s_uiZoneDepth] = (unsigned char)eZone;
		++s_uiZoneDepth;
	}

	virtual void OnZoneLeave(int eZone)
	{
		if(s_uiZoneDepth)
			--s_uiZoneDepth;
	}

	virtual void OnFixTrigger(int iFixId)
	{
		++s_auiFixTriggers[(unsigned int)iFixId & 1023u];
	}

	//! Same behaviour as the DLL without a sink (HeadlessOptVerifyMismatch in CvGameCoreDLL.cpp): stderr + exit code 86.
	virtual void OnOptVerifyMismatch(int iOptId, const char* pszWhat)
	{
		char szBuf[512];
		_snprintf(szBuf, sizeof(szBuf) - 1, "HEADLESS_OPT_VERIFY mismatch: OPT-%04d %s (game turn %d)\n", iOptId, pszWhat ? pszWhat : "", s_iPhase);
		szBuf[sizeof(szBuf) - 1] = 0;
		HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
		if(hErr != NULL && hErr != INVALID_HANDLE_VALUE)
		{
			DWORD dwWritten = 0;
			WriteFile(hErr, szBuf, (DWORD)strlen(szBuf), &dwWritten, NULL);
			FlushFileBuffers(hErr);
		}
		OutputDebugStringA(szBuf);
		TerminateProcess(GetCurrentProcess(), 86);
	}
};

HstRngSink s_kSink;

typedef int (__cdecl *PFN_HeadlessDiagInstall)(IHstDiagSink1* pSink);

const char* KindName(unsigned int uiKind)
{
	static const char* s_aszNames[KINDS] = { "jon", "map", "async", "other-sync", "other-async" };
	return uiKind < (unsigned int)KINDS ? s_aszNames[uiKind] : "?";
}

HANDLE CreateOutput(const std::wstring& wstrName)
{
	return CreateFileW(HstJoinPath(HstGetRunDir(), wstrName).c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

void WriteSites()
{
	if(!s_pwstrSitesPath)
		return;
	HstDetHeapPauseScope kPause;
	FILE* pkFile = _wfopen(s_pwstrSitesPath->c_str(), L"w");
	if(!pkFile)
		return;
	fprintf(pkFile, "site\tkind\tcalls\tret\trva\tfunction\tfile\tline\tlog\n");
	for(unsigned int i = 0; i < s_uiNumSites; ++i)
	{
		const Site& kSite = s_akSites[i];
		std::string strFunction, strFile;
		unsigned int uiLine = 0, uiRva = 0, uiDisp = 0;
		// The return address points behind the call; one byte back is inside the call instruction (same rule as sites.tsv).
		HstResolveAddress((const char*)kSite.pRet - 1, strFunction, strFile, uiLine, uiRva, &uiDisp);
		for(size_t k = 0; k < strFile.size(); ++k)
			if(strFile[k] == '\\')
				strFile[k] = '/';
		char szLog[160];
		const char* pszText = s_acTextPool + kSite.uiTextOff;
		size_t n = 0;
		for(; pszText[n] && n < sizeof(szLog) - 1; ++n)
			szLog[n] = (unsigned char)pszText[n] < 32 ? ' ' : pszText[n];
		szLog[n] = 0;
		if(strFunction.empty())
			fprintf(pkFile, "%u\t%s\t%u\t0x%08x\t0x%08x\t?\t%s\t%u\t%s\n", i, KindName(kSite.uiKind), kSite.uiCount, (unsigned int)(DWORD_PTR)kSite.pRet, uiRva,
				strFile.c_str(), uiLine, szLog);
		else
			fprintf(pkFile, "%u\t%s\t%u\t0x%08x\t0x%08x\t%s+0x%x\t%s\t%u\t%s\n", i, KindName(kSite.uiKind), kSite.uiCount, (unsigned int)(DWORD_PTR)kSite.pRet,
				uiRva, strFunction.c_str(), uiDisp, strFile.c_str(), uiLine, szLog);
	}
	fclose(pkFile);
	s_bSitesDirty = false;
}
} // namespace

bool HstRngLog::ParseWindow(const std::string& strWindow, int& iFrom, int& iTo)
{
	std::string str = strWindow;
	if(str.empty())
		return false;
	size_t uiDash = std::string::npos;
	if(str.compare(0, 3, "pre") == 0)
	{
		iFrom = -1;
		if(str.size() == 3)
		{
			iTo = -1;
			return true;
		}
		if(str[3] != '-')
			return false;
		uiDash = 3;
	}
	else
	{
		char* pcEnd = NULL;
		const long lFrom = strtol(str.c_str(), &pcEnd, 10);
		if(pcEnd == str.c_str() || lFrom < 0)
			return false;
		iFrom = (int)lFrom;
		if(*pcEnd == 0)
		{
			iTo = iFrom;
			return true;
		}
		if(*pcEnd != '-')
			return false;
		uiDash = (size_t)(pcEnd - str.c_str());
	}
	char* pcEnd = NULL;
	const long lTo = strtol(str.c_str() + uiDash + 1, &pcEnd, 10);
	if(pcEnd == str.c_str() + uiDash + 1 || *pcEnd != 0 || lTo < iFrom)
		return false;
	iTo = (int)lTo;
	return true;
}

bool HstRngLog::Install(HMODULE hDll, const HstRngLogOptions& kOpt)
{
	s_kOpt = kOpt;
	s_bInstalled = false;
	PFN_HeadlessDiagInstall pfnInstall = (PFN_HeadlessDiagInstall)GetProcAddress(hDll, "HeadlessDiagInstall");
	if(!pfnInstall)
	{
		HstLog("rng: the DLL exports no HeadlessDiagInstall (golden / optimized build): --rng-digest / --rng-log record nothing");
		return false;
	}
	s_dwSimThread = GetCurrentThreadId();
	ResetAccumulators();
	const int iVersion = pfnInstall(&s_kSink);
	if(iVersion != HST_DIAG_VERSION)
	{
		HstLog("rng: DLL diag interface version %d, host expects %d: sink not installed", iVersion, (int)HST_DIAG_VERSION);
		pfnInstall(NULL);
		return false;
	}
	s_bInstalled = true;
	HstLog("rng: diag sink installed (version %d)%s%s", iVersion, kOpt.bDigest ? ", rng-digest" : "", kOpt.bLog ? ", rng-log" : "");
	return true;
}

bool HstRngLog::IsInstalled()
{
	return s_bInstalled;
}

void HstRngLog::BeginGame(int iGameId)
{
	if(!s_bInstalled)
		return;
	EndGame();
	HstDetHeapPauseScope kPause;
	s_iGameId = iGameId;
	wchar_t wszName[48];
	DWORD dwWritten = 0;
	if(s_kOpt.bDigest)
	{
		if(iGameId <= 1)
			wcscpy_s(wszName, L"rng-digest.bin");
		else
			swprintf_s(wszName, L"rng-digest-%d.bin", iGameId);
		s_hDigest = CreateOutput(wszName);
		if(s_hDigest != INVALID_HANDLE_VALUE)
		{
			const unsigned int auiHeader[4] = { 0x474E5243u /* "CRNG" */, 1, (unsigned int)KINDS, (unsigned int)sizeof(DigestRecord) };
			WriteFile(s_hDigest, auiHeader, sizeof(auiHeader), &dwWritten, NULL);
		}
	}
	if(s_kOpt.bLog)
	{
		if(iGameId <= 1)
			wcscpy_s(wszName, L"rng.log.bin");
		else
			swprintf_s(wszName, L"rng-%d.log.bin", iGameId);
		s_hLog = CreateOutput(wszName);
		if(s_hLog != INVALID_HANDLE_VALUE)
		{
			const unsigned int auiHeader[4] = { 0x474C5243u /* "CRLG" */, 1, (unsigned int)sizeof(HstRngLogRecord), 0 };
			WriteFile(s_hLog, auiHeader, sizeof(auiHeader), &dwWritten, NULL);
			s_ui64LogBytes = sizeof(auiHeader);
		}
		if(iGameId <= 1)
			wcscpy_s(wszName, L"rng.sites.tsv");
		else
			swprintf_s(wszName, L"rng-%d.sites.tsv", iGameId);
		if(!s_pwstrSitesPath)
			s_pwstrSitesPath = new std::wstring();
		*s_pwstrSitesPath = HstJoinPath(HstGetRunDir(), wszName);
	}
	s_uiNumSites = 0;
	s_uiSitesDropped = 0;
	s_uiTextFill = 1;
	s_acTextPool[0] = 0;
	memset(s_ausTable, 0, sizeof(s_ausTable));
	s_uiLogFill = 0;
	s_ui64LogCalls = 0;
	s_uiDigestRecords = 0;
	s_bLogFull = false;
	s_bSitesDirty = false;
	s_ui64SummaryKey = 0;
	s_bInPhase = false;
	s_bLogging = false;
	s_uiZoneDepth = 0;
	memset(s_auiFixTriggers, 0, sizeof(s_auiFixTriggers));
	ResetAccumulators();
}

void HstRngLog::BeginPhase(int iTurn)
{
	if(!s_bInstalled)
		return;
	if(s_bInPhase)
		EndPhase();
	if(AnyCalls())
	{
		WriteDigestRecord(-2, 2);	// calls between phases (none expected)
		ResetAccumulators();
	}
	s_bInPhase = true;
	s_iPhase = iTurn;
	s_uiSlice = 0;
	s_bLogging = s_kOpt.bLog && s_hLog != INVALID_HANDLE_VALUE && iTurn >= s_kOpt.iLogFrom && iTurn <= s_kOpt.iLogTo;
}

void HstRngLog::NextSlice()
{
	++s_uiSlice;
}

void HstRngLog::EndPhase()
{
	if(!s_bInstalled || !s_bInPhase)
		return;
	WriteDigestRecord(s_iPhase, s_iPhase < 0 ? 1u : 0u);
	if(s_bLogging)
		FlushLogBuf();
	const bool bWindowDone = s_kOpt.bLog && s_iPhase >= s_kOpt.iLogTo;
	s_bInPhase = false;
	s_bLogging = false;
	ResetAccumulators();
	if(bWindowDone && s_bSitesDirty)
		WriteSites();
}

void HstRngLog::Flush()
{
	if(!s_bInstalled)
		return;
	FlushLogBuf();
	if(s_kOpt.bLog && s_bSitesDirty)
		WriteSites();
	const bool bAny = s_hDigest != INVALID_HANDLE_VALUE || s_hLog != INVALID_HANDLE_VALUE;
	const unsigned __int64 ui64Key = s_ui64LogCalls * 65537ULL + s_uiDigestRecords;
	if(bAny && ui64Key != s_ui64SummaryKey)
	{
		s_ui64SummaryKey = ui64Key;
		HstDetHeapPauseScope kPause;
		std::string strFix;
		for(unsigned int i = 0; i < 1024; ++i)
		{
			if(s_auiFixTriggers[i])
			{
				char sz[32];
				_snprintf_s(sz, sizeof(sz), _TRUNCATE, " %u:%u", i, s_auiFixTriggers[i]);
				strFix += sz;
			}
		}
		HstLog("rng: game %d: %u digest record(s), %I64u logged call(s) in %u site(s)%s%s; calls from other threads: %u; fix triggers:%s", s_iGameId,
			s_uiDigestRecords, s_ui64LogCalls, s_uiNumSites, s_uiSitesDropped ? " (site table overflowed)" : "", s_bLogFull ? " (log size cap reached)" : "",
			s_uiForeignCalls, strFix.empty() ? " none" : strFix.c_str());
	}
}

void HstRngLog::EndGame()
{
	if(!s_bInstalled)
		return;
	if(s_bInPhase)
		EndPhase();
	Flush();
	if(s_hDigest != INVALID_HANDLE_VALUE)
	{
		CloseHandle(s_hDigest);
		s_hDigest = INVALID_HANDLE_VALUE;
	}
	if(s_hLog != INVALID_HANDLE_VALUE)
	{
		CloseHandle(s_hLog);
		s_hLog = INVALID_HANDLE_VALUE;
	}
}
