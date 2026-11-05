//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstDigest.h
//!  \brief     Per-turn state digest (M3): xxh64 of the DLL's own serialization segments.
//!
//!		Segments, in order: "game" (ICvGame1::Write), "map" (ICvMap1::Write), "teamNN" (ICvTeam1::Write,
//!		NN = 00..MAX_TEAMS-1), "playerNN" (ICvPlayer1::Write, 00..MAX_PLAYERS-1), "supporting"
//!		(ICvGame1::WriteSupportingClassData: barbarians + goody huts).
//!
//!		digest.bin v1 (little-endian): "CSDG", u16 version=1, u16 flags=0, u32 n_segments,
//!		n_segments x (u16 len, utf8 name), then per completed turn: u32 turn, u64 hash[n_segments].
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#pragma once
#ifndef HSTDIGEST_H
#define HSTDIGEST_H

#include "HstCommon.h"

typedef unsigned __int64 HstHash64;

//! XXH64 (xxHash, Yann Collet; reimplemented from the published algorithm).
HstHash64 HstXxh64(const void* pData, size_t uiBytes, HstHash64 ui64Seed = 0);

class HstDigest
{
public:
	explicit HstDigest(ICvGameContext1* pkCtx);
	~HstDigest();

	const std::vector<std::string>& GetSegmentNames() const { return m_astrSegments; }
	int GetNumSegments() const { return (int)m_astrSegments.size(); }

	//! Serialize every segment and hash it. With a non-empty wstrDumpDir, also writes <dir>\<segment>.bin.
	//! uiTotalBytes receives the summed segment sizes.
	void Compute(std::vector<HstHash64>& aui64Hashes, const std::wstring& wstrDumpDir, unsigned int& uiTotalBytes);
	//! xxh64 over the concatenated per-segment hashes (one number per turn for logs).
	static HstHash64 Combine(const std::vector<HstHash64>& aui64Hashes);
	//! --trace-writes: a dump (Compute with a dump dir) also writes <segment>.trace (offset ranges -> return address of the
	//! writing DLL code) and sites.tsv (return address -> function, file, line from the DLL's PDB). The hashes do not change.
	static void SetTraceWrites(bool bTrace);
	//! Lockstep "mask-only" state: bytes whose writing call chain (first DLL frames found on the
	//! stack, as --trace-writes finds them) resolves to a function or source file containing one of the patterns
	//! (case-insensitive, e.g. "cvunitcycler.cpp"; matched against "function|file:line|", so "cvplot.cpp:11121|" names one
	//! source line of that DLL build) are left out of ComputeMasked. Needs the DLL's PDB next to it.
	static void SetMask(const std::vector<std::string>& astrPatterns);
	static bool HasMask();
	//! Like Compute, but without the masked bytes; uiMaskedBytes receives how many bytes were dropped. Dumps (+traces) go to
	//! wstrDumpDir as for Compute.
	void ComputeMasked(std::vector<HstHash64>& aui64Hashes, const std::wstring& wstrDumpDir, unsigned int& uiTotalBytes, unsigned int& uiMaskedBytes);

	//! Create digest.bin and write the header.
	bool OpenFile(const std::wstring& wstrPath);
	void AppendRecord(unsigned int uiTurn, const std::vector<HstHash64>& aui64Hashes);
	void CloseFile();

protected:
	void HashSegment(int iIndex, FMemoryStream& kStream, std::vector<HstHash64>& aui64Hashes, const std::wstring& wstrDumpDir, unsigned int& uiTotalBytes);
	template<class StreamT> void ComputeImpl(std::vector<HstHash64>& aui64Hashes, const std::wstring& wstrDumpDir, unsigned int& uiTotalBytes);

	ICvGameContext1* m_pkCtx;
	std::vector<std::string> m_astrSegments;
	std::set<unsigned int> m_kTraceSites;
	FILE* m_pkFile;
};

#endif // HSTDIGEST_H
