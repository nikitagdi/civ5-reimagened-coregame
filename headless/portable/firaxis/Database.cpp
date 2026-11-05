// CvGameDatabase (Database::Connection, Results, SingleResult, ResultsCache, BinaryIO) over the sqlite3 C API,
// rewritten for the portable build. SQL text and behavior follow the original library.
// The FireWorks headers expect the gamecore precompiled-header environment.
#include "CvGameCoreDLLPCH.h"

#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "CvGameDatabase.h"
#include "lekmod_portable/host_log.h"
#include "../runtime/rt_db.h"

using namespace Database;

namespace {

int busy_handler(void*, int count)
{
	if (count >= 20) return 0;
	Sleep(10);
	return 1;
}

}  // namespace

bool Connection::ms_bMemoryManagerInitialized = false;

void Connection::InitMemoryManagement()
{
	// The original carved static page-cache buffers out for sqlite; the default allocator is used here.
	ms_bMemoryManagerInitialized = true;
}

void* Connection::operator new(size_t tSize) { return malloc(tSize); }
void Connection::operator delete(void* pMem) { free(pMem); }

Connection::Connection() : m_db(0)
{
	if (!ms_bMemoryManagerInitialized)
	{
		InitMemoryManagement();
		sqlite3_initialize();
	}
}

Connection::Connection(const char* filename, int flags) : m_db(0)
{
	if (!ms_bMemoryManagerInitialized)
	{
		InitMemoryManagement();
		sqlite3_initialize();
	}
	Open(filename, flags);
}

Connection::~Connection()
{
	if (m_db) Close();
}

bool Connection::Open(const char* filename, int flags)
{
	if (m_db) Close();
	if (flags == 0) flags = OPEN_READWRITE | OPEN_CREATE;
	const char* name = filename ? filename : ":memory:";
	int status = sqlite3_open_v2(name, &m_db, flags, 0);
	if (Report(status))
	{
		sqlite3_busy_handler(m_db, busy_handler, this);
		return true;
	}
	if (m_db)
	{
		sqlite3_close(m_db);
		m_db = 0;
	}
	return false;
}

void Connection::Close()
{
	ClearCountCache();
	if (m_db)
	{
		sqlite3_stmt* stmt;
		while ((stmt = sqlite3_next_stmt(m_db, 0)) != 0) sqlite3_finalize(stmt);  // statements leaked by callers
		sqlite3_close(m_db);
		m_db = 0;
	}
}

void Connection::BeginTransaction() { Execute("SAVEPOINT _;"); }
void Connection::EndTransaction() { Execute("RELEASE _;"); }
void Connection::RollbackTransaction() { Execute("ROLLBACK TO _;"); }
void Connection::BeginDeferredTransaction() { Execute("BEGIN DEFERRED TRANSACTION;"); }
void Connection::BeginImmediateTransaction() { Execute("BEGIN IMMEDIATE TRANSACTION;"); }
void Connection::BeginExclusiveTransaction() { Execute("BEGIN EXCLUSIVE TRANSACTION;"); }
void Connection::CommitTransaction() { Execute("COMMIT TRANSACTION;"); }

bool Connection::SetSavePoint(const char* savePoint)
{
	char* sql = sqlite3_mprintf("SAVEPOINT %Q;", savePoint);
	bool ok = Execute(sql);
	sqlite3_free(sql);
	return ok;
}

bool Connection::RollbackToSavePoint(const char* savePoint)
{
	char* sql = sqlite3_mprintf("ROLLBACK TO %Q;", savePoint);
	bool ok = Execute(sql);
	sqlite3_free(sql);
	return ok;
}

