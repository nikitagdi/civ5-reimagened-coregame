//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstExeExports.cpp
//!  \brief     FireWorks services the exe exports to the gamecore DLL.
//!
//!		FireWorks is linked statically into the DLL with FXS_IS_DLL; there, FStringA_GetStringInitData,
//!		FILogFileMgr::GetInstance, GetTempHeap, FIFileSystem::GetInstance, FSBABind, ... resolve their target with
//!		GetProcAddress(GetModuleHandleA(NULL), "<name>") and jump to it. CivilizationV_DX11.exe exports 22 such
//!		functions (dumpbin /exports); the DLL references 15 of them. Without the exports the DLL jumps to NULL in
//!		its static initializers (LoadLibrary fails with ERROR_DLL_INIT_FAILED).
//!
//!		Provided:  FStringA/W_GetStringInitData (exe 0x413f70/0x415f10: zeroed static FString*Data),
//!		           GetLogFileMgr/PeekLogFileMgr (exe 0x41cd40/0x41cd50; logs go to <run>\Logs or nowhere),
//!		           GetTempHeap (exe 0x429aa0; FireWorks TempHeap from FireWorksWin32.lib).
//!		           GetFileSystemMgr (exe FIFileSystem; CvGame::Read writes the embedded saved-game DB with it).
//!		Not provided: FSBA* / GetSmallObjectAllocator (optional: FSBABind fails cleanly and the DLL uses its
//!		           normal allocator).
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#include "HstCommon.h"
#if !defined(_WINPC)
#define _WINPC	// FireWorks platform switch (FAtomic.h); FireWorksWin32.lib is the _WINPC build
#endif
#include <FireWorks/FTempHeap.h>
#include <FireWorks/FIFile.h>
#include "engine/HstExeExports.h"
#include "platform/HstDetHeap.h"
#include "platform/HstPlatform.h"

#include <stdarg.h>

// ---- FString empty-string init data -----------------------------------------------------------------------------
// FStringA::FStringAData = { int m_iAllocLength; int m_iDataLength; USHORT m_bLocked; USHORT m_bFixed; char data[] }.
// The exe returns a zero-initialized static (BSS); the trailing zeros are the empty string's terminator.
static int s_aiFStringAInit[8] = {0};
static int s_aiFStringWInit[8] = {0};

extern "C" __declspec(dllexport) void* FStringA_GetStringInitData()
{
	return s_aiFStringAInit;
}

extern "C" __declspec(dllexport) void* FStringW_GetStringInitData()
{
	return s_aiFStringWInit;
}

// ---- log files ---------------------------------------------------------------------------------------------------
class HstLogFileMgr;

class HstLogFile : public FILogFile
{
public:
	HstLogFile(HstLogFileMgr* pkMgr, const std::wstring& wstrName, uint uiFlags)
		: m_pkMgr(pkMgr), m_wstrName(wstrName), m_uiFlags(uiFlags), m_pkFile(NULL), m_bOpenFailed(false)
	{
	}
	virtual ~HstLogFile()
	{
		Close();
	}

	virtual void Msg(const TCHAR* format, ...)
	{
		if(!Enabled())
			return;
		va_list args;
		va_start(args, format);
		Write("", format, args);
		va_end(args);
	}
	virtual void DebugMsg(const TCHAR* format, ...)
	{
		if(!Enabled())
			return;
		va_list args;
		va_start(args, format);
		Write("DEBUG: ", format, args);
		va_end(args);
	}
	virtual void WarningMsg(const TCHAR* format, ...)
	{
		if(!Enabled())
			return;
		va_list args;
		va_start(args, format);
		Write("WARNING: ", format, args);
		va_end(args);
	}
	virtual void ErrorMsg(const TCHAR* format, ...)
	{
		if(!Enabled())
			return;
		va_list args;
		va_start(args, format);
		Write("ERROR: ", format, args);
		va_end(args);
	}
	virtual void SetFlags(uint uiFlags) { m_uiFlags = uiFlags; }
	virtual uint GetFlags() const { return m_uiFlags; }
	virtual void SetBufSize(int iSize) {}
	virtual LPCWSTR GetFileName() const { return m_wstrName.c_str(); }
	virtual void Close()
	{
		if(m_pkFile)
		{
			fclose(m_pkFile);
			m_pkFile = NULL;
		}
	}
	virtual void Open() {}
	virtual void Archive(LPCWSTR wszArchiveName) {}

	const std::wstring& GetName() const { return m_wstrName; }

protected:
	bool Enabled() const;

