//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstPlatform.cpp
//!  \brief     OS layer: logging, paths, DLL search path, FPU, crash handling, IAT file sandbox.
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#include "HstCommon.h"
#include "HstPlatform.h"
#include "HstDetHeap.h"

#include <float.h>
#include <malloc.h>
#include <stdarg.h>
#include <intrin.h>
#include <DbgHelp.h>

#pragma intrinsic(_ReturnAddress)

// =====================================================================================================================
// logging
// =====================================================================================================================
static FILE* s_pkLogFile = NULL;
static CRITICAL_SECTION s_kLogLock;
static bool s_bLogLockInit = false;
static bool s_bLogConsole = true;

void HstLogSetConsole(bool bConsole)
{
	s_bLogConsole = bConsole;
}

static void HstLogLockInit()
{
	if(!s_bLogLockInit)
	{
		InitializeCriticalSection(&s_kLogLock);
		s_bLogLockInit = true;
	}
}

bool HstLogOpen(const std::wstring& wstrPath)
{
	HstLogLockInit();
	s_pkLogFile = _wfopen(wstrPath.c_str(), L"w");
	return s_pkLogFile != NULL;
}

static void HstLogV(const char* szFormat, va_list args)
{
	HstLogLockInit();
	char szBuffer[4096];
	_vsnprintf_s(szBuffer, sizeof(szBuffer), _TRUNCATE, szFormat, args);
	EnterCriticalSection(&s_kLogLock);
	if(s_bLogConsole)
	{
		fputs(szBuffer, stdout);
		fputc('\n', stdout);
	}
	if(s_pkLogFile)
	{
		fputs(szBuffer, s_pkLogFile);
		fputc('\n', s_pkLogFile);
	}
	LeaveCriticalSection(&s_kLogLock);
}

void HstLog(const char* szFormat, ...)
{
	va_list args;
	va_start(args, szFormat);
	HstLogV(szFormat, args);
	va_end(args);
}

void HstLogFlush()
{
	fflush(stdout);
	if(s_pkLogFile)
		fflush(s_pkLogFile);
	HstSandboxFlush();
}

void HstFatal(int iExitCode, const char* szFormat, ...)
{
	va_list args;
	va_start(args, szFormat);
	HstLogV(szFormat, args);
	va_end(args);
	HstPrintStack(NULL, 1);
	HstLog("civhost: exit %d", iExitCode);
	HstLogFlush();
	ExitProcess(iExitCode);
}

// =====================================================================================================================
// strings / paths
// =====================================================================================================================
std::string HstToUtf8(const std::wstring& wstr)
{
	if(wstr.empty())
		return std::string();
	int iLen = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), NULL, 0, NULL, NULL);
	std::string str(iLen, '\0');
	WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &str[0], iLen, NULL, NULL);
	return str;
}

std::wstring HstFromUtf8(const std::string& str)
{
	if(str.empty())
		return std::wstring();
	int iLen = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), NULL, 0);
	std::wstring wstr(iLen, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &wstr[0], iLen);
	return wstr;
}

std::wstring HstFromAcp(const char* sz)
{
	if(!sz || !*sz)
		return std::wstring();
	int iLen = MultiByteToWideChar(CP_ACP, 0, sz, -1, NULL, 0);
	std::wstring wstr(iLen, L'\0');
	MultiByteToWideChar(CP_ACP, 0, sz, -1, &wstr[0], iLen);
	wstr.resize(iLen > 0 ? iLen - 1 : 0);
	return wstr;
}

std::wstring HstExeDir()
{
	wchar_t wszPath[MAX_PATH];
	DWORD dwLen = GetModuleFileNameW(NULL, wszPath, MAX_PATH);
	std::wstring wstr(wszPath, dwLen);
	size_t uiSlash = wstr.find_last_of(L"\\/");
	return uiSlash == std::wstring::npos ? std::wstring(L".") : wstr.substr(0, uiSlash);
}

std::wstring HstFullPath(const std::wstring& wstrPath)
{
	wchar_t wszFull[1024];
	DWORD dwLen = GetFullPathNameW(wstrPath.c_str(), HST_ARRAY_SIZE(wszFull), wszFull, NULL);
	if(dwLen == 0 || dwLen >= HST_ARRAY_SIZE(wszFull))
		return wstrPath;
	return std::wstring(wszFull, dwLen);
}

std::wstring HstJoinPath(const std::wstring& wstrA, const std::wstring& wstrB)
{
	if(wstrA.empty())
		return wstrB;
	wchar_t cLast = wstrA[wstrA.size() - 1];
	if(cLast == L'\\' || cLast == L'/')
		return wstrA + wstrB;
	return wstrA + L"\\" + wstrB;
}

bool HstFileExists(const std::wstring& wstrPath)
{
	DWORD dwAttr = GetFileAttributesW(wstrPath.c_str());
	return dwAttr != INVALID_FILE_ATTRIBUTES && !(dwAttr & FILE_ATTRIBUTE_DIRECTORY);
}

bool HstDirExists(const std::wstring& wstrPath)
{
	DWORD dwAttr = GetFileAttributesW(wstrPath.c_str());
	return dwAttr != INVALID_FILE_ATTRIBUTES && (dwAttr & FILE_ATTRIBUTE_DIRECTORY);
}