int Connection::Count(const char* tableName, bool bCache)
{
	std::string table(tableName);
	std::tr1::unordered_map<std::string, int>::iterator cached = m_hshCountValues.find(table);
	if (cached != m_hshCountValues.end() && sqlite3_changes(m_db) == 0) return cached->second;

	sqlite3_stmt* stmt = 0;
	bool wasCached = false;
	std::tr1::unordered_map<std::string, sqlite3_stmt*>::iterator it = m_hshCountStatements.find(table);
	if (it == m_hshCountStatements.end())
	{
		char* sql = sqlite3_mprintf("select count(*) from %s", tableName);
		int status = sqlite3_prepare_v2(m_db, sql, -1, &stmt, 0);
		sqlite3_free(sql);
		if (!Report(status)) return -1;
		if (bCache) m_hshCountStatements[table] = stmt;
	}
	else
	{
		stmt = it->second;
		Report(sqlite3_reset(stmt));
		wasCached = true;
	}

	int count = -1;
	if (sqlite3_step(stmt) == SQLITE_ROW)
	{
		count = sqlite3_column_int(stmt, 0);
		m_hshCountValues[table] = count;
	}
	if (wasCached || bCache)
	{
		sqlite3_reset(stmt);
		sqlite3_clear_bindings(stmt);
	}
	else
	{
		sqlite3_finalize(stmt);
	}
	return count;
}

void Connection::ClearCountCache()
{
	for (std::tr1::unordered_map<std::string, sqlite3_stmt*>::iterator it = m_hshCountStatements.begin(); it != m_hshCountStatements.end(); ++it)
		sqlite3_finalize(it->second);
	m_hshCountStatements.clear();
	m_hshCountValues.clear();
}

namespace {
// A SingleResult comes back positioned on its row: the gamecore reads it right after Select*/Execute without
// Step() (CvGameSpeedInfo::CacheResults count(*), CvPreGame setCalendar, CvGameTextMgr Months), and the call
// is false when there is no row.
bool position_single(Results& results, bool ok) { return ok && (!results.SingleQuery() || results.Step()); }

bool select_into(const Connection* conn, Results& results, char* sql)
{
	Command command(conn, 0);
	bool ok = false;
	if (Connection::AcquireCommand(command, sql)) ok = results.SetCommand(command);
	else Connection::ReleaseCommand(command);
	sqlite3_free(sql);
	return position_single(results, ok);
}
}  // namespace

bool Connection::SelectAt(Results& kResults, const char* tableName, int rowID)
{
	return select_into(this, kResults, sqlite3_mprintf("select %s from %q where ROWID = %i LIMIT 1", kResults.GetColumns(), tableName, rowID));
}

bool Connection::SelectAt(Results& kResults, const char* tableName, const char* columnName, const char* value)
{
	return select_into(this, kResults,
		sqlite3_mprintf("select %s from %q where %s = %Q %s", kResults.GetColumns(), tableName, columnName, value, kResults.SingleQuery() ? "LIMIT 1" : ""));
}

bool Connection::SelectAt(Results& kResults, const char* tableName, const char* columnName, int value)
{
	return select_into(this, kResults,
		sqlite3_mprintf("select %s from %q where %s = %i %s", kResults.GetColumns(), tableName, columnName, value, kResults.SingleQuery() ? "LIMIT 1" : ""));
}

bool Connection::SelectAt(Results& kResults, const char* tableName, const char* columnName, double value)
{
	return select_into(this, kResults,
		sqlite3_mprintf("select %s from %q where %s = %f %s", kResults.GetColumns(), tableName, columnName, value, kResults.SingleQuery() ? "LIMIT 1" : ""));
}

bool Connection::SelectAll(Results& kResults, const char* tableName)
{
	return select_into(this, kResults, sqlite3_mprintf("select %s from %q %s", kResults.GetColumns(), tableName, kResults.SingleQuery() ? "LIMIT 1" : ""));
}

bool Connection::SelectWhere(Results& kResults, const char* tableName, const char* condition)
{
	return select_into(this, kResults,
		sqlite3_mprintf("select %s from %q where %s %s", kResults.GetColumns(), tableName, condition, kResults.SingleQuery() ? "LIMIT 1" : ""));
}

bool Connection::Execute(Results& kResults, const char* szCommand, int lenCommand) const
{
	Command command(this, 0);
	if (AcquireCommand(command, szCommand, lenCommand)) return position_single(kResults, kResults.SetCommand(command));
	return false;
}

bool Connection::Execute(const char* szCommand, int lenCommand) const
{
	sqlite3_stmt* stmt = 0;
	if (!Report(sqlite3_prepare_v2(m_db, szCommand, lenCommand, &stmt, 0)) || !stmt) return false;
	int rc;
	do
	{
		rc = sqlite3_step(stmt);
	} while (rc == SQLITE_ROW);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
	{
		LogError(ErrorMessage());
		return false;
	}
	return true;
}