	void Write(const char* szPrefix, const TCHAR* szFormat, va_list args)
	{
		if(!m_pkFile && !m_bOpenFailed)
		{
			// Timestamps are omitted on purpose: logs must be byte-comparable across runs.
			HstDetHeapPauseScope kPause;	// run-dir path: keep it out of the deterministic heap
			std::wstring wstrDir = HstJoinPath(HstGetRunDir(), L"Logs");
			HstCreateDirs(wstrDir);
			m_pkFile = _wfopen(HstJoinPath(wstrDir, m_wstrName).c_str(), L"w");
			m_bOpenFailed = (m_pkFile == NULL);
		}
		if(!m_pkFile)
			return;
		fputs(szPrefix, m_pkFile);
		vfprintf(m_pkFile, szFormat, args);
		fputc('\n', m_pkFile);
		if(!(m_uiFlags & kDontFlushOnWrite))
			fflush(m_pkFile);
	}

	HstLogFileMgr* m_pkMgr;
	std::wstring m_wstrName;
	uint m_uiFlags;
	FILE* m_pkFile;
	bool m_bOpenFailed;
};

class HstLogFileMgr : public FILogFileMgr
{
public:
	HstLogFileMgr() : m_bEnabled(false) {}
	virtual ~HstLogFileMgr() {}

	virtual void EnableLogging() { m_bEnabled = true; }
	virtual void DisableLogging() { m_bEnabled = false; }
	virtual FILogFile* GetLog(LPCWSTR wszFileName, uint uiFlags, const TCHAR* szTitleString)
	{
		std::wstring wstrName = Sanitize(wszFileName ? wszFileName : L"unnamed.log");
		std::map<std::wstring, HstLogFile*>::iterator it = m_kLogs.find(wstrName);
		if(it != m_kLogs.end())
			return it->second;
		HstLogFile* pkLog = new HstLogFile(this, wstrName, uiFlags);
		m_kLogs[wstrName] = pkLog;
		return pkLog;
	}
	virtual FILogFile* GetLog(LPCSTR szFileName, uint uiFlags, const TCHAR* szTitleString)
	{
		return GetLog(HstFromAcp(szFileName ? szFileName : "unnamed.log").c_str(), uiFlags, szTitleString);
	}
	virtual bool DeleteLog(FILogFile* pkLog)
	{
		for(std::map<std::wstring, HstLogFile*>::iterator it = m_kLogs.begin(); it != m_kLogs.end(); ++it)
		{
			if(it->second == pkLog)
			{
				delete it->second;
				m_kLogs.erase(it);
				return true;
			}
		}
		return false;
	}
	virtual bool DeleteLog(LPCWSTR wszFileName)
	{
		std::map<std::wstring, HstLogFile*>::iterator it = m_kLogs.find(Sanitize(wszFileName ? wszFileName : L""));
		if(it == m_kLogs.end())
			return false;
		delete it->second;
		m_kLogs.erase(it);
		return true;
	}
	virtual bool IsLoggingEnabled() { return m_bEnabled; }

	void FlushAll()
	{
		for(std::map<std::wstring, HstLogFile*>::iterator it = m_kLogs.begin(); it != m_kLogs.end(); ++it)
			it->second->Close();
	}

protected:
	//! Keep log files inside <run>\Logs: strip drive letters, leading separators and "..".
	static std::wstring Sanitize(const std::wstring& wstrIn)
	{
		std::wstring wstr;
		for(size_t i = 0; i < wstrIn.size(); ++i)
		{
			wchar_t c = wstrIn[i];
			if(c == L'/')
				c = L'\\';
			if(c == L':')
				c = L'_';
			wstr += c;
		}
		while(!wstr.empty() && (wstr[0] == L'\\' || wstr[0] == L'.'))
			wstr.erase(0, 1);
		size_t uiPos;
		while((uiPos = wstr.find(L"..")) != std::wstring::npos)
			wstr.replace(uiPos, 2, L"_");
		return wstr.empty() ? std::wstring(L"unnamed.log") : wstr;
	}

	bool m_bEnabled;
	std::map<std::wstring, HstLogFile*> m_kLogs;
	friend class HstLogFile;
};

bool HstLogFile::Enabled() const
{
	return m_pkMgr->m_bEnabled;
}

static HstLogFileMgr* s_pkLogFileMgr = NULL;

extern "C" __declspec(dllexport) FILogFileMgr* GetLogFileMgr()
{
	if(!s_pkLogFileMgr)
		s_pkLogFileMgr = new HstLogFileMgr();
	return s_pkLogFileMgr;
}

extern "C" __declspec(dllexport) FILogFileMgr* PeekLogFileMgr()
{
	return s_pkLogFileMgr;
}

// ---- temp heap ----------------------------------------------------------------------------------------------------
// FireWorks TempHeap (bump allocator); the DLL runs its own copy of the TempHeap methods on this object.
static TempHeap s_kHstTempHeap;
static const uint s_uiHstTempHeapBytes = 16 * 1024 * 1024;	// GUESS: the exe's size is not pinned down

