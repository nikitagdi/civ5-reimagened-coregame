//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstDigest.cpp
//!  \brief     Per-turn state digest: XXH64 + the DLL's Write() segments (see HstDigest.h for the file format).
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#include "HstCommon.h"
#include "diag/HstDigest.h"
#include "platform/HstPlatform.h"
#include "platform/HstDetHeap.h"

// ---- XXH64 -----------------------------------------------------------------------------------------------------------

static const HstHash64 s_ui64Prime1 = 0x9E3779B185EBCA87ULL;
static const HstHash64 s_ui64Prime2 = 0xC2B2AE3D27D4EB4FULL;
static const HstHash64 s_ui64Prime3 = 0x165667B19E3779F9ULL;
static const HstHash64 s_ui64Prime4 = 0x85EBCA77C2B2AE63ULL;
static const HstHash64 s_ui64Prime5 = 0x27D4EB2F165667C5ULL;

static inline HstHash64 HstRotl64(HstHash64 x, int r)
{
	return (x << r) | (x >> (64 - r));
}

static inline HstHash64 HstRead64(const unsigned char* p)
{
	HstHash64 v;
	memcpy(&v, p, 8);	// x86: little-endian
	return v;
}

static inline unsigned int HstRead32(const unsigned char* p)
{
	unsigned int v;
	memcpy(&v, p, 4);
	return v;
}

static inline HstHash64 HstXxhRound(HstHash64 ui64Acc, HstHash64 ui64Input)
{
	ui64Acc += ui64Input * s_ui64Prime2;
	ui64Acc = HstRotl64(ui64Acc, 31);
	return ui64Acc * s_ui64Prime1;
}

static inline HstHash64 HstXxhMerge(HstHash64 ui64Acc, HstHash64 ui64Val)
{
	ui64Acc ^= HstXxhRound(0, ui64Val);
	return ui64Acc * s_ui64Prime1 + s_ui64Prime4;
}

HstHash64 HstXxh64(const void* pData, size_t uiBytes, HstHash64 ui64Seed)
{
	const unsigned char* p = (const unsigned char*)pData;
	const unsigned char* const pEnd = p + uiBytes;
	HstHash64 h;
	if(uiBytes >= 32)
	{
		const unsigned char* const pLimit = pEnd - 32;
		HstHash64 v1 = ui64Seed + s_ui64Prime1 + s_ui64Prime2;
		HstHash64 v2 = ui64Seed + s_ui64Prime2;
		HstHash64 v3 = ui64Seed;
		HstHash64 v4 = ui64Seed - s_ui64Prime1;
		do
		{
			v1 = HstXxhRound(v1, HstRead64(p)); p += 8;
			v2 = HstXxhRound(v2, HstRead64(p)); p += 8;
			v3 = HstXxhRound(v3, HstRead64(p)); p += 8;
			v4 = HstXxhRound(v4, HstRead64(p)); p += 8;
		}
		while(p <= pLimit);
		h = HstRotl64(v1, 1) + HstRotl64(v2, 7) + HstRotl64(v3, 12) + HstRotl64(v4, 18);
		h = HstXxhMerge(h, v1);
		h = HstXxhMerge(h, v2);
		h = HstXxhMerge(h, v3);
		h = HstXxhMerge(h, v4);
	}
	else
	{
		h = ui64Seed + s_ui64Prime5;
	}
	h += (HstHash64)uiBytes;
	while(p + 8 <= pEnd)
	{
		h ^= HstXxhRound(0, HstRead64(p));
		h = HstRotl64(h, 27) * s_ui64Prime1 + s_ui64Prime4;
		p += 8;
	}
	if(p + 4 <= pEnd)
	{
		h ^= (HstHash64)HstRead32(p) * s_ui64Prime1;
		h = HstRotl64(h, 23) * s_ui64Prime2 + s_ui64Prime3;
		p += 4;
	}
	while(p < pEnd)
	{
		h ^= (*p) * s_ui64Prime5;
		h = HstRotl64(h, 11) * s_ui64Prime1;
		++p;
	}
	h ^= h >> 33;
	h *= s_ui64Prime2;
	h ^= h >> 29;
	h *= s_ui64Prime3;
	h ^= h >> 32;
	return h;
}

// ---- write trace (--trace-writes): which line of the DLL wrote the byte at offset X of a segment ---------------------------

extern "C" void* _ReturnAddress(void);
#pragma intrinsic(_ReturnAddress)