bool Connection::ExecuteMultiple(const char* szCommands, int lenCommands) const
{
	if (!szCommands) return false;
	std::string sql = lenCommands < 0 ? std::string(szCommands) : std::string(szCommands, (size_t)lenCommands);
	char* err = 0;
	int rc = sqlite3_exec(m_db, sql.c_str(), 0, 0, &err);
	if (rc != SQLITE_OK)
	{
		LogError(err ? err : ErrorMessage());
		sqlite3_free(err);
		return false;
	}
	return true;
}

int Connection::TotalChanges() const { return m_db ? sqlite3_total_changes(m_db) : 0; }
void Connection::Analyze() const { Execute("ANALYZE;"); }
void Connection::Vacuum() const { Execute("VACUUM;"); }

int Connection::StatementCount() const
{
	int n = 0;
	for (sqlite3_stmt* s = m_db ? sqlite3_next_stmt(m_db, 0) : 0; s; s = sqlite3_next_stmt(m_db, s)) ++n;
	return n;
}

bool Connection::SetBusyTimeout(int ms) { return m_db && sqlite3_busy_timeout(m_db, ms) == SQLITE_OK; }

const char* Connection::StatementSQL(int index) const
{
	int n = 0;
	for (sqlite3_stmt* s = m_db ? sqlite3_next_stmt(m_db, 0) : 0; s; s = sqlite3_next_stmt(m_db, s), ++n)
		if (n == index) return sqlite3_sql(s);
	return 0;
}

bool Connection::Report(int status) const
{
	if (status == SQLITE_OK || status == SQLITE_ROW || status == SQLITE_DONE) return true;
	LogError(m_db ? sqlite3_errmsg(m_db) : sqlite3_errstr(status));
	return false;
}

void Connection::SetLogger(IDatabaseLogger* pkLogger) { m_pkDatabaseLogger.reset(pkLogger); }

void Connection::LogMessage(const char* szMessage) const
{
	if (m_pkDatabaseLogger.get()) m_pkDatabaseLogger->Log(szMessage, IDatabaseLogger::TYPE_MESSAGE);
	else lekmod_portable::Log(lekmod_portable::LOG_DEBUG, "database: %s", szMessage ? szMessage : "");
}

void Connection::LogWarning(const char* szWarning) const
{
	if (m_pkDatabaseLogger.get()) m_pkDatabaseLogger->Log(szWarning, IDatabaseLogger::TYPE_WARNING);
	else lekmod_portable::Log(lekmod_portable::LOG_WARN, "database: %s", szWarning ? szWarning : "");
}

void Connection::LogError(const char* szError) const
{
	if (m_pkDatabaseLogger.get()) m_pkDatabaseLogger->Log(szError, IDatabaseLogger::TYPE_ERROR);
	else lekmod_portable::Log(lekmod_portable::LOG_ERROR, "database: %s", szError ? szError : "");
}

const char* Connection::CalculateMemoryStats()
{
	std::stringstream ss;
	ss << "-- SQLite Memory Statistics --\n";
	ss << "Malloc:\t" << sqlite3_memory_used() << "\t" << sqlite3_memory_highwater(0) << "\n";
	ss << "Prepared Statements:\t" << StatementCount() << "\n";
	m_strMemoryStats = ss.str();
	LogMessage(m_strMemoryStats.c_str());
	return m_strMemoryStats.c_str();
}

