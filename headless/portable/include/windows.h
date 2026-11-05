// Portable replacement for <windows.h>: the subset of Win32 the gamecore and the Firaxis headers use.
// Win32 integer widths are kept (DWORD/LONG/ULONG are 32 bit on every target); pointer-sized types follow the target.
#ifndef LEKMOD_PORTABLE_WINDOWS_H
#define LEKMOD_PORTABLE_WINDOWS_H

#include "lekmod_portable/prelude.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#if !defined(__wasi__)
#include <pthread.h>
#include <sched.h>
#endif
#include <unistd.h>

// ---- base types ------------------------------------------------------------------------------
typedef int BOOL;
typedef unsigned char BYTE;
typedef unsigned char UCHAR;
typedef unsigned short WORD;
typedef unsigned short USHORT;
typedef unsigned int DWORD;
typedef unsigned int UINT;
typedef unsigned int ULONG;
typedef int INT;
typedef int LONG;
typedef short SHORT;
typedef char CHAR;
typedef wchar_t WCHAR;
typedef float FLOAT;
typedef signed char INT8;
typedef unsigned char UINT8;
typedef short INT16;
typedef unsigned short UINT16;
typedef int INT32;
typedef unsigned int UINT32;
typedef long long INT64;
typedef unsigned long long UINT64;
typedef long long LONGLONG;
typedef unsigned long long ULONGLONG;
typedef long long LONG64;
typedef unsigned long long DWORD64;
typedef intptr_t INT_PTR;
typedef uintptr_t UINT_PTR;
typedef intptr_t LONG_PTR;
typedef uintptr_t ULONG_PTR;
typedef uintptr_t DWORD_PTR;
typedef size_t SIZE_T;
typedef BYTE BOOLEAN;
typedef void VOID;
typedef void* PVOID;
typedef void* LPVOID;
typedef const void* LPCVOID;
typedef char* LPSTR;
typedef char* PSTR;
typedef const char* LPCSTR;
typedef const char* PCSTR;
typedef wchar_t* LPWSTR;
typedef wchar_t* PWSTR;
typedef const wchar_t* LPCWSTR;
typedef const wchar_t* PCWSTR;
typedef BYTE* LPBYTE;
typedef BYTE* PBYTE;
typedef DWORD* LPDWORD;
typedef DWORD* PDWORD;
typedef LONG* PLONG;
typedef LONG* LPLONG;
typedef BOOL* LPBOOL;
typedef INT32* PINT32;
typedef UINT32* PUINT32;
typedef int HRESULT;
typedef WORD ATOM;

#if !defined(LEKMOD_PORTABLE_TCHAR_DEFINED)
#define LEKMOD_PORTABLE_TCHAR_DEFINED
typedef char TCHAR;
typedef char _TCHAR;
typedef const char* LPCTSTR;
typedef char* LPTSTR;
#endif

typedef void* HANDLE;
typedef HANDLE* PHANDLE;
typedef struct HINSTANCE__* HINSTANCE;
typedef HINSTANCE HMODULE;
typedef struct HWND__* HWND;
typedef struct HKEY__* HKEY;
typedef UINT_PTR WPARAM;
typedef LONG_PTR LPARAM;
typedef LONG_PTR LRESULT;

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef NULL
#define NULL 0
#endif
#ifndef MAXINT
#define MAXINT ((INT)0x7fffffff)
#define MININT ((INT)0x80000000)
#define MAXUINT ((UINT)~((UINT)0))
#define MAXDWORD 0xffffffff
#endif
#ifndef MAX_PATH
#define MAX_PATH 260
#endif

#define WINAPI
#define APIENTRY
#define CALLBACK
#define STDMETHODCALLTYPE
#ifndef CONST
#define CONST const
#endif
#define IN
#define OUT
#define OPTIONAL
#define UNREFERENCED_PARAMETER(P) (void)(P)

#define S_OK ((HRESULT)0)
#define S_FALSE ((HRESULT)1)
#define E_FAIL ((HRESULT)0x80004005)
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define FAILED(hr) (((HRESULT)(hr)) < 0)

