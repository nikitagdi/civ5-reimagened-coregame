//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstVfs.cpp
//!  \brief     Path resolution for LoadFile / include.
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#include "HstCommon.h"
#include "script/HstVfs.h"
#include "platform/HstPlatform.h"
#include "platform/HstDetHeap.h"

static std::string HstLowerA(const std::string& str)
{
	std::string strOut(str);
	for(size_t i = 0; i < strOut.size(); ++i)
		strOut[i] = (char)tolower((unsigned char)strOut[i]);
	return strOut;
}

std::wstring HstVfs::Absolute(const std::wstring& wstrPath) const
{
	if(wstrPath.size() > 1 && (wstrPath[1] == L':' || (wstrPath[0] == L'\\' && wstrPath[1] == L'\\')))
		return wstrPath;
	return HstJoinPath(m_wstrGameDir, wstrPath);
}

void HstVfs::AddIncludeRoot(const std::wstring& wstrDir, bool bRecursive)
{
	Root kRoot;
	kRoot.wstrDir = Absolute(wstrDir);
	kRoot.bRecursive = bRecursive;
	kRoot.bIndexed = false;
	m_aRoots.push_back(kRoot);
}

void HstVfs::PushFrontIncludeRoot(const std::wstring& wstrDir, bool bRecursive)
{
	std::wstring wstrAbs = Absolute(wstrDir);
	for(size_t i = 0; i < m_aRoots.size(); ++i)
		if(_wcsicmp(m_aRoots[i].wstrDir.c_str(), wstrAbs.c_str()) == 0)
			return;
	Root kRoot;
	kRoot.wstrDir = wstrAbs;
	kRoot.bRecursive = bRecursive;
	kRoot.bIndexed = false;
	m_aRoots.insert(m_aRoots.begin(), kRoot);
}

void HstVfs::IndexDir(Root& kRoot, const std::wstring& wstrDir)
{
	WIN32_FIND_DATAW kData;
	HANDLE hFind = FindFirstFileW(HstJoinPath(wstrDir, L"*").c_str(), &kData);
	if(hFind == INVALID_HANDLE_VALUE)
		return;
	std::vector<std::wstring> aSubDirs;
	do
	{
		std::wstring wstrName = kData.cFileName;
		if(wstrName == L"." || wstrName == L"..")
			continue;
		if(kData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			if(kRoot.bRecursive)
				aSubDirs.push_back(HstJoinPath(wstrDir, wstrName));
			continue;
		}
		std::string strName = HstLowerA(HstToUtf8(wstrName));
		size_t uiDot = strName.find_last_of('.');
		if(uiDot == std::string::npos || strName.substr(uiDot) != ".lua")
			continue;
		std::string strStem = strName.substr(0, uiDot);
		if(kRoot.kStems.find(strStem) == kRoot.kStems.end())
			kRoot.kStems[strStem] = HstJoinPath(wstrDir, wstrName);
	}
	while(FindNextFileW(hFind, &kData));
	FindClose(hFind);
	// Files of a folder win over its subfolders; subfolders in name order (FindFirstFile order on NTFS).
	std::sort(aSubDirs.begin(), aSubDirs.end());
	for(size_t i = 0; i < aSubDirs.size(); ++i)
		IndexDir(kRoot, aSubDirs[i]);
}

void HstVfs::IndexRoot(Root& kRoot)
{
	if(kRoot.bIndexed)
		return;
	kRoot.bIndexed = true;
	IndexDir(kRoot, kRoot.wstrDir);
}

bool HstVfs::ResolvePath(const char* szPath, std::wstring& wstrOut) const
{
	if(!szPath || !*szPath)
		return false;
	std::wstring wstrPath = Absolute(HstFromUtf8(szPath));
	for(size_t i = 0; i < wstrPath.size(); ++i)
		if(wstrPath[i] == L'/')
			wstrPath[i] = L'\\';
	if(HstFileExists(wstrPath))
	{
		wstrOut = wstrPath;
		return true;
	}
	return false;
}

bool HstVfs::ResolveInclude(const char* szName, std::wstring& wstrOut)
{
	if(m_bPauseDetHeap)
		HstDetHeapPause();
	std::string strStem = HstLowerA(szName ? szName : "");
	size_t uiSlash = strStem.find_last_of("\\/");
	if(uiSlash != std::string::npos)
		strStem = strStem.substr(uiSlash + 1);
	if(strStem.size() > 4 && strStem.substr(strStem.size() - 4) == ".lua")
		strStem = strStem.substr(0, strStem.size() - 4);
	for(size_t i = 0; i < m_aRoots.size(); ++i)
	{
		IndexRoot(m_aRoots[i]);
		std::map<std::string, std::wstring>::const_iterator it = m_aRoots[i].kStems.find(strStem);
		if(it != m_aRoots[i].kStems.end())
		{
			wstrOut = it->second;
			if(m_bPauseDetHeap)
				HstDetHeapResume();
			return true;
		}
	}
	if(m_bPauseDetHeap)
		HstDetHeapResume();
	return false;
}

bool HstVfs::ReadFile(const std::wstring& wstrPath, std::string& strOut)
{
	FILE* pkFile = _wfopen(wstrPath.c_str(), L"rb");
	if(!pkFile)
		return false;
	fseek(pkFile, 0, SEEK_END);
	long lSize = ftell(pkFile);
	fseek(pkFile, 0, SEEK_SET);
	strOut.resize(lSize > 0 ? lSize : 0);
	size_t uiRead = lSize > 0 ? fread(&strOut[0], 1, lSize, pkFile) : 0;
	fclose(pkFile);
	strOut.resize(uiRead);
	// Strip a UTF-8 BOM (Lua 5.1 does not accept it).
	if(strOut.size() >= 3 && (unsigned char)strOut[0] == 0xEF && (unsigned char)strOut[1] == 0xBB && (unsigned char)strOut[2] == 0xBF)
		strOut.erase(0, 3);
	return true;
}
