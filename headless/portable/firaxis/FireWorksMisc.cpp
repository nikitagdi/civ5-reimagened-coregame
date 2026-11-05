// Small FireWorks pieces the gamecore links against: critical sections, CRC, temp heap, timer, aligned
// allocation, log/file-system singletons, FDebugHelper, FAutoArchive/FAutoVariableBase, FSerialization.
// The FireWorks headers expect the gamecore precompiled-header environment.
#include "CvGameCoreDLLPCH.h"

#include <algorithm>
#include <set>
#include <stdarg.h>
#include <stdio.h>
#include <string>
#include <vector>

#include "FireWorks/FFireTypes.h"
#include "FireWorks/FAssert.h"
#include "FireWorks/FDataStream.h"
#include "FireWorks/FCriticalSection.h"
#include "FireWorks/FCrc32.h"
#include "FireWorks/FTempHeap.h"
#include "FireWorks/FTimer.h"
#include "FireWorks/FILogFile.h"
#include "FireWorks/FFileDefines.h"
#include "FireWorks/FIFile.h"
#include "FireWorks/FAutoArchive.h"
#include "FireWorks/FAutoVariableBase.h"
#include "FireWorks/Win32/FDebugHelper.h"
#include "lekmod_portable/host_log.h"

// ---- host log ------------------------------------------------------------------------------------
namespace lekmod_portable {
namespace {
LogSink g_sink = 0;
void* g_sinkUser = 0;
std::set<std::string>* g_stubsLogged = 0;
}  // namespace

void SetLogSink(LogSink sink, void* user)
{
	g_sink = sink;
	g_sinkUser = user;
}

void Log(int level, const char* fmt, ...)
{
	char buf[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (g_sink)
		g_sink(g_sinkUser, level, buf);
	else if (level <= LOG_WARN)
		fprintf(stderr, "[gamecore] %s\n", buf);
}

void LogStubOnce(const char* what)
{
	if (!g_stubsLogged) g_stubsLogged = new std::set<std::string>();
	if (g_stubsLogged->insert(what).second) Log(LOG_WARN, "stub: %s", what);
}

DWORD& last_error()
{
	static DWORD e = 0;  // the gamecore runs on one thread
	return e;
}
}  // namespace lekmod_portable

// ---- FCriticalSection ----------------------------------------------------------------------------
FCriticalSection::FCriticalSection(void) { InitializeCriticalSection(&m_kCriticalSection); }
FCriticalSection::~FCriticalSection(void) { DeleteCriticalSection(&m_kCriticalSection); }
bool FCriticalSection::Try(void) { return TryEnterCriticalSection(&m_kCriticalSection) != FALSE; }
void FCriticalSection::Enter(void) { EnterCriticalSection(&m_kCriticalSection); }
void FCriticalSection::Leave(void) { LeaveCriticalSection(&m_kCriticalSection); }
int FCriticalSection::GetLockCount() const { return 0; }
int FCriticalSection::GetRecursionCount() const { return 0; }

// ---- FCRC ----------------------------------------------------------------------------------------
FCRC::FCRC(dword polynomial) : m_polynomial(polynomial), m_magic(0) { InitTable(polynomial); }
FCRC::~FCRC() {}
void FCRC::InitTable(dword polynomial)
{
	for (dword i = 0; i < 256; ++i)
	{
		dword c = i;
		for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ polynomial : (c >> 1);
		m_table[i] = c;
	}
}
dword FCRC::Calc(const void* pBuf, int nLen, dword crc) const
{
	const byte* p = static_cast<const byte*>(pBuf);
	while (nLen-- > 0) crc = (crc >> 8) ^ m_table[(crc & 0xff) ^ *p++];
	return crc;
}
dword FCRC::CalcBits(const void* pBuf, int nLen, dword crc) const { return Calc(pBuf, (nLen + 7) / 8, crc); }
dword FCRC::CalcFile(const char*) const { return CRC_INIT; }
dword FCRC::CalcFile(const wchar_t*) const { return CRC_INIT; }
FCRC g_CRC32;

// ---- TempHeap: every allocation goes to the C heap (OwnsAllocation is false, so frees match) --------
TempHeap::TempHeap() : m_pMemHead(0), m_uiCurrentOffset(0), m_uiCapacity(0), m_dwFlags(0) {}
TempHeap::~TempHeap() {}
void TempHeap::Shutdown() {}
uint TempHeap::Startup(const uint uiNumBytes, byte*)
{
	m_uiCapacity = uiNumBytes;
	return uiNumBytes;
}
byte* TempHeap::Allocate(const uint uiBytes) { return static_cast<byte*>(_aligned_malloc(uiBytes ? uiBytes : 1, 16)); }
void TempHeap::DeAllocate(const byte* pBytes) { _aligned_free(const_cast<byte*>(pBytes)); }
BOOL TempHeap::OwnsAllocation(const byte*) const { return FALSE; }
TempHeap* GetTempHeap()
{
	static TempHeap s_heap;
	return &s_heap;
}

// ---- FTimer --------------------------------------------------------------------------------------
__int64 FTimer::m_qMark = 0;
__int64 FTimer::m_qRate = 0;
double FTimer::m_fFreq = 0.0;
double FTimer::Read(__int64 offset)
{
	if (m_fFreq == 0.0)
	{
		QueryPerformanceFrequency((LARGE_INTEGER*)&m_qRate);
		QueryPerformanceCounter((LARGE_INTEGER*)&m_qMark);
		m_fFreq = m_qRate ? 1.0 / (double)m_qRate : 1.0;
	}
	__int64 now;
	QueryPerformanceCounter((LARGE_INTEGER*)&now);
	if (offset == 0) offset = m_qMark;
	return m_fFreq * (double)(now - offset);
}
void FTimer::Test() {}

// ---- aligned allocation (FMemHooks, non-tracking build) -------------------------------------------
void* FireMallocAlignedNoTracking(size_t nSize, size_t nAlignment, const char*, int) { return _aligned_malloc(nSize, nAlignment); }
void FireFreeAlignedNoTracking(void* pBlock) { _aligned_free(pBlock); }

// ---- FILogFileMgr: gamecore logs go to the host log at DEBUG level, when logging is enabled ---------
namespace {

class PortableLogFile : public FILogFile
{
public:
	explicit PortableLogFile(const std::string& name) : m_name(name), m_flags(0) {}
	virtual ~PortableLogFile() {}
	virtual void Msg(const TCHAR* format, ...)
	{
		va_list ap;
		va_start(ap, format);
		Emit(lekmod_portable::LOG_DEBUG, format, ap);
		va_end(ap);
	}
	virtual void DebugMsg(const TCHAR* format, ...)
	{
		va_list ap;
		va_start(ap, format);
		Emit(lekmod_portable::LOG_TRACE, format, ap);
		va_end(ap);
	}
	virtual void WarningMsg(const TCHAR* format, ...)
	{
		va_list ap;
		va_start(ap, format);
		Emit(lekmod_portable::LOG_WARN, format, ap);
		va_end(ap);
	}
	virtual void ErrorMsg(const TCHAR* format, ...)
	{
		va_list ap;
		va_start(ap, format);
		Emit(lekmod_portable::LOG_ERROR, format, ap);
		va_end(ap);
	}
	virtual void SetFlags(uint uiFlags) { m_flags = uiFlags; }
	virtual uint GetFlags() const { return m_flags; }
	virtual void SetBufSize(int) {}
	virtual LPCWSTR GetFileName() const { return L""; }
	virtual void Close(void) {}
	virtual void Open(void) {}
	virtual void Archive(LPCWSTR) {}

private:
	void Emit(int level, const char* format, va_list ap)
	{
		char buf[2048];
		vsnprintf(buf, sizeof(buf), format, ap);
		size_t n = strlen(buf);
		while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
		lekmod_portable::Log(level, "%s: %s", m_name.c_str(), buf);
	}
	std::string m_name;
	uint m_flags;
};

class PortableLogFileMgr : public FILogFileMgr
{
public:
	PortableLogFileMgr() : m_enabled(true) {}
	virtual ~PortableLogFileMgr()
	{
		for (size_t i = 0; i < m_logs.size(); ++i) delete m_logs[i].second;
	}
	virtual void EnableLogging(void) { m_enabled = true; }
	virtual void DisableLogging(void) { m_enabled = false; }
	virtual FILogFile* GetLog(LPCWSTR wszFileName, uint uiFlags, const TCHAR* szTitle)
	{
		std::string name;
		for (const wchar_t* p = wszFileName; p && *p; ++p) name += (char)(*p < 128 ? *p : '?');
		return GetLog(name.c_str(), uiFlags, szTitle);
	}
	virtual FILogFile* GetLog(LPCSTR szFileName, uint uiFlags, const TCHAR*)
	{
		std::string name = szFileName ? szFileName : "";
		for (size_t i = 0; i < m_logs.size(); ++i)
			if (m_logs[i].first == name) return m_logs[i].second;
		PortableLogFile* log = new PortableLogFile(name);
		log->SetFlags(uiFlags);
		m_logs.push_back(std::make_pair(name, log));
		return log;
	}
	virtual bool DeleteLog(FILogFile*) { return false; }
	virtual bool DeleteLog(LPCWSTR) { return false; }
	virtual bool IsLoggingEnabled(void) { return m_enabled; }

private:
	bool m_enabled;
	std::vector<std::pair<std::string, PortableLogFile*> > m_logs;
};

// Every operation fails: the gamecore reaches game files only through the host (gc_host.read_file).
class PortableFileSystem : public FIFileSystem
{
public:
	virtual FIFile* Create(const char*, FIFile::OpenMode, dword, uint) { return Stub(); }
	virtual FIFile* Create(const wchar*, FIFile::OpenMode, dword, uint) { return Stub(); }
	virtual FileErr Delete(const char*) { return FILE_NOTFOUND; }
	virtual FileErr Delete(const wchar*) { return FILE_NOTFOUND; }
	virtual FileErr ReadAll(const char*, byte**, size_t*, dword) { return FILE_NOTFOUND; }
	virtual FileErr ReadAll(const wchar*, byte**, size_t*, dword) { return FILE_NOTFOUND; }
	virtual FileErr ReadAll(const char*, byte**, size_t*, dword, FIFile::Allocator&) { return FILE_NOTFOUND; }
	virtual FileErr ReadAll(const wchar*, byte**, size_t*, dword, FIFile::Allocator&) { return FILE_NOTFOUND; }
	virtual bool Exist(const char*) { return false; }
	virtual bool Exist(const wchar*) { return false; }
	virtual uint GetLength(const char*) { return 0; }
	virtual uint GetLength(const wchar*) { return 0; }
	virtual FFileTimestamp GetLastWriteTime(const char*) { return FFileTimestamp(); }
	virtual bool IsFileCatalogEnabled() { return false; }
	virtual FFileErrorHandler* GetErrorHandler() { return 0; }

private:
	FIFile* Stub()
	{
		lekmod_portable::LogStubOnce("FIFileSystem::Create (no file system in the portable build)");
		return 0;
	}
};

PortableLogFileMgr g_logFileMgr;
PortableFileSystem g_fileSystem;

}  // namespace

FILogFileMgr* FILogFileMgr::ms_pInstance = &g_logFileMgr;
FILogFileMgr& FILogFileMgr::GetInstance() { return *ms_pInstance; }
FILogFileMgr* FILogFileMgr::PeekInstance() { return ms_pInstance; }

FIFileSystem* FIFileSystem::ms_pInstance = &g_fileSystem;
FIFileSystem& FIFileSystem::GetInstance() { return *ms_pInstance; }
void FIFileSystem::SetInstance(FIFileSystem* pkInstance) { ms_pInstance = pkInstance ? pkInstance : &g_fileSystem; }

// ---- FDebugHelper: no symbols or stack walking ------------------------------------------------------
FDebugHelper* FDebugHelper::pInstance = 0;
FDebugHelper::FDebugHelper() : hCurrentProcess(0), m_pExceptionTextCB(0) {}
FDebugHelper::~FDebugHelper() {}
FDebugHelper& FDebugHelper::GetInstance()
{
	if (!pInstance) pInstance = new FDebugHelper();
	return *pInstance;
}
void FDebugHelper::DestroyDebugHelper()
{
	delete pInstance;
	pInstance = 0;
}
bool FDebugHelper::LoadSymbols() { return false; }
bool FDebugHelper::LoadSymbols(HMODULE) { return false; }
void FDebugHelper::UnloadSymbols(HMODULE) {}
bool FDebugHelper::GetSymbolName(DWORD64, TCHAR* name, unsigned int maxNameLength)
{
	if (maxNameLength) name[0] = 0;
	return false;
}
bool FDebugHelper::GetSymbolName(const CONTEXT&, TCHAR* name, unsigned int maxNameLength) { return GetSymbolName(0, name, maxNameLength); }
bool FDebugHelper::GetSymbolFileLine(DWORD64, TCHAR* name, DWORD* pLineNumber, unsigned int maxNameLength)
{
	if (maxNameLength) name[0] = 0;
	if (pLineNumber) *pLineNumber = 0;
	return false;
}
bool FDebugHelper::GetSymbolFileLine(const CONTEXT&, TCHAR* name, DWORD* pLineNumber, unsigned int maxNameLength)
{
	return GetSymbolFileLine(0, name, pLineNumber, maxNameLength);
}
bool FDebugHelper::GetModuleName(DWORD64, TCHAR* name, unsigned int maxNameLength)
{
	if (maxNameLength) name[0] = 0;
	return false;
}
bool FDebugHelper::GetCallStack(FCallStack*, int, int) { return false; }
void FDebugHelper::WaitForDebuggerAttachment(bool) {}

// ---- FAutoArchive / FAutoVariableBase ------------------------------------------------------------
bool g_bFAutoArchiveCallStacks = false;

FAutoArchive::FAutoArchive() {}
FAutoArchive::~FAutoArchive() {}
void FAutoArchive::add(FAutoVariableBase& variable) { m_contents.push_back(&variable); }
void FAutoArchive::remove(FAutoVariableBase& variable)
{
	std::vector<FAutoVariableBase*>::iterator found = std::find(m_contents.begin(), m_contents.end(), &variable);
	if (found != m_contents.end()) m_contents.erase(found);
	m_deltas.erase(&variable);
}
void FAutoArchive::load(FDataStream& loadFrom)
{
	for (size_t i = 0; i < m_contents.size(); ++i) m_contents[i]->load(loadFrom);
}
void FAutoArchive::save(FDataStream& saveTo) const
{
	for (size_t i = 0; i < m_contents.size(); ++i) m_contents[i]->save(saveTo);
}
// Delta: UINT32 count, then (UINT32 index into m_contents, variable delta) per touched variable. The set is
// ordered by address, as in the original; indices make the stream independent of addresses.
void FAutoArchive::saveDelta(FDataStream& saveTo, std::vector<std::pair<std::string, std::string> >&) const
{
	std::vector<std::pair<UINT32, FAutoVariableBase*> > entries;
	for (std::set<FAutoVariableBase*>::const_iterator it = m_deltas.begin(); it != m_deltas.end(); ++it)
	{
		std::vector<FAutoVariableBase*>::const_iterator found = std::find(m_contents.begin(), m_contents.end(), *it);
		if (found != m_contents.end()) entries.push_back(std::make_pair((UINT32)(found - m_contents.begin()), *it));
	}
	UINT32 count = (UINT32)entries.size();
	saveTo << count;
	for (size_t i = 0; i < entries.size(); ++i)
	{
		saveTo << entries[i].first;
		entries[i].second->saveDelta(saveTo);
	}
	m_deltas.clear();
}
void FAutoArchive::loadDelta(FDataStream& loadFrom)
{
	UINT32 count = 0;
	loadFrom >> count;
	for (UINT32 i = 0; i < count; ++i)
	{
		UINT32 index = 0;
		loadFrom >> index;
		if (index >= m_contents.size()) return;  // corrupt delta
		m_contents[index]->loadDelta(loadFrom);
	}
}
void FAutoArchive::clearDelta()
{
	for (std::set<FAutoVariableBase*>::const_iterator it = m_deltas.begin(); it != m_deltas.end(); ++it) (*it)->clearDelta();
	m_deltas.clear();
}
void FAutoArchive::reset()
{
	for (size_t i = 0; i < m_contents.size(); ++i) m_contents[i]->reset();
}
bool FAutoArchive::hasDeltas() const { return !m_deltas.empty(); }
std::vector<const FAutoVariableBase*> FAutoArchive::findMismatchedVariables(FDataStream& otherValues) const
{
	std::vector<const FAutoVariableBase*> result;
	for (size_t i = 0; i < m_contents.size(); ++i)
		if (!m_contents[i]->compare(otherValues)) result.push_back(m_contents[i]);
	return result;
}
const FAutoVariableBase* FAutoArchive::findVariable(const std::string& name) const
{
	for (size_t i = 0; i < m_contents.size(); ++i)
		if (m_contents[i]->name() == name) return m_contents[i];
	return 0;
}
void FAutoArchive::EnableCallStacks(bool bState) { g_bFAutoArchiveCallStacks = bState; }
bool FAutoArchive::AreCallStacksEnabled() { return g_bFAutoArchiveCallStacks; }

FDataStream& operator<<(FDataStream& saveTo, const FAutoArchive& readFrom)
{
	readFrom.save(saveTo);
	return saveTo;
}
FDataStream& operator>>(FDataStream& loadFrom, FAutoArchive& writeTo)
{
	writeTo.load(loadFrom);
	return loadFrom;
}

FAutoVariableBase::FAutoVariableBase(const std::string& name, FAutoArchive& owner)
{
	owner.add(*this);
	owner.setVariableName(*this, name);
}
FAutoVariableBase::FAutoVariableBase(const std::string& name, FAutoArchive& owner, bool)
{
	owner.add(*this);
	owner.setVariableName(*this, name);
}
FAutoVariableBase::~FAutoVariableBase() {}
std::string FAutoVariableBase::getStackTrace() const { return "Stack Trace unavailable"; }
std::string FAutoVariableBase::getStackTraceRemark() const { return std::string(); }
std::string FAutoVariableBase::debugDump(const std::vector<std::pair<std::string, std::string> >&) const
{
	return std::string("Variable Out Of Sync : ") + name();
}

namespace FSerialization {
namespace {
std::string fmt(const char* f, ...)
{
	char buf[64];
	va_list ap;
	va_start(ap, f);
	vsnprintf(buf, sizeof(buf), f, ap);
	va_end(ap);
	return buf;
}
}  // namespace
std::string toString(const bool& source) { return source ? "true" : "false"; }
std::string toString(const unsigned char& source) { return fmt("%u", (unsigned)source); }
std::string toString(const char& source) { return fmt("%d", (int)source); }
std::string toString(const unsigned short& source) { return fmt("%u", (unsigned)source); }
std::string toString(const short& source) { return fmt("%d", (int)source); }
std::string toString(const unsigned int& source) { return fmt("%u", source); }
std::string toString(const int& source) { return fmt("%d", source); }
std::string toString(const float& source) { return fmt("%f", (double)source); }
std::string toString(const double& source) { return fmt("%f", source); }
std::string toString(const std::string& source) { return source; }
std::string toString(const FAutoVariableBase& source) { return source.toString(); }
}  // namespace FSerialization
