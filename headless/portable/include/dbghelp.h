// Portable replacement for <dbghelp.h>: types for FDebugHelper's declarations; stack walking is unavailable.
#ifndef LEKMOD_PORTABLE_DBGHELP_H
#define LEKMOD_PORTABLE_DBGHELP_H
#include <windows.h>

typedef struct _CONTEXT {
	DWORD64 Rip;
	DWORD64 Rsp;
	DWORD64 Rbp;
} CONTEXT, *PCONTEXT;

typedef enum { AddrMode1616, AddrMode1632, AddrModeReal, AddrModeFlat } ADDRESS_MODE;
typedef struct _tagADDRESS64 {
	DWORD64 Offset;
	WORD Segment;
	ADDRESS_MODE Mode;
} ADDRESS64;
typedef struct _tagSTACKFRAME64 {
	ADDRESS64 AddrPC;
	ADDRESS64 AddrReturn;
	ADDRESS64 AddrFrame;
	ADDRESS64 AddrStack;
	ADDRESS64 AddrBStore;
} STACKFRAME64, *LPSTACKFRAME64;
#define IMAGE_FILE_MACHINE_I386 0x014c
#define IMAGE_FILE_MACHINE_AMD64 0x8664
typedef PVOID (*PFUNCTION_TABLE_ACCESS_ROUTINE64)(HANDLE, DWORD64);
typedef DWORD64 (*PGET_MODULE_BASE_ROUTINE64)(HANDLE, DWORD64);
inline PVOID SymFunctionTableAccess64(HANDLE, DWORD64) { return 0; }
inline DWORD64 SymGetModuleBase64(HANDLE, DWORD64) { return 0; }
inline BOOL StackWalk64(DWORD, HANDLE, HANDLE, LPSTACKFRAME64, PVOID, PVOID, PFUNCTION_TABLE_ACCESS_ROUTINE64, PGET_MODULE_BASE_ROUTINE64, PVOID)
{
	return FALSE;
}

// Minidumps (AUI_MINIDUMPS): types only; writing a dump always fails outside Win32.
typedef enum _MINIDUMP_TYPE {
	MiniDumpNormal = 0x00000000,
	MiniDumpWithDataSegs = 0x00000001,
	MiniDumpWithFullMemory = 0x00000002,
	MiniDumpWithHandleData = 0x00000004,
	MiniDumpWithUnloadedModules = 0x00000020,
	MiniDumpWithIndirectlyReferencedMemory = 0x00000040,
	MiniDumpWithProcessThreadData = 0x00000100,
	MiniDumpWithPrivateReadWriteMemory = 0x00000200,
	MiniDumpWithFullMemoryInfo = 0x00000800,
	MiniDumpWithThreadInfo = 0x00001000,
	MiniDumpWithCodeSegs = 0x00002000,
	MiniDumpWithFullAuxiliaryState = 0x00008000
} MINIDUMP_TYPE;
typedef struct _MINIDUMP_EXCEPTION_INFORMATION {
	DWORD ThreadId;
	PEXCEPTION_POINTERS ExceptionPointers;
	BOOL ClientPointers;
} MINIDUMP_EXCEPTION_INFORMATION;
inline BOOL SymInitialize(HANDLE, PCSTR, BOOL) { return FALSE; }
inline BOOL MiniDumpWriteDump(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, const MINIDUMP_EXCEPTION_INFORMATION*, PVOID, PVOID)
{
	SetLastError(ERROR_ACCESS_DENIED);
	return FALSE;
}
#endif