// FireWorksWin32.lib does not define FDataStream::RaiseException (FMemoryStream's own vtable is prebuilt there); a stream
// class compiled here needs the vtable slot to resolve. Only reached on stream over/underflow, which the DLL never does.
void FDataStream::RaiseException(std::exception& kException) const
{
	throw kException;
}

extern "C" void* _AddressOfReturnAddress(void);
#pragma intrinsic(_AddressOfReturnAddress)

enum { HST_TRACE_DEPTH = 4 };

struct HstTraceRange
{
	unsigned int uiOffset;				// first byte of the run in the segment stream
	unsigned int uiSize;				// bytes of the run
	unsigned int uiCount;				// WriteIt calls merged into the run (same call chain, contiguous)
	unsigned int auiRet[HST_TRACE_DEPTH];	// return addresses, innermost first: [0] = caller of WriteIt (often the generic
										// FDataStream::Write<T>), then the callers found by scanning the stack for return
										// addresses into the DLL (the golden DLL has no frame pointers)
};

static const unsigned char* s_pTraceModBase = NULL;
static const unsigned char* s_pTraceModEnd = NULL;

//! A code address that follows a call instruction (E8 rel32 / FF /2 in the usual encodings).
static bool HstTraceLooksLikeReturn(const unsigned char* pRet)
{
	if(pRet[-5] == 0xE8)
		return true;
	if(pRet[-2] == 0xFF && (pRet[-1] & 0x38) == 0x10)
		return true;
	if(pRet[-3] == 0xFF && (pRet[-2] & 0x38) == 0x10)
		return true;
	if(pRet[-6] == 0xFF && (pRet[-5] & 0x38) == 0x10)
		return true;
	return false;
}

// Mask patterns (lower case) and the per-return-address verdict cache (host memory, paused heap).
static std::vector<std::string> s_astrHstMask;
static std::map<unsigned int, bool> s_kHstMaskCache;
static unsigned int s_uiHstMaskedBytes = 0;

static bool HstMaskedAddress(unsigned int uiRet)
{
	std::map<unsigned int, bool>::const_iterator it = s_kHstMaskCache.find(uiRet);
	if(it != s_kHstMaskCache.end())
		return it->second;
	HstDetHeapPauseScope kPause;
	std::string strFunction, strFile;
	unsigned int uiLine = 0, uiRva = 0;
	bool bMasked = false;
	if(HstResolveAddress((const void*)(DWORD_PTR)(uiRet - 1), strFunction, strFile, uiLine, uiRva))
	{
		char szLine[16];
		_snprintf_s(szLine, sizeof(szLine), _TRUNCATE, ":%u|", uiLine);
		std::string strKey = strFunction + "|" + strFile + szLine;	// "function|file:line|" (a pattern may name a line)
		for(size_t i = 0; i < strKey.size(); ++i)
			strKey[i] = (char)tolower((unsigned char)strKey[i]);
		for(size_t i = 0; i < s_astrHstMask.size() && !bMasked; ++i)
			bMasked = strKey.find(s_astrHstMask[i]) != std::string::npos;
	}
	s_kHstMaskCache[uiRet] = bMasked;
	return bMasked;
}