#define MAKEWORD(a, b) ((WORD)(((BYTE)(((DWORD_PTR)(a)) & 0xff)) | ((WORD)((BYTE)(((DWORD_PTR)(b)) & 0xff))) << 8))
#define MAKELONG(a, b) ((LONG)(((WORD)(((DWORD_PTR)(a)) & 0xffff)) | ((DWORD)((WORD)(((DWORD_PTR)(b)) & 0xffff))) << 16))
#define LOWORD(l) ((WORD)(((DWORD_PTR)(l)) & 0xffff))
#define HIWORD(l) ((WORD)((((DWORD_PTR)(l)) >> 16) & 0xffff))
#define LOBYTE(w) ((BYTE)(((DWORD_PTR)(w)) & 0xff))
#define HIBYTE(w) ((BYTE)((((DWORD_PTR)(w)) >> 8) & 0xff))

typedef union _LARGE_INTEGER {
	struct {
		DWORD LowPart;
		LONG HighPart;
	};
	struct {
		DWORD LowPart;
		LONG HighPart;
	} u;
	LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef union _ULARGE_INTEGER {
	struct {
		DWORD LowPart;
		DWORD HighPart;
	};
	struct {
		DWORD LowPart;
		DWORD HighPart;
	} u;
	ULONGLONG QuadPart;
} ULARGE_INTEGER, *PULARGE_INTEGER;

typedef struct _FILETIME {
	DWORD dwLowDateTime;
	DWORD dwHighDateTime;
} FILETIME, *PFILETIME, *LPFILETIME;

typedef struct _SYSTEMTIME {
	WORD wYear;
	WORD wMonth;
	WORD wDayOfWeek;
	WORD wDay;
	WORD wHour;
	WORD wMinute;
	WORD wSecond;
	WORD wMilliseconds;
} SYSTEMTIME, *PSYSTEMTIME, *LPSYSTEMTIME;

typedef struct tagPOINT {
	LONG x;
	LONG y;
} POINT, *PPOINT, *LPPOINT;

typedef struct tagRECT {
	LONG left;
	LONG top;
	LONG right;
	LONG bottom;
} RECT, *PRECT, *LPRECT;

typedef struct tagSIZE {
	LONG cx;
	LONG cy;
} SIZE, *PSIZE, *LPSIZE;

#ifndef GUID_DEFINED
#define GUID_DEFINED
typedef struct _GUID {
	unsigned int Data1;
	unsigned short Data2;
	unsigned short Data3;
	unsigned char Data4[8];
} GUID;
#endif
typedef GUID IID;
typedef GUID CLSID;
#define REFGUID const GUID&
#define REFIID const IID&
inline bool operator==(const GUID& a, const GUID& b) { return memcmp(&a, &b, sizeof(GUID)) == 0; }
inline bool operator!=(const GUID& a, const GUID& b) { return !(a == b); }

#define DECLSPEC_DEPRECATED
#define DECLSPEC_NOVTABLE
#define DECLSPEC_NOINLINE __attribute__((noinline))

// Structured exceptions do not exist outside Win32; the type only appears in unused signatures.
typedef struct _EXCEPTION_POINTERS {
	void* ExceptionRecord;
	void* ContextRecord;
} EXCEPTION_POINTERS, *PEXCEPTION_POINTERS;
#define EXCEPTION_EXECUTE_HANDLER 1
#define EXCEPTION_CONTINUE_SEARCH 0
typedef LONG (*LPTOP_LEVEL_EXCEPTION_FILTER)(PEXCEPTION_POINTERS);
inline LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER) { return 0; }

// ---- time ------------------------------------------------------------------------------------
namespace lekmod_portable {
inline unsigned long long monotonic_ns()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
}
}  // namespace lekmod_portable

