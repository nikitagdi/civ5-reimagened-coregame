//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstVfs.h
//!  \brief     Minimal virtual file system: game-relative paths and Civ5-style include() resolution.
//!
//!		include(name) matches the file stem case-insensitively (extension optional) over an ordered list of
//!		roots, each indexed recursively; the first root that has the stem wins (design 3.3, built-in order until
//!		game-data produces vfs.json).
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#pragma once
#ifndef HSTVFS_H
#define HSTVFS_H

#include "HstCommon.h"

class HstVfs
{
public:
	HstVfs() : m_bPauseDetHeap(false) {}
	void SetGameDir(const std::wstring& wstrGameDir) { m_wstrGameDir = wstrGameDir; }
	//! Index / resolve with the deterministic heap paused (roots whose paths vary per machine, e.g. --lua-overlay).
	void SetPauseDetHeap(bool bPause) { m_bPauseDetHeap = bPause; }
	size_t GetNumRoots() const { return m_aRoots.size(); }
	const std::wstring& GetRoot(size_t i) const { return m_aRoots[i].wstrDir; }
	//! Append an include root (lower priority than the ones added before). Relative paths are game-relative.
	void AddIncludeRoot(const std::wstring& wstrDir, bool bRecursive);
	//! Insert a root with top priority (e.g. the folder of the map script being loaded).
	void PushFrontIncludeRoot(const std::wstring& wstrDir, bool bRecursive);

	//! "Assets\Maps\...\X.lua" (game-relative), or an absolute path -> existing file.
	bool ResolvePath(const char* szPath, std::wstring& wstrOut) const;
	bool ResolveInclude(const char* szName, std::wstring& wstrOut);
	static bool ReadFile(const std::wstring& wstrPath, std::string& strOut);

protected:
	struct Root
	{
		std::wstring wstrDir;
		bool bRecursive;
		bool bIndexed;
		std::map<std::string, std::wstring> kStems;	// lower-case stem -> path (first found)
	};
	void IndexRoot(Root& kRoot);
	void IndexDir(Root& kRoot, const std::wstring& wstrDir);
	std::wstring Absolute(const std::wstring& wstrPath) const;

	std::wstring m_wstrGameDir;
	std::vector<Root> m_aRoots;
	bool m_bPauseDetHeap;
};

#endif // HSTVFS_H