bool HstCreateDirs(const std::wstring& wstrPath)
{
	std::wstring wstrFull = HstFullPath(wstrPath);
	if(HstDirExists(wstrFull))
		return true;
	size_t uiSlash = wstrFull.find_last_of(L"\\/");
	if(uiSlash != std::wstring::npos && uiSlash > 2)
		HstCreateDirs(wstrFull.substr(0, uiSlash));
	return CreateDirectoryW(wstrFull.c_str(), NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool HstCopyIfChanged(const std::wstring& wstrSrc, const std::wstring& wstrDst)
{
	WIN32_FILE_ATTRIBUTE_DATA kSrc, kDst;
	if(!GetFileAttributesExW(wstrSrc.c_str(), GetFileExInfoStandard, &kSrc))
		return false;
	if(GetFileAttributesExW(wstrDst.c_str(), GetFileExInfoStandard, &kDst) &&
		kSrc.nFileSizeLow == kDst.nFileSizeLow && kSrc.nFileSizeHigh == kDst.nFileSizeHigh &&
		CompareFileTime(&kSrc.ftLastWriteTime, &kDst.ftLastWriteTime) == 0)
	{
		return true;
	}
	SetFileAttributesW(wstrDst.c_str(), FILE_ATTRIBUTE_NORMAL);
	if(!CopyFileW(wstrSrc.c_str(), wstrDst.c_str(), FALSE))
		return false;
	SetFileAttributesW(wstrDst.c_str(), FILE_ATTRIBUTE_NORMAL);
	return true;
}

// =====================================================================================================================
// process setup
// =====================================================================================================================
static std::wstring s_wstrRunDir;
static std::wstring s_wstrGameDir;
static std::wstring s_wstrSymbolPath;

bool HstPlatformInit(const std::wstring& wstrRunDir, const std::wstring& wstrGameDir)
{
	s_wstrRunDir = HstFullPath(wstrRunDir);
	s_wstrGameDir = HstFullPath(wstrGameDir);
	if(!HstCreateDirs(s_wstrRunDir))
		return false;
	if(!SetCurrentDirectoryW(s_wstrRunDir.c_str()))
		return false;
	// P1: the three game DLLs are delay-loaded and resolve from the game root; nothing is copied.
	if(!SetDllDirectoryW(s_wstrGameDir.c_str()))
		return false;
	// P3: 53-bit precision, round to nearest (CRT default; the exe's game-core thread starts with 0x027F).
	unsigned int uiCW = 0;
	_controlfp_s(&uiCW, _PC_53 | _RC_NEAR, _MCW_PC | _MCW_RC);
	return true;
}

const std::wstring& HstGetRunDir()
{
	return s_wstrRunDir;
}

const std::wstring& HstGetGameDir()
{
	return s_wstrGameDir;
}

unsigned int HstGetFpuControlWord()
{
	unsigned short usCW = 0;
	__asm fnstcw usCW;
	return usCW;
}

void HstAddSymbolPath(const std::wstring& wstrDir)
{
	if(!s_wstrSymbolPath.empty())
		s_wstrSymbolPath += L";";
	s_wstrSymbolPath += wstrDir;
}

// =====================================================================================================================
// crash handling
// =====================================================================================================================
static bool s_bSymInit = false;

static void HstSymInit()
{
	if(s_bSymInit)
		return;
	s_bSymInit = true;
	HANDLE hProcess = GetCurrentProcess();
	std::wstring wstrPath = s_wstrSymbolPath;
	if(!wstrPath.empty())
		wstrPath += L";";
	wstrPath += HstExeDir();
	SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
	std::string strPath = HstToUtf8(wstrPath);
	if(!SymInitialize(hProcess, strPath.c_str(), TRUE))
	{
		// The gamecore DllMain (FDebugHelper::LoadSymbols) may already have initialized dbghelp for the process.
		SymSetSearchPath(hProcess, strPath.c_str());
		SymRefreshModuleList(hProcess);
	}
}

//! "module+0xoffset" without touching dbghelp (safe inside file-API hooks).
static std::string HstDescribeAddressNoSym(const void* pAddress)
{
	char szOut[MAX_PATH + 32];
	HMODULE hModule = NULL;
	char szModule[MAX_PATH] = "?";
	DWORD_PTR uiOffset = (DWORD_PTR)pAddress;
	if(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)pAddress, &hModule) && hModule)
	{
		char szPath[MAX_PATH];
		DWORD dwLen = GetModuleFileNameA(hModule, szPath, MAX_PATH);
		const char* szBase = szPath;
		for(DWORD i = 0; i < dwLen; ++i)
			if(szPath[i] == '\\' || szPath[i] == '/')
				szBase = szPath + i + 1;
		strncpy_s(szModule, szBase, _TRUNCATE);
		uiOffset = (DWORD_PTR)pAddress - (DWORD_PTR)hModule;
	}
	_snprintf_s(szOut, sizeof(szOut), _TRUNCATE, "%s+0x%x", szModule, (unsigned int)uiOffset);
	return std::string(szOut);
}

bool HstResolveAddress(const void* pAddress, std::string& strFunction, std::string& strFile, unsigned int& uiLine, unsigned int& uiRva,
	unsigned int* puiDisp)
{
	strFunction.clear();
	strFile.clear();
	uiLine = 0;
	uiRva = 0;
	if(puiDisp)
		*puiDisp = 0;
	HMODULE hModule = NULL;
	if(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)pAddress, &hModule) && hModule)
		uiRva = (unsigned int)((DWORD_PTR)pAddress - (DWORD_PTR)hModule);
	HstSymInit();
	char acSymbol[sizeof(SYMBOL_INFO) + 512];
	SYMBOL_INFO* pkSymbol = (SYMBOL_INFO*)acSymbol;
	memset(acSymbol, 0, sizeof(acSymbol));
	pkSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
	pkSymbol->MaxNameLen = 511;
	DWORD64 ui64Disp = 0;
	bool bFound = false;
	if(SymFromAddr(GetCurrentProcess(), (DWORD64)(DWORD_PTR)pAddress, &ui64Disp, pkSymbol))
	{
		strFunction = pkSymbol->Name;
		if(puiDisp)
			*puiDisp = (unsigned int)ui64Disp;
		bFound = true;
	}
	IMAGEHLP_LINE64 kLine;
	memset(&kLine, 0, sizeof(kLine));
	kLine.SizeOfStruct = sizeof(kLine);
	DWORD dwLineDisp = 0;
	if(SymGetLineFromAddr64(GetCurrentProcess(), (DWORD64)(DWORD_PTR)pAddress, &dwLineDisp, &kLine))
	{
		strFile = kLine.FileName;
		uiLine = kLine.LineNumber;
		bFound = true;
	}
	return bFound;
}

std::string HstDescribeAddress(const void* pAddress)
{
	char szOut[1024];
	std::string str = HstDescribeAddressNoSym(pAddress);

	HstSymInit();
	char acSymbol[sizeof(SYMBOL_INFO) + 512];
	SYMBOL_INFO* pkSymbol = (SYMBOL_INFO*)acSymbol;
	memset(acSymbol, 0, sizeof(acSymbol));
	pkSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
	pkSymbol->MaxNameLen = 511;
	DWORD64 ui64Disp = 0;
	if(SymFromAddr(GetCurrentProcess(), (DWORD64)(DWORD_PTR)pAddress, &ui64Disp, pkSymbol))
	{
		_snprintf_s(szOut, sizeof(szOut), _TRUNCATE, " (%s+0x%x", pkSymbol->Name, (unsigned int)ui64Disp);
		str += szOut;
		IMAGEHLP_LINE64 kLine;
		memset(&kLine, 0, sizeof(kLine));
		kLine.SizeOfStruct = sizeof(kLine);
		DWORD dwLineDisp = 0;
		if(SymGetLineFromAddr64(GetCurrentProcess(), (DWORD64)(DWORD_PTR)pAddress, &dwLineDisp, &kLine))
		{
			const char* szFile = kLine.FileName;
			for(const char* p = kLine.FileName; *p; ++p)
				if(*p == '\\' || *p == '/')
					szFile = p + 1;
			_snprintf_s(szOut, sizeof(szOut), _TRUNCATE, " %s:%u", szFile, (unsigned int)kLine.LineNumber);
			str += szOut;
		}
		str += ")";
	}
	return str;
}