inline DWORD GetTickCount() { return (DWORD)(lekmod_portable::monotonic_ns() / 1000000ull); }
inline ULONGLONG GetTickCount64() { return lekmod_portable::monotonic_ns() / 1000000ull; }
inline DWORD timeGetTime() { return GetTickCount(); }
#define TIMERR_NOERROR 0
inline UINT timeBeginPeriod(UINT) { return TIMERR_NOERROR; }
inline UINT timeEndPeriod(UINT) { return TIMERR_NOERROR; }
inline BOOL QueryPerformanceFrequency(LARGE_INTEGER* f)
{
	f->QuadPart = 1000000000ll;
	return TRUE;
}
inline BOOL QueryPerformanceCounter(LARGE_INTEGER* c)
{
	c->QuadPart = (LONGLONG)lekmod_portable::monotonic_ns();
	return TRUE;
}
inline void Sleep(DWORD ms)
{
#if defined(__wasi__)
	(void)ms;
#else
	usleep((useconds_t)ms * 1000u);
#endif
}
inline BOOL SwitchToThread() { return TRUE; }
inline void GetLocalTime(SYSTEMTIME* st)
{
	time_t now = time(0);
	struct tm t;
	localtime_r(&now, &t);
	st->wYear = (WORD)(t.tm_year + 1900);
	st->wMonth = (WORD)(t.tm_mon + 1);
	st->wDayOfWeek = (WORD)t.tm_wday;
	st->wDay = (WORD)t.tm_mday;
	st->wHour = (WORD)t.tm_hour;
	st->wMinute = (WORD)t.tm_min;
	st->wSecond = (WORD)t.tm_sec;
	st->wMilliseconds = 0;
}
inline void GetSystemTime(SYSTEMTIME* st) { GetLocalTime(st); }

// ---- debug / errors --------------------------------------------------------------------------
inline void OutputDebugStringA(const char*) {}
inline void OutputDebugStringW(const wchar_t*) {}
#define OutputDebugString OutputDebugStringA
inline void DebugBreak() {}
inline BOOL IsDebuggerPresent() { return FALSE; }

#define ERROR_SUCCESS 0L
#define ERROR_FILE_NOT_FOUND 2L
#define ERROR_PATH_NOT_FOUND 3L
#define ERROR_ACCESS_DENIED 5L
#define ERROR_ALREADY_EXISTS 183L
namespace lekmod_portable {
DWORD& last_error();
}
inline DWORD GetLastError() { return lekmod_portable::last_error(); }
inline void SetLastError(DWORD e) { lekmod_portable::last_error() = e; }

inline DWORD GetCurrentThreadId()
{
#if defined(__wasi__)
	return 1;
#else
	return (DWORD)(uintptr_t)pthread_self();
#endif
}
inline DWORD GetCurrentProcessId()
{
#if defined(__wasi__)
	return 1;
#else
	return (DWORD)getpid();
#endif
}
inline HANDLE GetCurrentProcess() { return (HANDLE)(intptr_t)-1; }
inline HANDLE GetCurrentThread() { return (HANDLE)(intptr_t)-2; }

// ---- interlocked -----------------------------------------------------------------------------
inline LONG InterlockedIncrement(volatile LONG* p) { return __sync_add_and_fetch(p, 1); }
inline LONG InterlockedDecrement(volatile LONG* p) { return __sync_sub_and_fetch(p, 1); }
inline LONG InterlockedExchange(volatile LONG* p, LONG v) { return __sync_lock_test_and_set(p, v); }
inline LONG InterlockedExchangeAdd(volatile LONG* p, LONG v) { return __sync_fetch_and_add(p, v); }
inline LONG InterlockedCompareExchange(volatile LONG* p, LONG xchg, LONG cmp) { return __sync_val_compare_and_swap(p, cmp, xchg); }
inline LONG InterlockedOr(volatile LONG* p, LONG v) { return __sync_fetch_and_or(p, v); }
inline LONG InterlockedAnd(volatile LONG* p, LONG v) { return __sync_fetch_and_and(p, v); }
inline LONG InterlockedXor(volatile LONG* p, LONG v) { return __sync_fetch_and_xor(p, v); }
inline LONGLONG InterlockedIncrement64(volatile LONGLONG* p) { return __sync_add_and_fetch(p, 1); }
inline LONGLONG InterlockedDecrement64(volatile LONGLONG* p) { return __sync_sub_and_fetch(p, 1); }
inline LONGLONG InterlockedExchangeAdd64(volatile LONGLONG* p, LONGLONG v) { return __sync_fetch_and_add(p, v); }
inline LONGLONG InterlockedCompareExchange64(volatile LONGLONG* p, LONGLONG xchg, LONGLONG cmp) { return __sync_val_compare_and_swap(p, cmp, xchg); }
inline PVOID InterlockedCompareExchangePointer(PVOID volatile* p, PVOID xchg, PVOID cmp) { return __sync_val_compare_and_swap(p, cmp, xchg); }
inline PVOID InterlockedExchangePointer(PVOID volatile* p, PVOID v) { return __sync_lock_test_and_set(p, v); }
#define MemoryBarrier() __sync_synchronize()
#define _ReadWriteBarrier() __asm__ __volatile__("" ::: "memory")
#define YieldProcessor() ((void)0)