//! FMemoryStream that records the callers of every WriteIt. Same allocation behaviour as FMemoryStream (the trace itself is
//! kept off the deterministic heap), so a traced dump leaves the digests of the run unchanged. With m_bMask, writes whose
//! call chain hits a mask pattern are dropped (lockstep masked digest).
class HstTraceStream : public FMemoryStream
{
public:
	HstTraceStream() : m_bRecord(true), m_bMask(false) {}
	bool m_bRecord;
	bool m_bMask;
	virtual unsigned int WriteIt(unsigned int uiByteCount, const void* pBuffer)
	{
		const unsigned int uiPos = (unsigned int)(m_pData - m_pStart);
		HstTraceRange kRange = { uiPos, uiByteCount, 1, { 0, 0, 0, 0 } };
		const void* const* ppStack = (const void* const*)_AddressOfReturnAddress();
		kRange.auiRet[0] = (unsigned int)(DWORD_PTR)ppStack[0];
		if(!s_pTraceModBase)
		{
			HMODULE hModule = NULL;
			if(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ppStack[0], &hModule) && hModule)
			{
				const IMAGE_DOS_HEADER* pkDos = (const IMAGE_DOS_HEADER*)hModule;
				const IMAGE_NT_HEADERS* pkNt = (const IMAGE_NT_HEADERS*)((const BYTE*)hModule + pkDos->e_lfanew);
				s_pTraceModBase = (const unsigned char*)hModule;
				s_pTraceModEnd = s_pTraceModBase + pkNt->OptionalHeader.SizeOfImage;
			}
		}
		if(s_pTraceModBase)
		{
			int iFound = 1;
			for(int i = 1; i < 64 && iFound < HST_TRACE_DEPTH; ++i)
			{
				const unsigned char* pWord = (const unsigned char*)ppStack[i];
				if(pWord >= s_pTraceModBase + 8 && pWord < s_pTraceModEnd && HstTraceLooksLikeReturn(pWord))
					kRange.auiRet[iFound++] = (unsigned int)(DWORD_PTR)pWord;
			}
		}
		if(m_bMask)
		{
			for(int j = 0; j < HST_TRACE_DEPTH; ++j)
			{
				if(kRange.auiRet[j] && HstMaskedAddress(kRange.auiRet[j]))
				{
					s_uiHstMaskedBytes += uiByteCount;
					return uiByteCount;
				}
			}
		}
		if(m_bRecord)
		{
			HstDetHeapPauseScope kPause;
			if(!m_akRanges.empty() && memcmp(m_akRanges.back().auiRet, kRange.auiRet, sizeof(kRange.auiRet)) == 0 &&
				m_akRanges.back().uiOffset + m_akRanges.back().uiSize == uiPos)
			{
				m_akRanges.back().uiSize += uiByteCount;
				++m_akRanges.back().uiCount;
			}
			else
				m_akRanges.push_back(kRange);
		}
		return FMemoryStream::WriteIt(uiByteCount, pBuffer);
	}
	std::vector<HstTraceRange> m_akRanges;
};

class HstMaskStream : public HstTraceStream
{
public:
	HstMaskStream() { m_bRecord = false; m_bMask = true; }
};

class HstMaskTraceStream : public HstTraceStream
{
public:
	HstMaskTraceStream() { m_bRecord = true; m_bMask = true; }
};

static bool s_bTraceWrites = false;

void HstDigest::SetTraceWrites(bool bTrace)
{
	s_bTraceWrites = bTrace;
}

void HstDigest::SetMask(const std::vector<std::string>& astrPatterns)
{
	HstDetHeapPauseScope kPause;
	s_astrHstMask.clear();
	s_kHstMaskCache.clear();
	for(size_t i = 0; i < astrPatterns.size(); ++i)
	{
		std::string str = astrPatterns[i];
		for(size_t j = 0; j < str.size(); ++j)
			str[j] = (char)tolower((unsigned char)str[j]);
		if(!str.empty())
			s_astrHstMask.push_back(str);
	}
}

bool HstDigest::HasMask()
{
	return !s_astrHstMask.empty();
}

void HstDigest::ComputeMasked(std::vector<HstHash64>& aui64Hashes, const std::wstring& wstrDumpDir, unsigned int& uiTotalBytes, unsigned int& uiMaskedBytes)
{
	aui64Hashes.assign(m_astrSegments.size(), 0);
	uiTotalBytes = 0;
	s_uiHstMaskedBytes = 0;
	if(!wstrDumpDir.empty())
	{
		HstDetHeapPauseScope kPause;
		HstCreateDirs(wstrDumpDir);
	}
	if(!wstrDumpDir.empty() && s_bTraceWrites)
	{
		{
			HstDetHeapPauseScope kPause;
			m_kTraceSites.clear();
		}
		ComputeImpl<HstMaskTraceStream>(aui64Hashes, wstrDumpDir, uiTotalBytes);
		HstDetHeapPauseScope kPause;
		FILE* pkFile = _wfopen(HstJoinPath(wstrDumpDir, L"sites.tsv").c_str(), L"w");
		if(pkFile)
		{
			fprintf(pkFile, "addr\trva\tfunction\tfile\tline\n");
			for(std::set<unsigned int>::const_iterator it = m_kTraceSites.begin(); it != m_kTraceSites.end(); ++it)
			{
				std::string strFunction, strFile;
				unsigned int uiLine = 0, uiRva = 0;
				HstResolveAddress((const void*)(DWORD_PTR)(*it - 1), strFunction, strFile, uiLine, uiRva);
				fprintf(pkFile, "%08x\t%08x\t%s\t%s\t%u\n", *it, uiRva + 1, strFunction.c_str(), strFile.c_str(), uiLine);
			}
			fclose(pkFile);
		}
		m_kTraceSites.clear();
	}
	else
		ComputeImpl<HstMaskStream>(aui64Hashes, wstrDumpDir, uiTotalBytes);
	uiMaskedBytes = s_uiHstMaskedBytes;
}