extern "C" __declspec(dllexport) TempHeap* GetTempHeap()
{
	return &s_kHstTempHeap;
}

// ---- file system --------------------------------------------------------------------------------------------------
// FIFileSystem::GetInstance() in the DLL binds "GetFileSystemMgr". Users: CvGame::Read (writes the embedded
// Civ5SavedGameDatabase.db back into the cache folder: Create(modeWrite) + Write + Close; the object is never deleted)
// and the WorldBuilder map loader. Plain Win32 files; writes go through the same policy as the IAT sandbox.
class HstFile : public FIFile
{
public:
	HstFile() : m_hFile(INVALID_HANDLE_VALUE) {}
	virtual ~HstFile() { Close(); }

	virtual FileErr Open(const char* szFileName, OpenMode eOpenMode, dword dwFlags, uint uiBufferSize)
	{
		return Open(HstFromAcp(szFileName ? szFileName : "").c_str(), eOpenMode, dwFlags, uiBufferSize);
	}
	virtual FileErr Open(const wchar* wszFileName, OpenMode eOpenMode, dword dwFlags, uint uiBufferSize)
	{
		Close();
		if(!wszFileName || !*wszFileName)
			return FILE_BADNAME;
		m_wstrName = wszFileName;
		const bool bWrite = (eOpenMode & modeWrite) != 0;
		if(bWrite && !HstSandboxIsWriteAllowed(wszFileName))
		{
			HstLog("filesystem: DENY write open %s", HstToUtf8(m_wstrName).c_str());
			return FILE_BADMODE;
		}
		const DWORD dwAccess = (eOpenMode == modeRead) ? GENERIC_READ : (eOpenMode == modeWrite ? GENERIC_WRITE : GENERIC_READ | GENERIC_WRITE);
		const DWORD dwDisposition = (eOpenMode == modeRead) ? OPEN_EXISTING : (eOpenMode == modeWrite ? CREATE_ALWAYS : OPEN_ALWAYS);
		m_hFile = CreateFileW(wszFileName, dwAccess, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, dwDisposition, FILE_ATTRIBUTE_NORMAL, NULL);
		return m_hFile != INVALID_HANDLE_VALUE ? FILE_OK : FILE_NOTFOUND;
	}
	virtual FileErr Close()
	{
		if(m_hFile != INVALID_HANDLE_VALUE)
		{
			CloseHandle(m_hFile);
			m_hFile = INVALID_HANDLE_VALUE;
		}
		return FILE_OK;
	}
	virtual const wchar_t* GetFileName() { return m_wstrName.c_str(); }
	virtual byte* GetViewCaret() const { return NULL; }
	virtual byte* GetViewEnd() const { return NULL; }
	virtual byte* GetViewPtr() const { return NULL; }
	virtual uint GetLength() const { return IsOpen() ? (uint)GetFileSize(m_hFile, NULL) : 0; }
	virtual void SetLength(uint uiSize)
	{
		if(IsOpen())
		{
			SetFilePointer(m_hFile, (LONG)uiSize, NULL, FILE_BEGIN);
			SetEndOfFile(m_hFile);
		}
	}
	virtual bool IsOpen() const { return m_hFile != INVALID_HANDLE_VALUE; }
	virtual bool IsMapped() const { return false; }
	virtual uint Seek(long lOffset, SeekMode eOrigin)
	{
		return IsOpen() ? (uint)SetFilePointer(m_hFile, lOffset, NULL, eOrigin == begin ? FILE_BEGIN : (eOrigin == current ? FILE_CURRENT : FILE_END)) : 0;
	}
	virtual uint SeekToBegin() { return Seek(0, begin); }
	virtual uint SeekToEnd() { return Seek(0, end); }
	virtual uint GetPosition() { return Seek(0, current); }
	virtual void Rewind() { SeekToBegin(); }
	virtual bool IsEOF() { return GetPosition() >= GetLength(); }
	virtual uint Read(void* pData, uint uiSize)
	{
		DWORD dwRead = 0;
		if(IsOpen())
			ReadFile(m_hFile, pData, uiSize, &dwRead, NULL);
		return (uint)dwRead;
	}
	virtual uint Write(const void* pData, uint uiSize)
	{
		DWORD dwWritten = 0;
		if(IsOpen())
			WriteFile(m_hFile, pData, uiSize, &dwWritten, NULL);
		return (uint)dwWritten;
	}
	virtual uint Flush()
	{
		if(IsOpen())
			FlushFileBuffers(m_hFile);
		return 0;
	}

protected:
	HANDLE m_hFile;
	std::wstring m_wstrName;
};