// ---- critical sections (recursive, like Win32) ------------------------------------------------
typedef struct _RTL_CRITICAL_SECTION {
#if defined(__wasi__)
	int depth;  // single-threaded target: only the nesting count is tracked
#else
	pthread_mutex_t mutex;
#endif
} CRITICAL_SECTION, *LPCRITICAL_SECTION, *PCRITICAL_SECTION;

inline void InitializeCriticalSection(CRITICAL_SECTION* cs)
{
#if defined(__wasi__)
	cs->depth = 0;
#else
	pthread_mutexattr_t a;
	pthread_mutexattr_init(&a);
	pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&cs->mutex, &a);
	pthread_mutexattr_destroy(&a);
#endif
}
inline BOOL InitializeCriticalSectionAndSpinCount(CRITICAL_SECTION* cs, DWORD)
{
	InitializeCriticalSection(cs);
	return TRUE;
}
inline void DeleteCriticalSection(CRITICAL_SECTION* cs)
{
#if defined(__wasi__)
	(void)cs;
#else
	pthread_mutex_destroy(&cs->mutex);
#endif
}
inline void EnterCriticalSection(CRITICAL_SECTION* cs)
{
#if defined(__wasi__)
	++cs->depth;
#else
	pthread_mutex_lock(&cs->mutex);
#endif
}
inline BOOL TryEnterCriticalSection(CRITICAL_SECTION* cs)
{
#if defined(__wasi__)
	++cs->depth;
	return TRUE;
#else
	return pthread_mutex_trylock(&cs->mutex) == 0;
#endif
}
inline void LeaveCriticalSection(CRITICAL_SECTION* cs)
{
#if defined(__wasi__)
	--cs->depth;
#else
	pthread_mutex_unlock(&cs->mutex);
#endif
}

// ---- memory ----------------------------------------------------------------------------------
#define ZeroMemory(dst, len) memset((dst), 0, (len))
#define SecureZeroMemory(dst, len) memset((dst), 0, (len))
#define FillMemory(dst, len, fill) memset((dst), (fill), (len))
#define CopyMemory(dst, src, len) memcpy((dst), (src), (len))
#define MoveMemory(dst, src, len) memmove((dst), (src), (len))

// ---- heaps (HeapCreate/HeapAlloc over malloc) -------------------------------------------------
#define HEAP_NO_SERIALIZE 0x00000001
#define HEAP_GENERATE_EXCEPTIONS 0x00000004
#define HEAP_ZERO_MEMORY 0x00000008
typedef enum _HEAP_INFORMATION_CLASS { HeapCompatibilityInformation = 0 } HEAP_INFORMATION_CLASS;
inline HANDLE HeapCreate(DWORD, SIZE_T, SIZE_T) { return (HANDLE)(intptr_t)0x4ea9; }
inline BOOL HeapDestroy(HANDLE) { return TRUE; }
inline BOOL HeapSetInformation(HANDLE, HEAP_INFORMATION_CLASS, PVOID, SIZE_T) { return TRUE; }
inline LPVOID HeapAlloc(HANDLE, DWORD flags, SIZE_T n)
{
	return (flags & HEAP_ZERO_MEMORY) ? calloc(1, n ? n : 1) : malloc(n ? n : 1);
}
inline LPVOID HeapReAlloc(HANDLE, DWORD, LPVOID p, SIZE_T n) { return realloc(p, n ? n : 1); }
inline BOOL HeapFree(HANDLE, DWORD, LPVOID p)
{
	free(p);
	return TRUE;
}
inline HANDLE GetProcessHeap() { return (HANDLE)(intptr_t)0x4ea9; }