bool Connection::ValidateFKConstraints(bool) const
{
	LogMessage("Validating Foreign Key Constraints...");
	sqlite3_stmt* tables = 0;
	if (sqlite3_prepare_v2(m_db, "select name from sqlite_master where type = 'table'", -1, &tables, 0) != SQLITE_OK) return false;
	bool valid = true;
	while (sqlite3_step(tables) == SQLITE_ROW)
	{
		std::string table = (const char*)sqlite3_column_text(tables, 0);
		char* fkSql = sqlite3_mprintf("pragma foreign_key_list(%s);", table.c_str());
		sqlite3_stmt* keys = 0;
		int rc = sqlite3_prepare_v2(m_db, fkSql, -1, &keys, 0);
		sqlite3_free(fkSql);
		if (rc != SQLITE_OK)
		{
			valid = false;
			break;
		}
		while (sqlite3_step(keys) == SQLITE_ROW)
		{
			std::string refTable = (const char*)sqlite3_column_text(keys, 2);
			std::string fromCol = (const char*)sqlite3_column_text(keys, 3);
			std::string toCol = sqlite3_column_text(keys, 4) ? (const char*)sqlite3_column_text(keys, 4) : fromCol;
			char* check = sqlite3_mprintf("select \"%s\" from %s where %s not null and %s not in (select \"%s\" from %s);", fromCol.c_str(), table.c_str(),
				fromCol.c_str(), fromCol.c_str(), toCol.c_str(), refTable.c_str());
			sqlite3_stmt* st = 0;
			if (sqlite3_prepare_v2(m_db, check, -1, &st, 0) == SQLITE_OK)
			{
				while (sqlite3_step(st) == SQLITE_ROW)
				{
					char* msg = sqlite3_mprintf("Invalid Reference on %s.%s - \"%s\" does not exist in %s", table.c_str(), fromCol.c_str(),
						(const char*)sqlite3_column_text(st, 0), refTable.c_str());
					LogError(msg);
					sqlite3_free(msg);
					valid = false;
				}
			}
			sqlite3_finalize(st);
			sqlite3_free(check);
		}
		sqlite3_finalize(keys);
	}
	sqlite3_finalize(tables);
	LogMessage(valid ? "Passed Validation." : "Failed Validation.");
	return valid;
}

sqlite3* Connection::GetSQLite3() const { return m_db; }
int Connection::ErrorCode() const { return m_db ? sqlite3_errcode(m_db) : SQLITE_MISUSE; }
const char* Connection::ErrorMessage() const { return m_db ? sqlite3_errmsg(m_db) : 0; }

// No statement cache: every command is prepared on acquire and finalized on release.
bool Connection::AcquireCommand(Command& command, const char* szSQL, int lenSQL)
{
	command.stmt = 0;
	if (!command.connection || !command.connection->m_db) return false;
	int rc = sqlite3_prepare_v2(command.connection->m_db, szSQL, lenSQL, &command.stmt, 0);
	if (rc != SQLITE_OK || !command.stmt)
	{
		command.connection->Report(rc == SQLITE_OK ? SQLITE_ERROR : rc);
		if (command.stmt) sqlite3_finalize(command.stmt);
		command.stmt = 0;
		return false;
	}
	return true;
}

void Connection::ReleaseCommand(Command& command)
{
	if (command.stmt) sqlite3_finalize(command.stmt);
	command.stmt = 0;
}

// ---- Results -------------------------------------------------------------------------------------
void* Results::operator new(size_t tSize) { return malloc(tSize); }
void Results::operator delete(void* pMem) { free(pMem); }

Results::Results(const char* szColumns) : m_Command(), m_szColumns(NULL), m_bSingleQuery(false) { SetColumns(szColumns); }

Results::Results(const Results& copy) : m_Command(), m_szColumns(NULL), m_bSingleQuery(copy.m_bSingleQuery)
{
	SetColumns(copy.m_szColumns);  // the statement is not shared
}

Results::~Results() { Release(); }

const char* Results::GetColumns() const { return m_szColumns ? m_szColumns : "*"; }

void Results::SetColumns(const char* szColumns)
{
	if (m_szColumns) sqlite3_free(const_cast<char*>(m_szColumns));
	m_szColumns = szColumns ? sqlite3_mprintf("%s", szColumns) : NULL;
}

bool Results::SetCommand(Command& command)
{
	if (m_Command.stmt) Connection::ReleaseCommand(m_Command);
	m_Command = command;
	m_hshColumnPositions.clear();
	return m_Command.stmt != NULL;
}