class HstFileSystem : public FIFileSystem
{
public:
	virtual FIFile* Create(const char* szFileName, FIFile::OpenMode eOpenMode, dword dwFlags, uint uiBufferSize)
	{
		return Create(HstFromAcp(szFileName ? szFileName : "").c_str(), eOpenMode, dwFlags, uiBufferSize);
	}
	virtual FIFile* Create(const wchar* wszFileName, FIFile::OpenMode eOpenMode, dword dwFlags, uint uiBufferSize)
	{
		HstFile* pkFile = new HstFile();
		if(wszFileName && pkFile->Open(wszFileName, eOpenMode, dwFlags, uiBufferSize) != FILE_OK)
		{
			delete pkFile;
			return NULL;
		}
		return pkFile;
	}
	virtual FileErr Delete(const char* szFileName) { return Delete(HstFromAcp(szFileName ? szFileName : "").c_str()); }
	virtual FileErr Delete(const wchar* wszFileName)
	{
		if(!wszFileName || !HstSandboxIsWriteAllowed(wszFileName))
			return FILE_BADMODE;
		return DeleteFileW(wszFileName) ? FILE_OK : FILE_NOTFOUND;
	}
	virtual FileErr ReadAll(const char* szFileName, byte** pucBufferOut, size_t* puiBufferSizeOut, dword dwFlags)
	{
		return ReadAll(HstFromAcp(szFileName ? szFileName : "").c_str(), pucBufferOut, puiBufferSizeOut, dwFlags);
	}
	virtual FileErr ReadAll(const wchar* wszFileName, byte** pucBufferOut, size_t* puiBufferSizeOut, dword dwFlags)
	{
		HstFile kFile;
		if(kFile.Open(wszFileName, FIFile::modeRead, dwFlags, 0) != FILE_OK)
			return FILE_NOTFOUND;
		const uint uiLength = kFile.GetLength();
		*pucBufferOut = new byte[uiLength + 1];
		*puiBufferSizeOut = kFile.Read(*pucBufferOut, uiLength);
		(*pucBufferOut)[*puiBufferSizeOut] = 0;
		return FILE_OK;
	}
	virtual FileErr ReadAll(const char* szFileName, byte** pucBufferOut, size_t* puiBufferSizeOut, dword dwFlags, FIFile::Allocator& kAlloc)
	{
		return ReadAll(HstFromAcp(szFileName ? szFileName : "").c_str(), pucBufferOut, puiBufferSizeOut, dwFlags, kAlloc);
	}
	virtual FileErr ReadAll(const wchar* wszFileName, byte** pucBufferOut, size_t* puiBufferSizeOut, dword dwFlags, FIFile::Allocator& kAlloc)
	{
		HstFile kFile;
		if(kFile.Open(wszFileName, FIFile::modeRead, dwFlags, 0) != FILE_OK)
			return FILE_NOTFOUND;
		const uint uiLength = kFile.GetLength();
		*pucBufferOut = kAlloc.Allocate(uiLength + 1);
		*puiBufferSizeOut = kFile.Read(*pucBufferOut, uiLength);
		(*pucBufferOut)[*puiBufferSizeOut] = 0;
		return FILE_OK;
	}
	virtual bool Exist(const char* pszFileName) { return Exist(HstFromAcp(pszFileName ? pszFileName : "").c_str()); }
	virtual bool Exist(const wchar* pwszFileName) { return pwszFileName && HstFileExists(pwszFileName); }
	virtual uint GetLength(const char* szFileName) { return GetLength(HstFromAcp(szFileName ? szFileName : "").c_str()); }
	virtual uint GetLength(const wchar* wszFileName)
	{
		HstFile kFile;
		return kFile.Open(wszFileName, FIFile::modeRead, 0, 0) == FILE_OK ? kFile.GetLength() : 0;
	}
	virtual FFileTimestamp GetLastWriteTime(const char* pFileName)
	{
		FILETIME kTime = { 0, 0 };	// deterministic: the DLL never compares real timestamps in the paths the host runs
		return FFileTimestamp(kTime);
	}
	virtual bool IsFileCatalogEnabled() { return false; }
	virtual FFileErrorHandler* GetErrorHandler() { return NULL; }
};

static HstFileSystem s_kHstFileSystem;

extern "C" __declspec(dllexport) FIFileSystem* GetFileSystemMgr()
{
	return &s_kHstFileSystem;
}

// ---- host-side control --------------------------------------------------------------------------------------------
void HstExeExportsInit(bool bEnableDllLogs)
{
	s_kHstTempHeap.Startup(s_uiHstTempHeapBytes, NULL);
	HstLogFileMgr* pkMgr = (HstLogFileMgr*)GetLogFileMgr();
	if(bEnableDllLogs)
		pkMgr->EnableLogging();
}

void HstExeExportsFlush()
{
	if(s_pkLogFileMgr)
		s_pkLogFileMgr->FlushAll();
}