// ---- GUIDs -----------------------------------------------------------------------------------
HRESULT CoCreateGuid(GUID* guid);

// ---- code pages ------------------------------------------------------------------------------
#define CP_ACP 0
#define CP_OEMCP 1
#define CP_UTF8 65001
// CP_ACP is treated as UTF-8. wchar_t strings are UTF-32 on Linux/WASI (UTF-16 on Win32).
int MultiByteToWideChar(UINT codePage, DWORD flags, LPCSTR src, int srcLen, LPWSTR dst, int dstLen);
int WideCharToMultiByte(UINT codePage, DWORD flags, LPCWSTR src, int srcLen, LPSTR dst, int dstLen, LPCSTR defaultChar, LPBOOL usedDefault);

// ---- files (stdio-backed HANDLEs) ------------------------------------------------------------
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INVALID_FILE_SIZE ((DWORD)0xFFFFFFFF)
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 0x00000001
#define FILE_SHARE_WRITE 0x00000002
#define FILE_SHARE_DELETE 0x00000004
#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5
#define FILE_ATTRIBUTE_READONLY 0x00000001
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010
#define FILE_ATTRIBUTE_NORMAL 0x00000080
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000
#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2
typedef struct _SECURITY_ATTRIBUTES* LPSECURITY_ATTRIBUTES;
typedef struct _OVERLAPPED* LPOVERLAPPED;

HANDLE CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags, HANDLE templ);
HANDLE CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags, HANDLE templ);
#define CreateFile CreateFileA
BOOL ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD read, LPOVERLAPPED ov);
BOOL WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD written, LPOVERLAPPED ov);
DWORD GetFileSize(HANDLE h, LPDWORD high);
DWORD SetFilePointer(HANDLE h, LONG dist, PLONG distHigh, DWORD method);
BOOL CloseHandle(HANDLE h);
BOOL DeleteFileA(LPCSTR name);
BOOL DeleteFileW(LPCWSTR name);
#define DeleteFile DeleteFileA
DWORD GetFileAttributesA(LPCSTR name);
DWORD GetFileAttributesW(LPCWSTR name);
#define GetFileAttributes GetFileAttributesA
typedef enum _GET_FILEEX_INFO_LEVELS { GetFileExInfoStandard } GET_FILEEX_INFO_LEVELS;
typedef struct _WIN32_FILE_ATTRIBUTE_DATA {
	DWORD dwFileAttributes;
	FILETIME ftCreationTime;
	FILETIME ftLastAccessTime;
	FILETIME ftLastWriteTime;
	DWORD nFileSizeHigh;
	DWORD nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA, *LPWIN32_FILE_ATTRIBUTE_DATA;
BOOL GetFileAttributesExA(LPCSTR name, GET_FILEEX_INFO_LEVELS level, LPVOID info);
BOOL GetFileAttributesExW(LPCWSTR name, GET_FILEEX_INFO_LEVELS level, LPVOID info);
#define GetFileAttributesEx GetFileAttributesExA
inline LONG CompareFileTime(const FILETIME* a, const FILETIME* b)
{
	ULONGLONG x = ((ULONGLONG)a->dwHighDateTime << 32) | a->dwLowDateTime;
	ULONGLONG y = ((ULONGLONG)b->dwHighDateTime << 32) | b->dwLowDateTime;
	return x < y ? -1 : (x > y ? 1 : 0);
}
BOOL CreateDirectoryA(LPCSTR name, LPSECURITY_ATTRIBUTES sa);
#define CreateDirectory CreateDirectoryA

// ---- modules ---------------------------------------------------------------------------------
inline DWORD GetModuleFileNameA(HMODULE, LPSTR buf, DWORD n)
{
	if (n) buf[0] = 0;
	return 0;
}
#define GetModuleFileName GetModuleFileNameA
inline HMODULE GetModuleHandleA(LPCSTR) { return 0; }
#define GetModuleHandle GetModuleHandleA

#define DLL_PROCESS_DETACH 0
#define DLL_PROCESS_ATTACH 1
#define DLL_THREAD_ATTACH 2
#define DLL_THREAD_DETACH 3

#endif  // LEKMOD_PORTABLE_WINDOWS_H