bool Results::Execute()
{
	if (!m_Command.connection || !m_Command.stmt) return false;
	int rc = sqlite3_step(m_Command.stmt);
	if (rc == SQLITE_DONE) return true;
	if (rc == SQLITE_ROW) m_Command.connection->LogWarning("A row was returned during an Execute command.");
	else m_Command.connection->LogError(m_Command.connection->ErrorMessage());
	char* msg = sqlite3_mprintf("While executing - '%s'", sqlite3_sql(m_Command.stmt));
	m_Command.connection->LogMessage(msg);
	sqlite3_free(msg);
	return false;
}

bool Results::TryExecute()
{
	if (!m_Command.stmt) return false;
	int rc = sqlite3_step(m_Command.stmt);
	return rc == SQLITE_DONE || rc == SQLITE_CONSTRAINT;
}

bool Results::Step()
{
	if (!m_Command.stmt) return false;
	int rc = sqlite3_step(m_Command.stmt);
	if (rc != SQLITE_ROW && rc != SQLITE_DONE && m_Command.connection)
	{
		m_Command.connection->LogError(m_Command.connection->ErrorMessage());
		char* msg = sqlite3_mprintf("While executing - '%s'", sqlite3_sql(m_Command.stmt));
		m_Command.connection->LogMessage(msg);
		sqlite3_free(msg);
	}
	return rc == SQLITE_ROW;
}

bool Results::Reset()
{
	if (!m_Command.stmt || sqlite3_reset(m_Command.stmt) != SQLITE_OK) return false;
	return sqlite3_clear_bindings(m_Command.stmt) == SQLITE_OK;
}

bool Results::TryReset()
{
	if (!m_Command.stmt) return false;
	sqlite3_reset(m_Command.stmt);
	return sqlite3_clear_bindings(m_Command.stmt) == SQLITE_OK;
}

void Results::Release()
{
	if (m_Command.stmt) Connection::ReleaseCommand(m_Command);
	if (m_szColumns) sqlite3_free(const_cast<char*>(m_szColumns));
	m_szColumns = NULL;
	m_hshColumnPositions.clear();
}

bool Results::Bind(int idx, const char* szValue, int lenValue, bool bMakeCopy)
{
	return sqlite3_bind_text(m_Command.stmt, idx, szValue, lenValue, bMakeCopy ? SQLITE_TRANSIENT : SQLITE_STATIC) == SQLITE_OK;
}
bool Results::Bind(int idx, const char* szValue, bool bMakeCopy) { return Bind(idx, szValue, -1, bMakeCopy); }

// wchar_t is UTF-32 outside Win32: bind as UTF-8.
bool Results::Bind(int idx, const wchar_t* wszValue, int lenValue, bool)
{
	if (!wszValue) return BindNULL(idx);
	size_t n = lenValue < 0 ? wcslen(wszValue) : (size_t)lenValue;
	std::string utf8;
	int bytes = WideCharToMultiByte(CP_UTF8, 0, wszValue, (int)n, NULL, 0, NULL, NULL);
	utf8.resize((size_t)bytes);
	if (bytes) WideCharToMultiByte(CP_UTF8, 0, wszValue, (int)n, &utf8[0], bytes, NULL, NULL);
	return sqlite3_bind_text(m_Command.stmt, idx, utf8.data(), (int)utf8.size(), SQLITE_TRANSIENT) == SQLITE_OK;
}
bool Results::Bind(int idx, const wchar_t* wszValue, bool bMakeCopy) { return Bind(idx, wszValue, -1, bMakeCopy); }
bool Results::Bind(int idx, double dValue) { return sqlite3_bind_double(m_Command.stmt, idx, dValue) == SQLITE_OK; }
bool Results::Bind(int idx, float fValue) { return sqlite3_bind_double(m_Command.stmt, idx, (double)fValue) == SQLITE_OK; }
bool Results::Bind(int idx, int iValue) { return sqlite3_bind_int(m_Command.stmt, idx, iValue) == SQLITE_OK; }
bool Results::Bind(int idx, __int64 iValue) { return sqlite3_bind_int64(m_Command.stmt, idx, iValue) == SQLITE_OK; }
bool Results::BindNULL(int idx) { return sqlite3_bind_null(m_Command.stmt, idx) == SQLITE_OK; }

