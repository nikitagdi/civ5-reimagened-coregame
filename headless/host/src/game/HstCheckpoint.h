//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstCheckpoint.h
//!  \brief     civhost checkpoint file (M4b): host header + the DLL's own serialization, in the exe's order.
//!
//!		Order (exe Resync_LoadGameFromStreamImpl 0x2b6230 -> 0x31bfe0, read-only Ghidra): pregame
//!		(ICvPreGame1::read +0x164 / write), then ICvGame1::Read (+0x12c), ICvMap1::Read (+0x44), ICvTeam1::Read
//!		(+0x40) for teams 0..63, ICvPlayer1::Read (+0xbc) for players 0..63, ICvMap1::RecalculateLandmasses (+0x40),
//!		ICvGame1::ReadSupportingClassData (+0x134), ICvMap1::Validate (+0x4c), ICvGame1::LogGameState(true) (+0xd8).
//!		The exe keeps everything in one zlib stream after its own .Civ5Save header; civhost stores one section per
//!		call instead (same bytes per call, plus a size and an xxh64 per section), so a Read that consumes more or
//!		fewer bytes than its Write produced is detected.
//!
//!		File (little-endian): "CHCK", u32 version=1, u32 n_sections, then per section: u16 name_len, name (utf8),
//!		u32 size, u64 xxh64(bytes), bytes. Sections: "header" (key=value text lines), "host" (virtual clock, GUID
//!		counter, loopback handshake state and queue), "pregame", "game", "map", "team00".."team63",
//!		"player00".."player63", "supporting".
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#pragma once
#ifndef HSTCHECKPOINT_H
#define HSTCHECKPOINT_H

#include "HstCommon.h"
#include "diag/HstDigest.h"

class HstCheckpointFile
{
public:
	struct Section
	{
		std::string strName;
		std::string strData;
	};

	HstCheckpointFile() : m_bBodyStream(false), m_uiBodyPos(0) {}

	//! Save import (--import-save): the DLL body of a real .Civ5Save is ONE stream in the exe's order (game, map, teams,
	//! players, ... see above), without per-object boundaries. With a body stream set, HstReadSection (HstSession.cpp) feeds
	//! the remaining body bytes to each DLL Read in turn and advances; only "header", "host" and "pregame" stay sections.
	void SetBodyStream(std::string& strBody) { m_strBody.swap(strBody); m_bBodyStream = true; m_uiBodyPos = 0; }
	bool IsBodyStream() const { return m_bBodyStream; }
	const std::string& GetBody() const { return m_strBody; }
	unsigned int GetBodyPos() const { return m_uiBodyPos; }
	void AdvanceBody(unsigned int uiBytes) const { m_uiBodyPos += uiBytes; }
	//! Where each DLL Read found its object in the body (offset, bytes consumed): the authoritative segment map of a real save.
	struct Span
	{
		std::string strName;
		unsigned int uiOffset;
		unsigned int uiSize;
	};
	void AddSpan(const std::string& strName, unsigned int uiOffset, unsigned int uiSize) const
	{
		Span kSpan = { strName, uiOffset, uiSize };
		m_akSpans.push_back(kSpan);
	}
	const Span* FindSpan(const std::string& strName) const
	{
		for(size_t i = 0; i < m_akSpans.size(); ++i)
			if(m_akSpans[i].strName == strName)
				return &m_akSpans[i];
		return NULL;
	}

	void Add(const std::string& strName, const void* pData, unsigned int uiBytes);
	void Add(const std::string& strName, FMemoryStream& kStream) { Add(strName, kStream.GetBuf(), kStream.GetEOF()); }
	const Section* Find(const std::string& strName) const;
	const std::vector<Section>& GetSections() const { return m_akSections; }

	//! Header text helpers ("key=value\n" lines in the "header" section).
	static std::string HeaderValue(const std::string& strHeader, const char* szKey);

	//! Writes the file; ui64Hash = xxh64 of the whole file.
	bool Write(const std::wstring& wstrPath, unsigned int& uiBytes, HstHash64& ui64Hash, std::string& strError) const;
	//! Reads and verifies every section hash.
	bool Read(const std::wstring& wstrPath, std::string& strError);

protected:
	std::vector<Section> m_akSections;
	std::string m_strBody;
	bool m_bBodyStream;
	mutable unsigned int m_uiBodyPos;
	mutable std::vector<Span> m_akSpans;
};

#endif // HSTCHECKPOINT_H
