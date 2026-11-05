//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstPlatform.h
//!  \brief     The only place in civhost that talks to the OS: paths, logging, DLL search path, FPU,
//!				crash handling (SEH minidump + symbolized stack) and the IAT file sandbox (design 5/R7).
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#pragma once
#ifndef HSTPLATFORM_H
#define HSTPLATFORM_H

#include "HstCommon.h"

// ---- logging -----------------------------------------------------------------------------------------------------
bool HstLogOpen(const std::wstring& wstrPath);
void HstLog(const char* szFormat, ...);
void HstLogFlush();
//! Log, print the caller stack, flush and exit the process with iExitCode.
void HstFatal(int iExitCode, const char* szFormat, ...);

// ---- strings / paths ---------------------------------------------------------------------------------------------
std::string HstToUtf8(const std::wstring& wstr);
std::wstring HstFromUtf8(const std::string& str);
std::wstring HstFromAcp(const char* sz);
std::wstring HstExeDir();
std::wstring HstFullPath(const std::wstring& wstrPath);
std::wstring HstJoinPath(const std::wstring& wstrA, const std::wstring& wstrB);
bool HstFileExists(const std::wstring& wstrPath);
bool HstDirExists(const std::wstring& wstrPath);
bool HstCreateDirs(const std::wstring& wstrPath);
//! Copy wstrSrc to wstrDst unless wstrDst already exists with the same size and write time. Clears read-only.
bool HstCopyIfChanged(const std::wstring& wstrSrc, const std::wstring& wstrDst);

// ---- process setup -----------------------------------------------------------------------------------------------
//! Creates the run dir, makes it the CWD, points the DLL search path at the game root (P1/P2) and sets the FPU (P3).
bool HstPlatformInit(const std::wstring& wstrRunDir, const std::wstring& wstrGameDir);
const std::wstring& HstGetRunDir();
const std::wstring& HstGetGameDir();
unsigned int HstGetFpuControlWord();
//! Extra symbol search directory (folder of the gamecore DLL/PDB).
void HstAddSymbolPath(const std::wstring& wstrDir);

// ---- crash handling ----------------------------------------------------------------------------------------------
//! __except filter: writes <run>\civhost-crash.dmp, prints exception, faulting module+offset and the stack.
LONG HstCrashFilter(EXCEPTION_POINTERS* pkExceptionPointers);
//! Re-installs the host's unhandled-exception filter (GC.init() installs the DLL's own AUI_MINIDUMPS filter).
void HstInstallUnhandledFilter();
//! Log the first 20 first-chance error/C++ exceptions (module+offset); diagnostic for crashes the loader swallows.
void HstInstallFirstChanceLogger();
//! Print a symbolized stack for pkContext (NULL: the caller's stack).
void HstPrintStack(const CONTEXT* pkContext, int iSkipFrames);
//! "module+0xoffset (symbol+0xdisp)" for an address.
std::string HstDescribeAddress(const void* pAddress);
//! Function, source file (full path as in the PDB) and line of an address through dbghelp; uiRva = offset in its module,
//! puiDisp (optional) = offset of the address inside the function.
bool HstResolveAddress(const void* pAddress, std::string& strFunction, std::string& strFile, unsigned int& uiLine, unsigned int& uiRva,
	unsigned int* puiDisp = NULL);

// ---- IAT file sandbox (design 5/R7) ------------------------------------------------------------------------------
//! Opens <run>\sandbox.log. Writes are allowed only below the run dir; reads anywhere; everything is logged.
bool HstSandboxInit(const std::wstring& wstrRunDir);
//! Patch the kernel32 file APIs imported by hModule. Returns the number of patched IAT slots.
int HstSandboxPatchModule(HMODULE hModule);
//! Patch every already-loaded module named in the built-in list (game DLLs + MSVCR90/MSVCP90/dbghelp).
int HstSandboxPatchKnownModules();
//! Denied writes by the game modules / CRT (must stay 0).
int HstSandboxGetDenied();
//! Denied write-opens by dbghelp (PDB open-for-update probing during symbol load; it falls back to read-only).
int HstSandboxGetDeniedTool();
int HstSandboxGetWritesAllowed();
bool HstSandboxIsWriteAllowed(const wchar_t* wszPath);
void HstSandboxFlush();

// ---- determinism (IAT): virtual time + deterministic GUIDs ---------------------------------------------------------
//! Patch timeGetTime, GetTickCount, QueryPerformance*, GetSystemTimeAsFileTime, GetLocalTime (virtual clock that
//! only the host advances) and CoCreateGuid (counter GUIDs; CvMap::Write serializes the map GUID).
int HstDeterminismPatchModule(HMODULE hModule);
//! Axis stack_fill (determinism lab): the byte HstWipeStack writes instead of zero.
void HstSetStackFill(unsigned char ucFill);
//! Axes fpu_pc / fpu_pc_global / --fpu-check. 24, 53 or 64 selects the x87 precision control, anything else leaves the
//! platform default (53). iPcTurn applies to the Update slices only (HstFpuApplyTurn before, HstFpuEndTurnSlice after:
//! the sensitivity of the simulation itself); iPcGlobal applies to everything else from the start (boot, map generation,
//! Lua, loads: HstFpuApply on every DLL entry). bCheck logs every change of the control word that HstFpuCheck sees (DLL, Lua).
void HstFpuConfigure(int iPcTurn, int iPcGlobal, bool bCheck);
void HstFpuApply();
void HstFpuApplyTurn();
void HstFpuEndTurnSlice();
int HstFpuGetPcBits();
void HstFpuCheckSlow(const char* szWhere);
int HstFpuGetChangeCount();
extern bool g_bHstFpuCheck;
#define HST_FPU_CHECK(where) do { if(g_bHstFpuCheck) HstFpuCheckSlow(where); } while(0)
//! Zero uiBytes of the stack below the caller's frame (or the --stack-fill pattern). The DLL serializes uninitialized locals (stack residue); host
//! code that handles run-specific data (paths, timings) leaves residue that would reach the digests. The main thread
//! stack sits at a fixed address, so wiped residue makes the bytes a function of the DLL's own call history only.
void HstWipeStack(unsigned int uiBytes);
//! Record the fixed stack position (uiMarginBytes below the caller, 64 KB aligned); call once at the top of wmain.
void HstFixedStackInit(unsigned int uiMarginBytes);
//! Call pfn(pUser) with ESP at the fixed position, so DLL code sees the same stack addresses whatever host path leads
//! to it (stack addresses end up in serialized state). Falls back to a direct call when already below it.
void HstRunAtFixedStack(void (*pfn)(void*), void* pUser);
unsigned int HstFixedStackGet();
//! GetFullPathNameW/A keep relative names relative (database DLL: SQLite file names; see HstPlatform.cpp).
int HstRelativePathPatchModule(HMODULE hModule);
//! Patch one named import of hModule (import descriptors whose DLL name starts with szDllPrefix). Returns slots patched.
int HstPatchImport(HMODULE hModule, const char* szDllPrefix, const char* szName, void* pHook);
void HstVirtualTimeAdvance(unsigned int uiMs);
unsigned int HstVirtualTimeMs();
//! Checkpoint restore of the virtual clock and the CoCreateGuid counter.
void HstVirtualTimeSet(unsigned int uiMs);
unsigned int HstGuidCounterGet();
void HstGuidCounterSet(unsigned int uiCount);
//! false: HstLog writes only to the log file (--control stdio keeps stdout for the protocol).
void HstLogSetConsole(bool bConsole);

#endif // HSTPLATFORM_H