// ---- HstDigest -------------------------------------------------------------------------------------------------------

HstDigest::HstDigest(ICvGameContext1* pkCtx)
	: m_pkCtx(pkCtx)
	, m_pkFile(NULL)
{
	m_astrSegments.push_back("game");
	m_astrSegments.push_back("map");
	char szName[32];
	for(int i = 0; i < MAX_TEAMS; ++i)
	{
		_snprintf_s(szName, sizeof(szName), _TRUNCATE, "team%02d", i);
		m_astrSegments.push_back(szName);
	}
	for(int i = 0; i < MAX_PLAYERS; ++i)
	{
		_snprintf_s(szName, sizeof(szName), _TRUNCATE, "player%02d", i);
		m_astrSegments.push_back(szName);
	}
	m_astrSegments.push_back("supporting");
}

HstDigest::~HstDigest()
{
	CloseFile();
}

void HstDigest::HashSegment(int iIndex, FMemoryStream& kStream, std::vector<HstHash64>& aui64Hashes, const std::wstring& wstrDumpDir, unsigned int& uiTotalBytes)
{
	const unsigned int uiBytes = kStream.GetEOF();
	aui64Hashes[iIndex] = HstXxh64(kStream.GetBuf(), uiBytes);
	uiTotalBytes += uiBytes;
	if(!wstrDumpDir.empty())
	{
		HstDetHeapPauseScope kPause;
		std::wstring wstrPath = HstJoinPath(wstrDumpDir, HstFromUtf8(m_astrSegments[iIndex] + ".bin"));
		FILE* pkFile = _wfopen(wstrPath.c_str(), L"wb");
		if(pkFile)
		{
			fwrite(kStream.GetBuf(), 1, uiBytes, pkFile);
			fclose(pkFile);
		}
		const HstTraceStream* pkTrace = dynamic_cast<const HstTraceStream*>(&kStream);
		if(pkTrace && pkTrace->m_bRecord)
		{
			// <segment>.trace: u32 n, n x { u32 offset, u32 size, u32 calls, u32 return address x 4 } (28 bytes)
			wstrPath = HstJoinPath(wstrDumpDir, HstFromUtf8(m_astrSegments[iIndex] + ".trace"));
			pkFile = _wfopen(wstrPath.c_str(), L"wb");
			if(pkFile)
			{
				const unsigned int uiCount = (unsigned int)pkTrace->m_akRanges.size();
				fwrite(&uiCount, 4, 1, pkFile);
				if(uiCount)
					fwrite(&pkTrace->m_akRanges[0], sizeof(HstTraceRange), uiCount, pkFile);
				fclose(pkFile);
			}
			for(size_t i = 0; i < pkTrace->m_akRanges.size(); ++i)
				for(int j = 0; j < HST_TRACE_DEPTH; ++j)
					if(pkTrace->m_akRanges[i].auiRet[j])
						m_kTraceSites.insert(pkTrace->m_akRanges[i].auiRet[j]);
		}
	}
}

template<class StreamT> void HstDigest::ComputeImpl(std::vector<HstHash64>& aui64Hashes, const std::wstring& wstrDumpDir, unsigned int& uiTotalBytes)
{
	std::auto_ptr<ICvGame1> pkGame(m_pkCtx->GetGame());
	int iIndex = 0;
	{
		StreamT kStream;
		pkGame->Write(kStream);
		HashSegment(iIndex++, kStream, aui64Hashes, wstrDumpDir, uiTotalBytes);
	}
	{
		std::auto_ptr<ICvMap1> pkMap(m_pkCtx->GetMap());
		StreamT kStream;
		pkMap->Write(kStream);
		HashSegment(iIndex++, kStream, aui64Hashes, wstrDumpDir, uiTotalBytes);
	}
	for(int i = 0; i < MAX_TEAMS; ++i)
	{
		std::auto_ptr<ICvTeam1> pkTeam(m_pkCtx->GetTeam((TeamTypes)i));
		StreamT kStream;
		if(pkTeam.get())
			pkTeam->Write(kStream);
		HashSegment(iIndex++, kStream, aui64Hashes, wstrDumpDir, uiTotalBytes);
	}
	for(int i = 0; i < MAX_PLAYERS; ++i)
	{
		std::auto_ptr<ICvPlayer1> pkPlayer(pkGame->GetPlayer((PlayerTypes)i));
		StreamT kStream;
		if(pkPlayer.get())
			pkPlayer->Write(kStream);
		HashSegment(iIndex++, kStream, aui64Hashes, wstrDumpDir, uiTotalBytes);
	}
	{
		StreamT kStream;
		pkGame->WriteSupportingClassData(kStream);
		HashSegment(iIndex++, kStream, aui64Hashes, wstrDumpDir, uiTotalBytes);
	}
}