void HstPrintStack(const CONTEXT* pkContext, int iSkipFrames)
{
	CONTEXT kContext;
	if(pkContext)
		kContext = *pkContext;
	else
	{
		memset(&kContext, 0, sizeof(kContext));
		kContext.ContextFlags = CONTEXT_FULL;
		RtlCaptureContext(&kContext);
	}
	HstSymInit();
	STACKFRAME64 kFrame;
	memset(&kFrame, 0, sizeof(kFrame));
	kFrame.AddrPC.Offset = kContext.Eip;
	kFrame.AddrPC.Mode = AddrModeFlat;
	kFrame.AddrFrame.Offset = kContext.Ebp;
	kFrame.AddrFrame.Mode = AddrModeFlat;
	kFrame.AddrStack.Offset = kContext.Esp;
	kFrame.AddrStack.Mode = AddrModeFlat;
	HstLog("stack:");
	for(int i = 0; i < 64; ++i)
	{
		if(!StackWalk64(IMAGE_FILE_MACHINE_I386, GetCurrentProcess(), GetCurrentThread(), &kFrame, &kContext, NULL,
			SymFunctionTableAccess64, SymGetModuleBase64, NULL))
			break;
		if(kFrame.AddrPC.Offset == 0)
			break;
		if(i < iSkipFrames)
			continue;
		HstLog("  #%02d %08x %s", i, (unsigned int)kFrame.AddrPC.Offset, HstDescribeAddress((const void*)(DWORD_PTR)kFrame.AddrPC.Offset).c_str());
	}
}

static LONG s_lCrashing = 0;

LONG HstCrashFilter(EXCEPTION_POINTERS* pkEx)
{
	if(InterlockedExchange(&s_lCrashing, 1) != 0)
		return EXCEPTION_EXECUTE_HANDLER;
	EXCEPTION_RECORD* pkRec = pkEx->ExceptionRecord;
	HstLog("civhost: CRASH exception 0x%08x at %s", (unsigned int)pkRec->ExceptionCode, HstDescribeAddress(pkRec->ExceptionAddress).c_str());
	if(pkRec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && pkRec->NumberParameters >= 2)
		HstLog("  access violation: %s address 0x%08x", pkRec->ExceptionInformation[0] ? (pkRec->ExceptionInformation[0] == 8 ? "execute" : "write") : "read",
			(unsigned int)pkRec->ExceptionInformation[1]);

	std::wstring wstrDump = HstJoinPath(s_wstrRunDir.empty() ? std::wstring(L".") : s_wstrRunDir, L"civhost-crash.dmp");
	HANDLE hFile = CreateFileW(wstrDump.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if(hFile != INVALID_HANDLE_VALUE)
	{
		MINIDUMP_EXCEPTION_INFORMATION kInfo;
		kInfo.ThreadId = GetCurrentThreadId();
		kInfo.ExceptionPointers = pkEx;
		kInfo.ClientPointers = FALSE;
		MINIDUMP_TYPE eType = (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs);
		BOOL bOk = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, eType, &kInfo, NULL, NULL);
		CloseHandle(hFile);
		HstLog("  minidump %s: %s", bOk ? "written" : "FAILED", HstToUtf8(wstrDump).c_str());
	}
	HstPrintStack(pkEx->ContextRecord, 0);
	HstLogFlush();
	return EXCEPTION_EXECUTE_HANDLER;
}

static LONG WINAPI HstUnhandledFilter(EXCEPTION_POINTERS* pkEx)
{
	HstCrashFilter(pkEx);
	ExitProcess(3);
}

void HstInstallUnhandledFilter()
{
	SetUnhandledExceptionFilter(HstUnhandledFilter);
}

static LONG s_lFirstChanceLogged = 0;