int Results::ColumnCount() const { return m_Command.stmt ? sqlite3_column_count(m_Command.stmt) : 0; }
const char* Results::ColumnName(int iColumn) { return m_Command.stmt ? sqlite3_column_name(m_Command.stmt, iColumn) : NULL; }
const char* Results::ColumnTypeName(int iColumn) { return m_Command.stmt ? sqlite3_column_decltype(m_Command.stmt, iColumn) : NULL; }

ColumnTypes Results::ColumnType(int iColumn)
{
	if (!m_Command.stmt) return COLTYPE_NULL;
	const char* decl = sqlite3_column_decltype(m_Command.stmt, iColumn);
	if (decl && _stricmp(decl, "boolean") == 0) return COLTYPE_BOOL;
	return (ColumnTypes)sqlite3_column_type(m_Command.stmt, iColumn);
}

void Results::HashColumnPositions()
{
	m_hshColumnPositions.clear();
	int n = ColumnCount();
	for (int i = 0; i < n; ++i)
	{
		const char* name = sqlite3_column_name(m_Command.stmt, i);
		if (name) m_hshColumnPositions[name] = i;
	}
}

bool Results::HasColumn(const char* szColumnName) { return ColumnPosition(szColumnName) != -1; }

int Results::ColumnPosition(const char* szColumnName)
{
	if (m_hshColumnPositions.empty()) HashColumnPositions();
	stdext::hash_map<std::string, int>::iterator it = m_hshColumnPositions.find(szColumnName);
	return it == m_hshColumnPositions.end() ? -1 : it->second;
}

const char* Results::GetText(int iColumn) { return m_Command.stmt ? (const char*)sqlite3_column_text(m_Command.stmt, iColumn) : NULL; }

const wchar_t* Results::GetText16(int iColumn)
{
	static std::wstring s_buffer;  // valid until the next GetText16 call (single gamecore thread)
	const char* text = GetText(iColumn);
	if (!text) return NULL;
	int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
	s_buffer.assign((size_t)(n > 0 ? n : 1), L'\0');
	if (n > 0) MultiByteToWideChar(CP_UTF8, 0, text, -1, &s_buffer[0], n);
	return s_buffer.c_str();
}

bool Results::GetBool(int iColumn)
{
	if (!m_Command.stmt) return false;
	ColumnTypes type = ColumnType(iColumn);
	if (type == COLTYPE_TEXT)
	{
		const char* text = (const char*)sqlite3_column_text(m_Command.stmt, iColumn);
		return text && (*text == 'T' || *text == 't');
	}
	if (type == COLTYPE_INT || type == COLTYPE_BOOL) return sqlite3_column_int(m_Command.stmt, iColumn) != 0;
	return false;
}

int Results::GetInt(int iColumn) { return m_Command.stmt ? sqlite3_column_int(m_Command.stmt, iColumn) : 0; }
__int64 Results::GetInt64(int iColumn) { return m_Command.stmt ? sqlite3_column_int64(m_Command.stmt, iColumn) : 0; }
float Results::GetFloat(int iColumn) { return m_Command.stmt ? (float)sqlite3_column_double(m_Command.stmt, iColumn) : 0.0f; }
double Results::GetDouble(int iColumn) { return m_Command.stmt ? sqlite3_column_double(m_Command.stmt, iColumn) : 0.0; }

void Results::GetValue(int iColumn, bool& v) { v = GetBool(iColumn); }
void Results::GetValue(int iColumn, int& v) { v = GetInt(iColumn); }
void Results::GetValue(int iColumn, __int64& v) { v = GetInt64(iColumn); }
void Results::GetValue(int iColumn, float& v) { v = GetFloat(iColumn); }
void Results::GetValue(int iColumn, double& v) { v = GetDouble(iColumn); }
void Results::GetValue(int iColumn, const char*& v) { v = GetText(iColumn); }
void Results::GetValue(int iColumn, const wchar_t*& v) { v = GetText16(iColumn); }