void HstDigest::Compute(std::vector<HstHash64>& aui64Hashes, const std::wstring& wstrDumpDir, unsigned int& uiTotalBytes)
{
	aui64Hashes.assign(m_astrSegments.size(), 0);
	uiTotalBytes = 0;
	if(!wstrDumpDir.empty())
	{
		HstDetHeapPauseScope kPause;
		HstCreateDirs(wstrDumpDir);
	}
	if(!wstrDumpDir.empty() && s_bTraceWrites)
	{
		{
			HstDetHeapPauseScope kPause;
			m_kTraceSites.clear();
		}
		ComputeImpl<HstTraceStream>(aui64Hashes, wstrDumpDir, uiTotalBytes);
		// sites.tsv: every distinct return address of the traced writes resolved through the PDB of the DLL.
		HstDetHeapPauseScope kPause;
		FILE* pkFile = _wfopen(HstJoinPath(wstrDumpDir, L"sites.tsv").c_str(), L"w");
		if(pkFile)
		{
			fprintf(pkFile, "addr\trva\tfunction\tfile\tline\n");
			for(std::set<unsigned int>::const_iterator it = m_kTraceSites.begin(); it != m_kTraceSites.end(); ++it)
			{
				std::string strFunction, strFile;
				unsigned int uiLine = 0, uiRva = 0;
				// A return address belongs to the instruction AFTER the call, which can start the next source line: look up addr - 1.
				HstResolveAddress((const void*)(DWORD_PTR)(*it - 1), strFunction, strFile, uiLine, uiRva);
				fprintf(pkFile, "%08x\t%08x\t%s\t%s\t%u\n", *it, uiRva + 1, strFunction.c_str(), strFile.c_str(), uiLine);
			}
			fclose(pkFile);
		}
		m_kTraceSites.clear();
	}
	else
		ComputeImpl<FMemoryStream>(aui64Hashes, wstrDumpDir, uiTotalBytes);
}

HstHash64 HstDigest::Combine(const std::vector<HstHash64>& aui64Hashes)
{
	return aui64Hashes.empty() ? 0 : HstXxh64(&aui64Hashes[0], aui64Hashes.size() * sizeof(HstHash64));
}

bool HstDigest::OpenFile(const std::wstring& wstrPath)
{
	HstDetHeapPauseScope kPause;
	CloseFile();
	m_pkFile = _wfopen(wstrPath.c_str(), L"wb");
	if(!m_pkFile)
		return false;
	const unsigned short usVersion = 1;
	const unsigned short usFlags = 0;
	const unsigned int uiCount = (unsigned int)m_astrSegments.size();
	fwrite("CSDG", 1, 4, m_pkFile);
	fwrite(&usVersion, 2, 1, m_pkFile);
	fwrite(&usFlags, 2, 1, m_pkFile);
	fwrite(&uiCount, 4, 1, m_pkFile);
	for(size_t i = 0; i < m_astrSegments.size(); ++i)
	{
		const unsigned short usLen = (unsigned short)m_astrSegments[i].size();
		fwrite(&usLen, 2, 1, m_pkFile);
		fwrite(m_astrSegments[i].data(), 1, usLen, m_pkFile);
	}
	fflush(m_pkFile);
	return true;
}

void HstDigest::AppendRecord(unsigned int uiTurn, const std::vector<HstHash64>& aui64Hashes)
{
	if(!m_pkFile)
		return;
	fwrite(&uiTurn, 4, 1, m_pkFile);
	if(!aui64Hashes.empty())
		fwrite(&aui64Hashes[0], sizeof(HstHash64), aui64Hashes.size(), m_pkFile);
	fflush(m_pkFile);	// a crash later still leaves every completed turn on disk
}

void HstDigest::CloseFile()
{
	if(m_pkFile)
	{
		fclose(m_pkFile);
		m_pkFile = NULL;
	}
}