static LONG WINAPI HstFirstChanceHandler(EXCEPTION_POINTERS* pkEx)
{
	DWORD dwCode = pkEx->ExceptionRecord->ExceptionCode;
	// Only errors (severity 3) and C++ exceptions; skip OutputDebugString / thread-naming notifications.
	if((dwCode & 0xC0000000) == 0xC0000000 || dwCode == 0xE06D7363)
	{
		if(InterlockedIncrement(&s_lFirstChanceLogged) <= 20)
			HstLog("first-chance exception 0x%08x at %s", (unsigned int)dwCode, HstDescribeAddressNoSym(pkEx->ExceptionRecord->ExceptionAddress).c_str());
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

void HstInstallFirstChanceLogger()
{
	AddVectoredExceptionHandler(1, HstFirstChanceHandler);
}

// =====================================================================================================================
// IAT file sandbox
// =====================================================================================================================
static FILE* s_pkSandboxLog = NULL;
static std::wstring s_wstrSandboxRoot;		// "<run>\" lower-case
static int s_iSandboxDenied = 0;
static int s_iSandboxWrites = 0;
static std::set<std::wstring> s_kSandboxReads;
static CRITICAL_SECTION s_kSandboxLock;

static std::wstring HstLower(const std::wstring& wstr)
{
	std::wstring wstrOut(wstr);
	for(size_t i = 0; i < wstrOut.size(); ++i)
		wstrOut[i] = (wchar_t)towlower(wstrOut[i]);
	return wstrOut;
}

static bool HstSandboxIsDevice(const wchar_t* wszPath)
{
	if(wcsncmp(wszPath, L"\\\\.\\", 4) == 0)
		return true;
	return _wcsicmp(wszPath, L"CONOUT$") == 0 || _wcsicmp(wszPath, L"CONIN$") == 0 || _wcsicmp(wszPath, L"NUL") == 0;
}

bool HstSandboxIsWriteAllowed(const wchar_t* wszPath)
{
	if(!wszPath)
		return true;
	if(HstSandboxIsDevice(wszPath))
		return true;
	std::wstring wstrFull = HstLower(HstFullPath(wszPath));
	if(s_wstrSandboxRoot.empty())
		return false;
	return wstrFull.compare(0, s_wstrSandboxRoot.size(), s_wstrSandboxRoot) == 0 ||
		wstrFull + L"\\" == s_wstrSandboxRoot;
}

static int s_iSandboxDeniedTool = 0;	// denials for dbghelp (PDB open-for-update probing; it falls back to read-only)

//! Zeroes the stack below the hook frame on exit (after the original API ran).
struct HstStackWipeScope
{
	~HstStackWipeScope() { HstWipeStack(32 * 1024); }
};

//! Returns false when the operation must be refused. szDetail: extra info for the log (CreateFile access/disposition).
static bool HstSandboxCheck(const wchar_t* wszPath, bool bWrite, const char* szApi, void* pCaller, const char* szDetail = "")
{
	if(!wszPath)
		return true;
	bool bAllowed = !bWrite || HstSandboxIsWriteAllowed(wszPath);
	std::wstring wstrFull = HstSandboxIsDevice(wszPath) ? std::wstring(wszPath) : HstFullPath(wszPath);
	HMODULE hCaller = NULL;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)pCaller, &hCaller);
	const bool bTool = hCaller != NULL && hCaller == GetModuleHandleW(L"dbghelp.dll");
	EnterCriticalSection(&s_kSandboxLock);
	bool bLog = true;
	if(!bWrite)
		bLog = s_kSandboxReads.insert(HstLower(wstrFull)).second;	// reads: first occurrence only
	if(bWrite)
	{
		if(bAllowed)
			++s_iSandboxWrites;
		else if(bTool)
			++s_iSandboxDeniedTool;
		else
			++s_iSandboxDenied;
	}
	if(bLog && s_pkSandboxLog)
	{
		fprintf(s_pkSandboxLog, "%s %s %-16s %s  [%s]%s%s\n", bAllowed ? "ALLOW" : (bTool ? "DENYT" : "DENY "), bWrite ? "W" : "R", szApi,
			HstToUtf8(wstrFull).c_str(), HstDescribeAddressNoSym(pCaller).c_str(), szDetail[0] ? " " : "", szDetail);
		fflush(s_pkSandboxLog);
	}
	LeaveCriticalSection(&s_kSandboxLock);
	return bAllowed;
}
// ---- hooked kernel32 file APIs ---------------------------------------------------------------------------------------
typedef HANDLE (WINAPI* PFN_CreateFileW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE (WINAPI* PFN_CreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef BOOL (WINAPI* PFN_PathW)(LPCWSTR);
typedef BOOL (WINAPI* PFN_PathA)(LPCSTR);
typedef BOOL (WINAPI* PFN_PathSaW)(LPCWSTR, LPSECURITY_ATTRIBUTES);
typedef BOOL (WINAPI* PFN_PathSaA)(LPCSTR, LPSECURITY_ATTRIBUTES);
typedef BOOL (WINAPI* PFN_Path2W)(LPCWSTR, LPCWSTR);
typedef BOOL (WINAPI* PFN_Path2A)(LPCSTR, LPCSTR);
typedef BOOL (WINAPI* PFN_Path2FlW)(LPCWSTR, LPCWSTR, DWORD);
typedef BOOL (WINAPI* PFN_Path2FlA)(LPCSTR, LPCSTR, DWORD);
typedef BOOL (WINAPI* PFN_Path2BW)(LPCWSTR, LPCWSTR, BOOL);
typedef BOOL (WINAPI* PFN_Path2BA)(LPCSTR, LPCSTR, BOOL);

static PFN_CreateFileW s_pfnCreateFileW;
static PFN_CreateFileA s_pfnCreateFileA;
static PFN_PathW s_pfnDeleteFileW;
static PFN_PathA s_pfnDeleteFileA;
static PFN_PathW s_pfnRemoveDirectoryW;
static PFN_PathA s_pfnRemoveDirectoryA;
static PFN_PathSaW s_pfnCreateDirectoryW;
static PFN_PathSaA s_pfnCreateDirectoryA;
static PFN_Path2W s_pfnMoveFileW;
static PFN_Path2A s_pfnMoveFileA;
static PFN_Path2FlW s_pfnMoveFileExW;
static PFN_Path2FlA s_pfnMoveFileExA;
static PFN_Path2BW s_pfnCopyFileW;
static PFN_Path2BA s_pfnCopyFileA;

static bool HstIsWriteOpen(DWORD dwAccess, DWORD dwDisposition, DWORD dwFlags)
{
	const DWORD dwWriteMask = GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE |
		FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA | WRITE_DAC | WRITE_OWNER;
	return (dwAccess & dwWriteMask) != 0 || dwDisposition != OPEN_EXISTING || (dwFlags & FILE_FLAG_DELETE_ON_CLOSE) != 0;
}

static HANDLE WINAPI HstHook_CreateFileW(LPCWSTR wszPath, DWORD dwAccess, DWORD dwShare, LPSECURITY_ATTRIBUTES pkSA, DWORD dwDisposition, DWORD dwFlags, HANDLE hTemplate)
{
	HstStackWipeScope kWipe;		// runs last: zero the callee stack (path residue)
	HstDetHeapPauseScope kPause;	// path bookkeeping must not shift the deterministic heap
	char szDetail[48];
	_snprintf_s(szDetail, sizeof(szDetail), _TRUNCATE, "access=0x%08x disp=%u", (unsigned int)dwAccess, (unsigned int)dwDisposition);
	if(!HstSandboxCheck(wszPath, HstIsWriteOpen(dwAccess, dwDisposition, dwFlags), "CreateFileW", _ReturnAddress(), szDetail))
	{
		SetLastError(ERROR_ACCESS_DENIED);
		return INVALID_HANDLE_VALUE;
	}
	return s_pfnCreateFileW(wszPath, dwAccess, dwShare, pkSA, dwDisposition, dwFlags, hTemplate);
}

static HANDLE WINAPI HstHook_CreateFileA(LPCSTR szPath, DWORD dwAccess, DWORD dwShare, LPSECURITY_ATTRIBUTES pkSA, DWORD dwDisposition, DWORD dwFlags, HANDLE hTemplate)
{
	HstStackWipeScope kWipe;		// runs last: zero the callee stack (path residue)
	HstDetHeapPauseScope kPause;	// path bookkeeping must not shift the deterministic heap
	char szDetail[48];
	_snprintf_s(szDetail, sizeof(szDetail), _TRUNCATE, "access=0x%08x disp=%u", (unsigned int)dwAccess, (unsigned int)dwDisposition);
	if(szPath && !HstSandboxCheck(HstFromAcp(szPath).c_str(), HstIsWriteOpen(dwAccess, dwDisposition, dwFlags), "CreateFileA", _ReturnAddress(), szDetail))
	{
		SetLastError(ERROR_ACCESS_DENIED);
		return INVALID_HANDLE_VALUE;
	}
	return s_pfnCreateFileA(szPath, dwAccess, dwShare, pkSA, dwDisposition, dwFlags, hTemplate);
}

#define HST_DENY_BOOL() { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }

static BOOL WINAPI HstHook_DeleteFileW(LPCWSTR wszPath)
{
	HstStackWipeScope kWipe;		// runs last: zero the callee stack (path residue)
	HstDetHeapPauseScope kPause;	// path bookkeeping must not shift the deterministic heap
	if(!HstSandboxCheck(wszPath, true, "DeleteFileW", _ReturnAddress())) HST_DENY_BOOL();
	return s_pfnDeleteFileW(wszPath);
}
static BOOL WINAPI HstHook_DeleteFileA(LPCSTR szPath)
{
	HstStackWipeScope kWipe;		// runs last: zero the callee stack (path residue)
	HstDetHeapPauseScope kPause;	// path bookkeeping must not shift the deterministic heap
	if(szPath && !HstSandboxCheck(HstFromAcp(szPath).c_str(), true, "DeleteFileA", _ReturnAddress())) HST_DENY_BOOL();
	return s_pfnDeleteFileA(szPath);
}
static BOOL WINAPI HstHook_RemoveDirectoryW(LPCWSTR wszPath)
{
	HstStackWipeScope kWipe;		// runs last: zero the callee stack (path residue)
	HstDetHeapPauseScope kPause;	// path bookkeeping must not shift the deterministic heap
	if(!HstSandboxCheck(wszPath, true, "RemoveDirectoryW", _ReturnAddress())) HST_DENY_BOOL();
	return s_pfnRemoveDirectoryW(wszPath);
}
static BOOL WINAPI HstHook_RemoveDirectoryA(LPCSTR szPath)
{
	HstStackWipeScope kWipe;		// runs last: zero the callee stack (path residue)
	HstDetHeapPauseScope kPause;	// path bookkeeping must not shift the deterministic heap
	if(szPath && !HstSandboxCheck(HstFromAcp(szPath).c_str(), true, "RemoveDirectoryA", _ReturnAddress())) HST_DENY_BOOL();
	return s_pfnRemoveDirectoryA(szPath);
}
static BOOL WINAPI HstHook_CreateDirectoryW(LPCWSTR wszPath, LPSECURITY_ATTRIBUTES pkSA)
{
	HstStackWipeScope kWipe;		// runs last: zero the callee stack (path residue)
	HstDetHeapPauseScope kPause;	// path bookkeeping must not shift the deterministic heap
	if(!HstSandboxCheck(wszPath, true, "CreateDirectoryW", _ReturnAddress())) HST_DENY_BOOL();
	return s_pfnCreateDirectoryW(wszPath, pkSA);
}
static BOOL WINAPI HstHook_CreateDirectoryA(LPCSTR szPath, LPSECURITY_ATTRIBUTES pkSA)
{
	HstStackWipeScope kWipe;		// runs last: zero the callee stack (path residue)
	HstDetHeapPauseScope kPause;	// path bookkeeping must not shift the deterministic heap
	if(szPath && !HstSandboxCheck(HstFromAcp(szPath).c_str(), true, "CreateDirectoryA", _ReturnAddress())) HST_DENY_BOOL();
	return s_pfnCreateDirectoryA(szPath, pkSA);
}
static BOOL WINAPI HstHook_MoveFileW(LPCWSTR wszFrom, LPCWSTR wszTo)
{
	HstStackWipeScope kWipe;		// runs last: zero the callee stack (path residue)
	HstDetHeapPauseScope kPause;	// path bookkeeping must not shift the deterministic heap
	void* pRet = _ReturnAddress();
	if(!HstSandboxCheck(wszFrom, true, "MoveFileW", pRet) || !HstSandboxCheck(wszTo, true, "MoveFileW", pRet)) HST_DENY_BOOL();
	return s_pfnMoveFileW(wszFrom, wszTo);
}
static BOOL WINAPI HstHook_MoveFileA(LPCSTR szFrom, LPCSTR szTo)
{
	HstStackWipeScope kWipe;		// runs last: zero the callee stack (path residue)
	HstDetHeapPauseScope kPause;	// path bookkeeping must not shift the deterministic heap
	void* pRet = _ReturnAddress();
	if((szFrom && !HstSandboxCheck(HstFromAcp(szFrom).c_str(), true, "MoveFileA", pRet)) ||
		(szTo && !HstSandboxCheck(HstFromAcp(szTo).c_str(), true, "MoveFileA", pRet))) HST_DENY_BOOL();
	return s_pfnMoveFileA(szFrom, szTo);
}
static BOOL WINAPI HstHook_MoveFileExW(LPCWSTR wszFrom, LPCWSTR wszTo, DWORD dwFlags)
{
	HstStackWipeScope kWipe;		// runs last: zero the callee stack (path residue)
	HstDetHeapPauseScope kPause;	// path bookkeeping must not shift the deterministic heap
	void* pRet = _ReturnAddress();
	if(!HstSandboxCheck(wszFrom, true, "MoveFileExW", pRet) || !HstSandboxCheck(wszTo, true, "MoveFileExW", pRet)) HST_DENY_BOOL();
	return s_pfnMoveFileExW(wszFrom, wszTo, dwFlags);
}
static BOOL WINAPI HstHook_MoveFileExA(LPCSTR szFrom, LPCSTR szTo, DWORD dwFlags)
{
	HstStackWipeScope kWipe;		// runs last: zero the callee stack (path residue)
	HstDetHeapPauseScope kPause;	// path bookkeeping must not shift the deterministic heap
	void* pRet = _ReturnAddress();
	if((szFrom && !HstSandboxCheck(HstFromAcp(szFrom).c_str(), true, "MoveFileExA", pRet)) ||
		(szTo && !HstSandboxCheck(HstFromAcp(szTo).c_str(), true, "MoveFileExA", pRet))) HST_DENY_BOOL();
	return s_pfnMoveFileExA(szFrom, szTo, dwFlags);
}
static BOOL WINAPI HstHook_CopyFileW(LPCWSTR wszFrom, LPCWSTR wszTo, BOOL bFailIfExists)
{
	HstStackWipeScope kWipe;		// runs last: zero the callee stack (path residue)
	HstDetHeapPauseScope kPause;	// path bookkeeping must not shift the deterministic heap
	void* pRet = _ReturnAddress();
	if(!HstSandboxCheck(wszFrom, false, "CopyFileW", pRet) || !HstSandboxCheck(wszTo, true, "CopyFileW", pRet)) HST_DENY_BOOL();
	return s_pfnCopyFileW(wszFrom, wszTo, bFailIfExists);
}
static BOOL WINAPI HstHook_CopyFileA(LPCSTR szFrom, LPCSTR szTo, BOOL bFailIfExists)
{
	HstStackWipeScope kWipe;		// runs last: zero the callee stack (path residue)
	HstDetHeapPauseScope kPause;	// path bookkeeping must not shift the deterministic heap
	void* pRet = _ReturnAddress();
	if((szFrom && !HstSandboxCheck(HstFromAcp(szFrom).c_str(), false, "CopyFileA", pRet)) ||
		(szTo && !HstSandboxCheck(HstFromAcp(szTo).c_str(), true, "CopyFileA", pRet))) HST_DENY_BOOL();
	return s_pfnCopyFileA(szFrom, szTo, bFailIfExists);
}

struct HstHookEntry
{
	const char* szName;
	void* pHook;
	void** ppOriginal;
};

static HstHookEntry s_akHooks[] =
{
	{"CreateFileW", (void*)HstHook_CreateFileW, (void**)&s_pfnCreateFileW},
	{"CreateFileA", (void*)HstHook_CreateFileA, (void**)&s_pfnCreateFileA},
	{"DeleteFileW", (void*)HstHook_DeleteFileW, (void**)&s_pfnDeleteFileW},
	{"DeleteFileA", (void*)HstHook_DeleteFileA, (void**)&s_pfnDeleteFileA},
	{"RemoveDirectoryW", (void*)HstHook_RemoveDirectoryW, (void**)&s_pfnRemoveDirectoryW},
	{"RemoveDirectoryA", (void*)HstHook_RemoveDirectoryA, (void**)&s_pfnRemoveDirectoryA},
	{"CreateDirectoryW", (void*)HstHook_CreateDirectoryW, (void**)&s_pfnCreateDirectoryW},
	{"CreateDirectoryA", (void*)HstHook_CreateDirectoryA, (void**)&s_pfnCreateDirectoryA},
	{"MoveFileW", (void*)HstHook_MoveFileW, (void**)&s_pfnMoveFileW},
	{"MoveFileA", (void*)HstHook_MoveFileA, (void**)&s_pfnMoveFileA},
	{"MoveFileExW", (void*)HstHook_MoveFileExW, (void**)&s_pfnMoveFileExW},
	{"MoveFileExA", (void*)HstHook_MoveFileExA, (void**)&s_pfnMoveFileExA},
	{"CopyFileW", (void*)HstHook_CopyFileW, (void**)&s_pfnCopyFileW},
	{"CopyFileA", (void*)HstHook_CopyFileA, (void**)&s_pfnCopyFileA},
};

bool HstSandboxInit(const std::wstring& wstrRunDir)
{
	InitializeCriticalSection(&s_kSandboxLock);
	s_wstrSandboxRoot = HstLower(HstJoinPath(HstFullPath(wstrRunDir), L""));
	if(s_wstrSandboxRoot.empty() || s_wstrSandboxRoot[s_wstrSandboxRoot.size() - 1] != L'\\')
		s_wstrSandboxRoot += L"\\";
	HMODULE hKernel = GetModuleHandleW(L"kernel32.dll");
	for(size_t i = 0; i < HST_ARRAY_SIZE(s_akHooks); ++i)
	{
		*s_akHooks[i].ppOriginal = (void*)GetProcAddress(hKernel, s_akHooks[i].szName);
		if(!*s_akHooks[i].ppOriginal)
			return false;
	}
	s_pkSandboxLog = _wfopen(HstJoinPath(wstrRunDir, L"sandbox.log").c_str(), L"w");
	if(s_pkSandboxLog)
		fprintf(s_pkSandboxLog, "# civhost IAT file sandbox: writes allowed only below %s\n", HstToUtf8(s_wstrSandboxRoot).c_str());
	return s_pkSandboxLog != NULL;
}

int HstSandboxPatchModule(HMODULE hModule)
{
	if(!hModule || !s_pfnCreateFileW)	// sandbox not initialized (--no-sandbox)
		return 0;
	BYTE* pBase = (BYTE*)hModule;
	IMAGE_DOS_HEADER* pkDos = (IMAGE_DOS_HEADER*)pBase;
	if(pkDos->e_magic != IMAGE_DOS_SIGNATURE)
		return 0;
	IMAGE_NT_HEADERS* pkNt = (IMAGE_NT_HEADERS*)(pBase + pkDos->e_lfanew);
	IMAGE_DATA_DIRECTORY& kDir = pkNt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if(kDir.VirtualAddress == 0)
		return 0;
	int iPatched = 0;
	for(IMAGE_IMPORT_DESCRIPTOR* pkDesc = (IMAGE_IMPORT_DESCRIPTOR*)(pBase + kDir.VirtualAddress); pkDesc->Name; ++pkDesc)
	{
		const char* szDll = (const char*)(pBase + pkDesc->Name);
		if(_strnicmp(szDll, "kernel32", 8) != 0 && _strnicmp(szDll, "kernelbase", 10) != 0 && _strnicmp(szDll, "api-ms-win-core-file", 20) != 0)
			continue;
		IMAGE_THUNK_DATA* pkIAT = (IMAGE_THUNK_DATA*)(pBase + pkDesc->FirstThunk);
		IMAGE_THUNK_DATA* pkINT = pkDesc->OriginalFirstThunk ? (IMAGE_THUNK_DATA*)(pBase + pkDesc->OriginalFirstThunk) : NULL;
		for(int i = 0; pkIAT[i].u1.Function; ++i)
		{
			for(size_t h = 0; h < HST_ARRAY_SIZE(s_akHooks); ++h)
			{
				bool bMatch = false;
				if(pkINT)
				{
					if(!(pkINT[i].u1.Ordinal & IMAGE_ORDINAL_FLAG32))
					{
						IMAGE_IMPORT_BY_NAME* pkName = (IMAGE_IMPORT_BY_NAME*)(pBase + pkINT[i].u1.AddressOfData);
						bMatch = strcmp((const char*)pkName->Name, s_akHooks[h].szName) == 0;
					}
				}
				else
					bMatch = (void*)pkIAT[i].u1.Function == *s_akHooks[h].ppOriginal;
				if(!bMatch || (void*)pkIAT[i].u1.Function == s_akHooks[h].pHook)
					continue;
				DWORD dwOld = 0;
				if(VirtualProtect(&pkIAT[i].u1.Function, sizeof(DWORD_PTR), PAGE_READWRITE, &dwOld))
				{
					pkIAT[i].u1.Function = (DWORD_PTR)s_akHooks[h].pHook;
					VirtualProtect(&pkIAT[i].u1.Function, sizeof(DWORD_PTR), dwOld, &dwOld);
					++iPatched;
				}
			}
		}
	}
	return iPatched;
}

int HstSandboxPatchKnownModules()
{
	static const wchar_t* s_awszModules[] =
	{
		L"CvGameCore_Expansion2.dll", L"lua51_Win32.dll", L"CvGameDatabaseWin32Final Release.dll",
		L"CvLocalizationWin32Final Release.dll", L"MSVCR90.dll", L"MSVCP90.dll", L"dbghelp.dll",
	};
	int iTotal = 0;
	for(size_t i = 0; i < HST_ARRAY_SIZE(s_awszModules); ++i)
	{
		HMODULE hModule = GetModuleHandleW(s_awszModules[i]);
		if(!hModule)
			continue;
		int iPatched = HstSandboxPatchModule(hModule);
		iTotal += iPatched;
		if(s_pkSandboxLog)
			fprintf(s_pkSandboxLog, "# patched %d IAT slots in %s\n", iPatched, HstToUtf8(s_awszModules[i]).c_str());
	}
	if(s_pkSandboxLog)
		fflush(s_pkSandboxLog);
	return iTotal;
}

int HstSandboxGetDenied()
{
	return s_iSandboxDenied;
}

int HstSandboxGetDeniedTool()
{
	return s_iSandboxDeniedTool;
}

int HstSandboxGetWritesAllowed()
{
	return s_iSandboxWrites;
}

void HstSandboxFlush()
{
	if(s_pkSandboxLog)
		fflush(s_pkSandboxLog);
}

// =====================================================================================================================
// Determinism patches (IAT): virtual clocks and deterministic GUIDs for the gamecore DLL
// =====================================================================================================================
static volatile LONG s_lVirtualMs = 0;		// advanced by the host (HstVirtualTimeAdvance), never by queries
static volatile LONG s_lGuidCounter = 0;
static const unsigned __int64 s_ui64VirtualEpochFileTime = 130013280000000000ULL;	// 2013-01-01 00:00:00 UTC

void HstVirtualTimeAdvance(unsigned int uiMs)
{
	InterlockedExchangeAdd(&s_lVirtualMs, (LONG)uiMs);
}

unsigned int HstVirtualTimeMs()
{
	return (unsigned int)s_lVirtualMs;
}

void HstVirtualTimeSet(unsigned int uiMs)
{
	InterlockedExchange(&s_lVirtualMs, (LONG)uiMs);
}

unsigned int HstGuidCounterGet()
{
	return (unsigned int)s_lGuidCounter;
}

void HstGuidCounterSet(unsigned int uiCount)
{
	InterlockedExchange(&s_lGuidCounter, (LONG)uiCount);
}

static DWORD WINAPI HstHook_timeGetTime()
{
	return (DWORD)s_lVirtualMs;
}

static DWORD WINAPI HstHook_GetTickCount()
{
	return (DWORD)s_lVirtualMs;
}

static BOOL WINAPI HstHook_QueryPerformanceFrequency(LARGE_INTEGER* pkFreq)
{
	pkFreq->QuadPart = 1000000;
	return TRUE;
}

static BOOL WINAPI HstHook_QueryPerformanceCounter(LARGE_INTEGER* pkCount)
{
	pkCount->QuadPart = (LONGLONG)s_lVirtualMs * 1000;
	return TRUE;
}

static void HstVirtualFileTime(FILETIME* pkTime)
{
	unsigned __int64 ui64 = s_ui64VirtualEpochFileTime + (unsigned __int64)s_lVirtualMs * 10000ULL;
	pkTime->dwLowDateTime = (DWORD)ui64;
	pkTime->dwHighDateTime = (DWORD)(ui64 >> 32);
}

static void WINAPI HstHook_GetSystemTimeAsFileTime(FILETIME* pkTime)
{
	HstVirtualFileTime(pkTime);
}

static void WINAPI HstHook_GetLocalTime(SYSTEMTIME* pkTime)
{
	FILETIME kTime;
	HstVirtualFileTime(&kTime);
	FileTimeToSystemTime(&kTime, pkTime);	// "local" = UTC: machine independent
}

//! CvMap::init calls CoCreateGuid (CvMap.cpp:482) and CvMap::Write serializes the GUID; make it a counter.
static HRESULT WINAPI HstHook_CoCreateGuid(GUID* pkGuid)
{
	const LONG lCount = InterlockedIncrement(&s_lGuidCounter);
	pkGuid->Data1 = 0x43495648;	// 'CIVH'
	pkGuid->Data2 = (unsigned short)(lCount >> 16);
	pkGuid->Data3 = (unsigned short)lCount;
	static const unsigned char s_aucTail[8] = { 'c', 'i', 'v', 'h', 'o', 's', 't', 0 };
	memcpy(pkGuid->Data4, s_aucTail, 8);
	return S_OK;
}

//! Patch one named import of hModule in every import descriptor whose DLL name starts with szDllPrefix.
int HstPatchImport(HMODULE hModule, const char* szDllPrefix, const char* szName, void* pHook)
{
	BYTE* pBase = (BYTE*)hModule;
	IMAGE_DOS_HEADER* pkDos = (IMAGE_DOS_HEADER*)pBase;
	if(!hModule || pkDos->e_magic != IMAGE_DOS_SIGNATURE)
		return 0;
	IMAGE_NT_HEADERS* pkNt = (IMAGE_NT_HEADERS*)(pBase + pkDos->e_lfanew);
	IMAGE_DATA_DIRECTORY& kDir = pkNt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if(kDir.VirtualAddress == 0)
		return 0;
	const size_t uiPrefix = strlen(szDllPrefix);
	int iPatched = 0;
	for(IMAGE_IMPORT_DESCRIPTOR* pkDesc = (IMAGE_IMPORT_DESCRIPTOR*)(pBase + kDir.VirtualAddress); pkDesc->Name; ++pkDesc)
	{
		if(_strnicmp((const char*)(pBase + pkDesc->Name), szDllPrefix, uiPrefix) != 0 || !pkDesc->OriginalFirstThunk)
			continue;
		IMAGE_THUNK_DATA* pkIAT = (IMAGE_THUNK_DATA*)(pBase + pkDesc->FirstThunk);
		IMAGE_THUNK_DATA* pkINT = (IMAGE_THUNK_DATA*)(pBase + pkDesc->OriginalFirstThunk);
		for(int i = 0; pkIAT[i].u1.Function; ++i)
		{
			if(pkINT[i].u1.Ordinal & IMAGE_ORDINAL_FLAG32)
				continue;
			IMAGE_IMPORT_BY_NAME* pkName = (IMAGE_IMPORT_BY_NAME*)(pBase + pkINT[i].u1.AddressOfData);
			if(strcmp((const char*)pkName->Name, szName) != 0)
				continue;
			DWORD dwOld = 0;
			if(VirtualProtect(&pkIAT[i].u1.Function, sizeof(DWORD_PTR), PAGE_READWRITE, &dwOld))
			{
				pkIAT[i].u1.Function = (DWORD_PTR)pHook;
				VirtualProtect(&pkIAT[i].u1.Function, sizeof(DWORD_PTR), dwOld, &dwOld);
				++iPatched;
			}
		}
	}
	return iPatched;
}

static unsigned char s_ucStackFill = 0;

void HstSetStackFill(unsigned char ucFill)
{
	s_ucStackFill = ucFill;
}

__declspec(noinline) void HstWipeStack(unsigned int uiBytes)
{
	void* p = _alloca(uiBytes);
	// Inline asm: not optimized away (the buffer is never read), and a rep stos is faster than SecureZeroMemory's byte loop.
	const unsigned int uiWord = s_ucStackFill * 0x01010101u;
	const unsigned int uiWords = uiBytes / 4;
	__asm
	{
		mov edi, p
		mov eax, uiWord
		mov ecx, uiWords
		cld
		rep stosd
	}
	for(unsigned int i = uiWords * 4; i < uiBytes; ++i)
		((volatile unsigned char*)p)[i] = s_ucStackFill;
}

// ---- x87 control word (determinism axis fpu_cw / fidelity risk D10) ----------------------------------------------------
bool g_bHstFpuCheck = false;
static int s_iFpuBase = 0;					// precision of everything outside the Update slices; 0 = leave the platform default (53 bit)
static int s_iFpuTurn = 0;					// precision of the Update slices; 0 = same as the base
static unsigned int s_uiFpuExpected = 0;
static int s_iFpuChanges = 0;

static int HstFpuValidBits(int iBits)
{
	return (iBits == 24 || iBits == 53 || iBits == 64) ? iBits : 0;
}

static void HstFpuSetPc(int iBits)
{
	unsigned int uiCW = 0;
	const unsigned int uiPc = iBits == 24 ? _PC_24 : (iBits == 53 ? _PC_53 : _PC_64);
	_controlfp_s(&uiCW, uiPc | _RC_NEAR, _MCW_PC | _MCW_RC);
	s_uiFpuExpected = HstGetFpuControlWord();
}

void HstFpuConfigure(int iPcTurn, int iPcGlobal, bool bCheck)
{
	s_iFpuTurn = HstFpuValidBits(iPcTurn);
	s_iFpuBase = HstFpuValidBits(iPcGlobal);
	if(!s_iFpuBase && s_iFpuTurn)
		s_iFpuBase = 53;	// outside the slices the platform default stays in force
	g_bHstFpuCheck = bCheck;
	HstFpuApply();
	s_uiFpuExpected = HstGetFpuControlWord();
}

void HstFpuApply()
{
	if(s_iFpuBase)
		HstFpuSetPc(s_iFpuBase);
}

void HstFpuApplyTurn()
{
	if(s_iFpuTurn)
		HstFpuSetPc(s_iFpuTurn);
	else
		HstFpuApply();
}

void HstFpuEndTurnSlice()
{
	if(s_iFpuTurn)
		HstFpuApply();
}

int HstFpuGetPcBits()
{
	return s_iFpuTurn ? s_iFpuTurn : s_iFpuBase;
}

void HstFpuCheckSlow(const char* szWhere)
{
	const unsigned int uiCW = HstGetFpuControlWord();
	if(uiCW == s_uiFpuExpected)
		return;
	++s_iFpuChanges;
	if(s_iFpuChanges <= 40)
		HstLog("fpu: control word changed 0x%04x -> 0x%04x during/after %s", s_uiFpuExpected, uiCW, szWhere ? szWhere : "?");
	s_uiFpuExpected = uiCW;
}

int HstFpuGetChangeCount()
{
	return s_iFpuChanges;
}

// Fixed stack position for game entry points. The DLL stores stack addresses in its state (e.g. an uninitialized
// 8-byte field of every replay message holds a stack address: game segment, finding 14), so the stack pointer at
// each DLL entry must not depend on which host code path (CLI or control channel) leads there.
static UINT_PTR s_uiHstFixedStack = 0;
static bool s_bHstFixedStackWarned = false;

__declspec(noinline) void HstFixedStackInit(unsigned int uiMarginBytes)
{
	char cProbe = 0;
	s_uiHstFixedStack = (((UINT_PTR)&cProbe) - uiMarginBytes) & ~(UINT_PTR)0xFFFF;
}

unsigned int HstFixedStackGet()
{
	return (unsigned int)s_uiHstFixedStack;
}

__declspec(noinline) void HstRunAtFixedStack(void (*pfn)(void*), void* pUser)
{
	char cProbe = 0;
	const UINT_PTR uiHere = (UINT_PTR)&cProbe;
	UINT_PTR uiTarget = s_uiHstFixedStack;
	HstFpuApply();	// --fpu-pc: the precision control is re-asserted on every DLL entry
	if(!uiTarget || uiHere < uiTarget + 1024)
	{
		if(uiTarget && !s_bHstFixedStackWarned)
		{
			s_bHstFixedStackWarned = true;
			HstLog("stack: WARNING already below the fixed stack position (0x%08x < 0x%08x); stack addresses vary", (unsigned int)uiHere, (unsigned int)uiTarget);
		}
		pfn(pUser);
		HST_FPU_CHECK("session entry");
		return;
	}
	// Commit the pages down to the target (stack pages are committed on demand through the guard page).
	volatile char* pProbe = (volatile char*)_alloca(uiHere - uiTarget + 4096);
	pProbe[0] = 0;
	__asm
	{
		mov ecx, pfn
		mov edx, pUser
		mov eax, uiTarget
		mov ebx, esp
		mov esp, eax
		push edx
		call ecx
		mov esp, ebx
	}
	HST_FPU_CHECK("session entry");
}

// SQLite (inside the database DLL) expands every file name with GetFullPathName and keeps the result in heap blocks;
// with relative names (the DLL's cache folder is ".\") returning the name unchanged keeps the run dir's length out of
// the deterministic heap. Relative names stay valid: the CWD is the run dir for the whole process lifetime.
typedef DWORD (WINAPI* PFN_GetFullPathNameW)(LPCWSTR, DWORD, LPWSTR, LPWSTR*);
typedef DWORD (WINAPI* PFN_GetFullPathNameA)(LPCSTR, DWORD, LPSTR, LPSTR*);
static PFN_GetFullPathNameW s_pfnGetFullPathNameW = NULL;
static PFN_GetFullPathNameA s_pfnGetFullPathNameA = NULL;

template<class C> static bool HstIsRelativePath(const C* sz)
{
	if(!sz || !sz[0])
		return false;
	if(sz[0] == '\\' || sz[0] == '/')
		return false;
	return !(sz[1] == ':');
}

template<class C> static DWORD HstKeepRelative(const C* szIn, DWORD dwBuf, C* szOut, C** pszFilePart)
{
	size_t uiLen = 0;
	while(szIn[uiLen])
		++uiLen;
	if(!szOut || dwBuf < uiLen + 1)
		return (DWORD)(uiLen + 1);
	memcpy(szOut, szIn, (uiLen + 1) * sizeof(C));
	if(pszFilePart)
	{
		*pszFilePart = szOut;
		for(size_t i = 0; i < uiLen; ++i)
		{
			if(szOut[i] == '\\' || szOut[i] == '/')
				*pszFilePart = szOut + i + 1;
		}
	}
	return (DWORD)uiLen;
}

static DWORD WINAPI HstHook_GetFullPathNameW(LPCWSTR wszIn, DWORD dwBuf, LPWSTR wszOut, LPWSTR* pwszFilePart)
{
	if(HstIsRelativePath(wszIn))
		return HstKeepRelative(wszIn, dwBuf, wszOut, pwszFilePart);
	return s_pfnGetFullPathNameW(wszIn, dwBuf, wszOut, pwszFilePart);
}

static DWORD WINAPI HstHook_GetFullPathNameA(LPCSTR szIn, DWORD dwBuf, LPSTR szOut, LPSTR* pszFilePart)
{
	if(HstIsRelativePath(szIn))
		return HstKeepRelative(szIn, dwBuf, szOut, pszFilePart);
	return s_pfnGetFullPathNameA(szIn, dwBuf, szOut, pszFilePart);
}

int HstRelativePathPatchModule(HMODULE hModule)
{
	HMODULE hKernel = GetModuleHandleW(L"kernel32.dll");
	s_pfnGetFullPathNameW = (PFN_GetFullPathNameW)GetProcAddress(hKernel, "GetFullPathNameW");
	s_pfnGetFullPathNameA = (PFN_GetFullPathNameA)GetProcAddress(hKernel, "GetFullPathNameA");
	if(!s_pfnGetFullPathNameW || !s_pfnGetFullPathNameA)
		return 0;
	return HstPatchImport(hModule, "kernel32", "GetFullPathNameW", (void*)HstHook_GetFullPathNameW) +
		HstPatchImport(hModule, "kernel32", "GetFullPathNameA", (void*)HstHook_GetFullPathNameA);
}

int HstDeterminismPatchModule(HMODULE hModule)
{
	int iPatched = 0;
	iPatched += HstPatchImport(hModule, "winmm", "timeGetTime", (void*)HstHook_timeGetTime);
	iPatched += HstPatchImport(hModule, "kernel32", "GetTickCount", (void*)HstHook_GetTickCount);
	iPatched += HstPatchImport(hModule, "kernel32", "QueryPerformanceFrequency", (void*)HstHook_QueryPerformanceFrequency);
	iPatched += HstPatchImport(hModule, "kernel32", "QueryPerformanceCounter", (void*)HstHook_QueryPerformanceCounter);
	iPatched += HstPatchImport(hModule, "kernel32", "GetSystemTimeAsFileTime", (void*)HstHook_GetSystemTimeAsFileTime);
	iPatched += HstPatchImport(hModule, "kernel32", "GetLocalTime", (void*)HstHook_GetLocalTime);
	iPatched += HstPatchImport(hModule, "ole32", "CoCreateGuid", (void*)HstHook_CoCreateGuid);
	return iPatched;
}