const char* Results::GetText(const char* c) { return GetText(ColumnPosition(c)); }
const wchar_t* Results::GetText16(const char* c) { return GetText16(ColumnPosition(c)); }
bool Results::GetBool(const char* c) { return GetBool(ColumnPosition(c)); }
int Results::GetInt(const char* c) { return GetInt(ColumnPosition(c)); }
__int64 Results::GetInt64(const char* c) { return GetInt64(ColumnPosition(c)); }
float Results::GetFloat(const char* c) { return GetFloat(ColumnPosition(c)); }
double Results::GetDouble(const char* c) { return GetDouble(ColumnPosition(c)); }
void Results::GetValue(const char* c, bool& v) { v = GetBool(c); }
void Results::GetValue(const char* c, int& v) { v = GetInt(c); }
void Results::GetValue(const char* c, __int64& v) { v = GetInt64(c); }
void Results::GetValue(const char* c, float& v) { v = GetFloat(c); }
void Results::GetValue(const char* c, double& v) { v = GetDouble(c); }
void Results::GetValue(const char* c, const char*& v) { v = GetText(c); }
void Results::GetValue(const char* c, const wchar_t*& v) { v = GetText16(c); }

bool Results::SingleQuery() const { return m_bSingleQuery; }
void Results::SingleQuery(bool bSingle) { m_bSingleQuery = bSingle; }

SingleResult::SingleResult(const char* szColumns) : Results(szColumns) { SingleQuery(true); }

// ---- ResultsCache --------------------------------------------------------------------------------
ResultsCache::ResultsCache(Database::Connection& db) : m_db(db) {}
ResultsCache::~ResultsCache() { Clear(); }

void ResultsCache::Clear()
{
	for (ResultsMap::iterator it = m_storedResults.begin(); it != m_storedResults.end(); ++it) delete it->second;
	m_storedResults.clear();
}

void ResultsCache::Clear(const std::string& strKey)
{
	ResultsMap::iterator it = m_storedResults.find(strKey);
	if (it != m_storedResults.end())
	{
		delete it->second;
		m_storedResults.erase(it);
	}
}

Database::Connection& ResultsCache::Connection() { return m_db; }

Results* ResultsCache::Get(const std::string& strKey)
{
	ResultsMap::iterator it = m_storedResults.find(strKey);
	return it == m_storedResults.end() ? NULL : it->second;
}

Results* ResultsCache::Prepare(const std::string& strKey, const char* szStmt, int lenStmt)
{
	Clear(strKey);
	Results* results = new Results();
	if (!m_db.Execute(*results, szStmt, lenStmt))
	{
		delete results;
		return NULL;
	}
	m_storedResults[strKey] = results;
	return results;
}

const char* ResultsCache::ErrorMessage() const { return m_db.ErrorMessage(); }

// ---- BinaryIO: whole-database copy to/from a file through the sqlite backup API ---------------------
BinaryIO::BinaryIO(const char* szFilename) : m_pszFilename(szFilename) {}
bool BinaryIO::operator<<(Database::Connection& db) { return Load(db); }
bool BinaryIO::operator>>(Database::Connection& db) { return Save(db); }
bool BinaryIO::Save(Database::Connection& db) { return SaveLoad(m_pszFilename, db, true); }

// Load: a name registered with rt_db::RegisterImage is a memory image (sqlite3_deserialize into "main"); any
// other name is a file copied in through the backup API. Save always writes a file.
bool BinaryIO::Load(Database::Connection& db)
{
	const void* image = 0;
	size_t size = 0;
	if (lekmod_portable::rt_db::FindImage(m_pszFilename, &image, &size))
		return lekmod_portable::rt_db::Deserialize(db.GetSQLite3(), "main", image, size, false);
	return SaveLoad(m_pszFilename, db, false);
}

bool BinaryIO::SaveLoad(const char* szFilename, Database::Connection& db, bool bSave)
{
	sqlite3* file = 0;
	if (!szFilename || sqlite3_open(szFilename, &file) != SQLITE_OK)
	{
		if (file) sqlite3_close(file);
		return false;
	}
	sqlite3* to = bSave ? file : db.GetSQLite3();
	sqlite3* from = bSave ? db.GetSQLite3() : file;
	sqlite3_backup* backup = sqlite3_backup_init(to, "main", from, "main");
	bool ok = false;
	if (backup)
	{
		sqlite3_backup_step(backup, -1);
		ok = sqlite3_backup_finish(backup) == SQLITE_OK;
	}
	sqlite3_close(file);
	return ok;
}
