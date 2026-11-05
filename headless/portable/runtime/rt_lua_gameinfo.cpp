// GameInfo and DB.Query over sqlite (see rt_lua_gameinfo.h for the behaviour and where it was recorded).
#include "CvGameCoreDLLPCH.h"

#include <limits.h>
#include <math.h>
#include <string.h>

#include <string>
#include <vector>

#include <sqlite3.h>

#include "rt_lua_gameinfo.h"
#include "lekmod_portable/host_log.h"

namespace lekmod_portable {

namespace {

const char* const kRowMeta = "RtGameInfoRow";      // GameInfo rows: read-only
const char* const kDbRowMeta = "RtDbQueryRow";     // DB.Query rows
const char* const kQueryMeta = "RtGameInfoQuery";  // statement userdata (GameInfo iterators and DB.Query)
const char* const kIterateError = "Failed to iterate table due to query error.  Check Database.log for more details.";

sqlite3* Db(lua_State* L, int idx)
{
	return (sqlite3*)lua_touserdata(L, idx);
}

// lua_tointeger of the original's Lua build (MSVC x86: fistp, round to nearest, ties to even). Values outside int give
// INT_MIN like fistp's "integer indefinite" (and do not trap on wasm).
int RoundToInt(lua_Number d)
{
	const double r = nearbyint((double)d);
	if (!(r >= -2147483648.0 && r <= 2147483647.0)) return INT_MIN;
	return (int)r;
}

// ---- statements ---------------------------------------------------------------------------------------------------
struct Query
{
	sqlite3_stmt* stmt;
	bool failed;  // prepare failed: the first iterator call raises kIterateError
	bool done;
};

int lQueryGc(lua_State* L)
{
	Query* q = (Query*)lua_touserdata(L, 1);
	if (q && q->stmt)
	{
		sqlite3_finalize(q->stmt);
		q->stmt = NULL;
	}
	return 0;
}

Query* NewQuery(lua_State* L)
{
	Query* q = (Query*)lua_newuserdata(L, sizeof(Query));
	q->stmt = NULL;
	q->failed = false;
	q->done = false;
	luaL_getmetatable(L, kQueryMeta);
	lua_setmetatable(L, -2);
	return q;
}

void Finish(Query* q)
{
	if (q->stmt) sqlite3_finalize(q->stmt);
	q->stmt = NULL;
	q->done = true;
}

// One result row as a new table: non-NULL columns from `first` on, booleans for INTEGER values of boolean columns.
void PushRow(lua_State* L, sqlite3_stmt* stmt, int first, const char* meta)
{
	const int n = sqlite3_column_count(stmt);
	lua_createtable(L, 0, n > first ? n - first : 0);
	for (int i = first; i < n; ++i)
	{
		const char* name = sqlite3_column_name(stmt, i);
		if (!name) continue;
		switch (sqlite3_column_type(stmt, i))
		{
		case SQLITE_INTEGER:
		{
			const char* decl = sqlite3_column_decltype(stmt, i);
			lua_pushstring(L, name);
			// case-sensitive, as in the original: Lekmod's "BOOLEAN" columns (Units.AnyIdeologyUnlock, Buildings.MaliTreasury)
			// give numbers for their INTEGER values
			if (decl && strcmp(decl, "boolean") == 0)
				lua_pushboolean(L, sqlite3_column_int64(stmt, i) != 0);
			else
				lua_pushnumber(L, (lua_Number)sqlite3_column_int64(stmt, i));
			break;
		}
		case SQLITE_FLOAT:
			lua_pushstring(L, name);
			lua_pushnumber(L, (lua_Number)sqlite3_column_double(stmt, i));
			break;
		case SQLITE_TEXT:
			lua_pushstring(L, name);
			lua_pushlstring(L, (const char*)sqlite3_column_text(stmt, i), (size_t)sqlite3_column_bytes(stmt, i));
			break;
		case SQLITE_BLOB:
		{
			lua_pushstring(L, name);
			const void* blob = sqlite3_column_blob(stmt, i);
			lua_pushlstring(L, blob ? (const char*)blob : "", (size_t)sqlite3_column_bytes(stmt, i));
			break;
		}
		default: continue;  // NULL: no field
		}
		lua_rawset(L, -3);
	}
	luaL_getmetatable(L, meta);
	lua_setmetatable(L, -2);
}

int lRowIndex(lua_State* L)
{
	(void)L;
	return 0;
}

int lRowNewIndex(lua_State* L)
{
	return luaL_error(L, "Database row is read-only from Lua.");
}

// ---- GameInfo.X ---------------------------------------------------------------------------------------------------
// Userdata of a GameInfo table. Its metatable holds __tablename, __db and __rowdata (ID -> row); its environment table
// caches Type -> row hits.
struct Table
{
	int idColumn;    // position of "ID" in "select * from X", -1 when the table has none (rows are not cached)
	int typeColumn;  // position of "Type", -1: indexing by string is an error
};

Table* CheckTable(lua_State* L, int idx)
{
	return (Table*)lua_touserdata(L, idx);
}

// The table's metatable field onto the stack.
void PushMetaField(lua_State* L, int tableIdx, const char* field)
{
	lua_getmetatable(L, tableIdx);
	lua_getfield(L, -1, field);
	lua_remove(L, -2);
}

sqlite3* TableDb(lua_State* L, int tableIdx)
{
	PushMetaField(L, tableIdx, "__db");
	sqlite3* db = Db(L, -1);
	lua_pop(L, 1);
	return db;
}

std::string TableName(lua_State* L, int tableIdx)
{
	PushMetaField(L, tableIdx, "__tablename");
	std::string name = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
	lua_pop(L, 1);
	return name;
}

// "select * from "X"" + suffix.
std::string SelectSql(const std::string& table, const char* suffix)
{
	char* sql = sqlite3_mprintf("select * from \"%w\"%s", table.c_str(), suffix ? suffix : "");
	std::string out = sql ? sql : "";
	sqlite3_free(sql);
	return out;
}

// The row of the current result of stmt (a "select * from X" statement) onto the stack: the cached object when the table
// has an ID column and the row was seen before, otherwise a new row (cached by ID when possible).
void PushTableRow(lua_State* L, int tableIdx, sqlite3_stmt* stmt)
{
	const Table* t = CheckTable(L, tableIdx);
	if (t->idColumn < 0 || sqlite3_column_type(stmt, t->idColumn) == SQLITE_NULL)
	{
		PushRow(L, stmt, 0, kRowMeta);
		return;
	}
	const lua_Number id = (lua_Number)sqlite3_column_int64(stmt, t->idColumn);
	PushMetaField(L, tableIdx, "__rowdata");
	lua_pushnumber(L, id);
	lua_rawget(L, -2);
	if (!lua_isnil(L, -1))
	{
		lua_remove(L, -2);
		return;
	}
	lua_pop(L, 1);
	PushRow(L, stmt, 0, kRowMeta);
	lua_pushnumber(L, id);
	lua_pushvalue(L, -2);
	lua_rawset(L, -4);
	lua_remove(L, -2);
}

// Single-row lookup: pushes the row or nil.
int LookupRow(lua_State* L, int tableIdx, const char* where, bool byText, int intKey, const char* textKey)
{
	sqlite3* db = TableDb(L, tableIdx);
	const std::string sql = SelectSql(TableName(L, tableIdx), where);
	sqlite3_stmt* stmt = NULL;
	if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, NULL) != SQLITE_OK || !stmt)
	{
		Log(LOG_WARN, "GameInfo: %s (%s)", sqlite3_errmsg(db), sql.c_str());
		if (stmt) sqlite3_finalize(stmt);
		lua_pushnil(L);
		return 1;
	}
	if (byText)
		sqlite3_bind_text(stmt, 1, textKey, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_int(stmt, 1, intKey);
	if (sqlite3_step(stmt) == SQLITE_ROW)
		PushTableRow(L, tableIdx, stmt);
	else
		lua_pushnil(L);
	sqlite3_finalize(stmt);
	return 1;
}

// X[key]
int lTableIndex(lua_State* L)
{
	const Table* t = CheckTable(L, 1);
	switch (lua_type(L, 2))
	{
	case LUA_TNUMBER:
	{
		const int id = RoundToInt(lua_tonumber(L, 2));
		if (t->idColumn >= 0)
		{
			PushMetaField(L, 1, "__rowdata");
			lua_pushnumber(L, (lua_Number)id);
			lua_rawget(L, -2);
			if (!lua_isnil(L, -1)) return 1;
			lua_pop(L, 2);
		}
		return LookupRow(L, 1, " where rowid = ?", false, id, NULL);
	}
	case LUA_TSTRING:
	{
		if (t->typeColumn < 0) return luaL_error(L, "Indexing by type is not available for this table.");
		const bool cacheable = t->idColumn >= 0;
		if (cacheable)
		{
			lua_getfenv(L, 1);
			lua_pushvalue(L, 2);
			lua_rawget(L, -2);
			if (!lua_isnil(L, -1)) return 1;
			lua_pop(L, 2);
		}
		LookupRow(L, 1, " where Type = ?", true, 0, lua_tostring(L, 2));
		if (cacheable && !lua_isnil(L, -1))
		{
			lua_getfenv(L, 1);
			lua_pushvalue(L, 2);
			lua_pushvalue(L, -3);
			lua_rawset(L, -3);
			lua_pop(L, 1);
		}
		return 1;
	}
	default: return luaL_argerror(L, 2, "Key must be of type 'number' or 'string'.");
	}
}

int lTableNewIndex(lua_State* L)
{
	return luaL_error(L, "Database table is read-only from Lua.");
}

int lTableLen(lua_State* L)
{
	sqlite3* db = TableDb(L, 1);
	char* sql = sqlite3_mprintf("select count(*) from \"%w\"", TableName(L, 1).c_str());
	sqlite3_stmt* stmt = NULL;
	lua_Number n = 0;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK && stmt && sqlite3_step(stmt) == SQLITE_ROW)
		n = (lua_Number)sqlite3_column_int64(stmt, 0);
	if (stmt) sqlite3_finalize(stmt);
	sqlite3_free(sql);
	lua_pushnumber(L, n);
	return 1;
}

// Iterator of X(...): upvalue 1 = Query, upvalue 2 = the table userdata.
int lTableIter(lua_State* L)
{
	Query* q = (Query*)lua_touserdata(L, lua_upvalueindex(1));
	if (q->failed)
	{
		lua_pushstring(L, kIterateError);
		return lua_error(L);
	}
	if (q->done || !q->stmt) return 0;
	const int rc = sqlite3_step(q->stmt);
	if (rc == SQLITE_ROW)
	{
		PushTableRow(L, lua_upvalueindex(2), q->stmt);
		return 1;
	}
	if (rc != SQLITE_DONE)
	{
		Log(LOG_WARN, "GameInfo: %s (%s)", sqlite3_errmsg(sqlite3_db_handle(q->stmt)), sqlite3_sql(q->stmt));
		Finish(q);
		lua_pushstring(L, kIterateError);
		return lua_error(L);
	}
	Finish(q);
	return 0;
}

struct FilterBind
{
	bool text;
	std::string s;
	int i;
};

// X(), X{k = v, ...}, X("condition")
int lTableCall(lua_State* L)
{
	std::string where;
	const int filterType = lua_type(L, 2);
	// filter table: keys and values copied out first (lua_tolstring would change number keys during lua_next)
	std::vector<FilterBind> binds;
	if (filterType == LUA_TSTRING)
		where = std::string(" where ") + lua_tostring(L, 2);
	else if (filterType == LUA_TTABLE)
	{
		where = " where ";
		lua_pushnil(L);
		while (lua_next(L, 2) != 0)
		{
			lua_pushvalue(L, -2);
			const char* key = lua_tostring(L, -1);
			if (key)
			{
				if (!binds.empty()) where += " and ";
				where += key;
				where += " = ?";
				FilterBind b;
				b.text = lua_type(L, -2) == LUA_TSTRING;
				b.i = 0;
				if (b.text)
				{
					size_t len = 0;
					const char* s = lua_tolstring(L, -2, &len);
					b.s.assign(s, len);
				}
				else
					b.i = RoundToInt(lua_tonumber(L, -2));
				binds.push_back(b);
			}
			lua_pop(L, 2);
		}
	}
	else if (filterType != LUA_TNONE && filterType != LUA_TNIL)
	{
		lua_pushstring(L, "Filter must be of type 'string' or 'table'");
		return lua_error(L);
	}

	sqlite3* db = TableDb(L, 1);
	const std::string sql = SelectSql(TableName(L, 1), where.empty() ? NULL : where.c_str());
	Query* q = NewQuery(L);
	if (sqlite3_prepare_v2(db, sql.c_str(), -1, &q->stmt, NULL) != SQLITE_OK || !q->stmt)
	{
		Log(LOG_WARN, "GameInfo: %s (%s)", sqlite3_errmsg(db), sql.c_str());
		if (q->stmt) sqlite3_finalize(q->stmt);
		q->stmt = NULL;
		q->failed = true;
	}
	else
	{
		for (size_t i = 0; i < binds.size(); ++i)
		{
			if (binds[i].text)
				sqlite3_bind_text(q->stmt, (int)i + 1, binds[i].s.data(), (int)binds[i].s.size(), SQLITE_TRANSIENT);
			else
				sqlite3_bind_int(q->stmt, (int)i + 1, binds[i].i);
		}
	}
	lua_pushvalue(L, 1);
	lua_pushcclosure(L, lTableIter, 2);
	return 1;
}

// Position of a result column by exact name, -1 when absent.
int ColumnOf(sqlite3_stmt* stmt, const char* name)
{
	const int n = sqlite3_column_count(stmt);
	for (int i = 0; i < n; ++i)
	{
		const char* c = sqlite3_column_name(stmt, i);
		if (c && strcmp(c, name) == 0) return i;
	}
	return -1;
}

// GameInfo.__index(name): the table object, created on first use; nil when the main DB has no such table.
int lGameInfoIndex(lua_State* L)
{
	if (lua_type(L, 2) != LUA_TSTRING) return 0;
	sqlite3* db = Db(L, lua_upvalueindex(1));
	const char* name = lua_tostring(L, 2);

	sqlite3_stmt* stmt = NULL;
	bool exists = false;
	if (sqlite3_prepare_v2(db, "select 1 from main.sqlite_master where type = 'table' and name = ?", -1, &stmt, NULL) == SQLITE_OK && stmt)
	{
		sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
		exists = sqlite3_step(stmt) == SQLITE_ROW;
	}
	if (stmt) sqlite3_finalize(stmt);
	if (!exists) return 0;

	Table info = {-1, -1};
	stmt = NULL;
	const std::string sql = SelectSql(name, NULL);
	if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, NULL) == SQLITE_OK && stmt)
	{
		info.idColumn = ColumnOf(stmt, "ID");
		info.typeColumn = ColumnOf(stmt, "Type");
	}
	if (stmt) sqlite3_finalize(stmt);

	Table* t = (Table*)lua_newuserdata(L, sizeof(Table));
	*t = info;
	const int ud = lua_gettop(L);
	lua_newtable(L);  // metatable
	lua_pushvalue(L, 2);
	lua_setfield(L, -2, "__tablename");
	lua_pushlightuserdata(L, db);
	lua_setfield(L, -2, "__db");
	lua_newtable(L);
	lua_setfield(L, -2, "__rowdata");
	lua_pushcfunction(L, lTableIndex);
	lua_setfield(L, -2, "__index");
	lua_pushcfunction(L, lTableNewIndex);
	lua_setfield(L, -2, "__newindex");
	lua_pushcfunction(L, lTableCall);
	lua_setfield(L, -2, "__call");
	lua_pushcfunction(L, lTableLen);
	lua_setfield(L, -2, "__len");
	lua_setmetatable(L, ud);
	lua_newtable(L);  // environment: Type -> row cache
	lua_setfenv(L, ud);

	lua_pushvalue(L, 2);
	lua_pushvalue(L, ud);
	lua_rawset(L, 1);
	return 1;
}

// ---- DB.Query (port of civhost lDbQuery / lDbQueryIter) --------------------------------------------------------------
int lDbQueryIter(lua_State* L)
{
	Query* q = (Query*)lua_touserdata(L, lua_upvalueindex(1));
	if (!q || !q->stmt) return 0;
	if (sqlite3_step(q->stmt) == SQLITE_ROW)
	{
		PushRow(L, q->stmt, 0, kDbRowMeta);
		return 1;
	}
	Finish(q);
	return 0;
}

int lDbQuery(lua_State* L)
{
	sqlite3* db = Db(L, lua_upvalueindex(1));
	const char* sql = luaL_checkstring(L, 1);
	const int nParams = lua_gettop(L) - 1;
	Query* q = NewQuery(L);
	if (sqlite3_prepare_v2(db, sql, -1, &q->stmt, NULL) != SQLITE_OK || !q->stmt)
	{
		if (q->stmt) sqlite3_finalize(q->stmt);
		q->stmt = NULL;
		return luaL_error(L, "DB.Query: %s (%s)", sqlite3_errmsg(db), sql);
	}
	for (int i = 1; i <= nParams; ++i)
	{
		const int idx = i + 1;
		switch (lua_type(L, idx))
		{
		case LUA_TNUMBER:
		{
			const double d = lua_tonumber(L, idx);
			if (d >= -2147483648.0 && d <= 2147483647.0 && (double)(int)d == d)
				sqlite3_bind_int(q->stmt, i, (int)d);
			else
				sqlite3_bind_double(q->stmt, i, d);
			break;
		}
		case LUA_TSTRING:
		{
			size_t len = 0;
			const char* s = lua_tolstring(L, idx, &len);
			sqlite3_bind_text(q->stmt, i, s, (int)len, SQLITE_TRANSIENT);
			break;
		}
		case LUA_TBOOLEAN: sqlite3_bind_int(q->stmt, i, lua_toboolean(L, idx) ? 1 : 0); break;
		default: sqlite3_bind_null(q->stmt, i); break;
		}
	}
	lua_pushcclosure(L, lDbQueryIter, 1);  // upvalue: the statement userdata (keeps it alive)
	return 1;
}

}  // namespace

bool RegisterGameInfo(lua_State* L, Database::Connection* conn)
{
	sqlite3* db = conn ? conn->GetSQLite3() : NULL;
	if (!db) return false;
	const int top = lua_gettop(L);

	luaL_newmetatable(L, kQueryMeta);
	lua_pushcfunction(L, lQueryGc);
	lua_setfield(L, -2, "__gc");
	luaL_newmetatable(L, kRowMeta);
	lua_pushcfunction(L, lRowIndex);
	lua_setfield(L, -2, "__index");
	lua_pushcfunction(L, lRowNewIndex);
	lua_setfield(L, -2, "__newindex");
	luaL_newmetatable(L, kDbRowMeta);
	lua_pushcfunction(L, lRowIndex);
	lua_setfield(L, -2, "__index");
	lua_settop(L, top);

	// DB
	lua_newtable(L);
	lua_pushlightuserdata(L, db);
	lua_pushcclosure(L, lDbQuery, 1);
	lua_setfield(L, -2, "Query");
	lua_setglobal(L, "DB");

	// GameInfo
	lua_newtable(L);
	lua_newtable(L);
	lua_pushlightuserdata(L, db);
	lua_setfield(L, -2, "__db");
	lua_pushlightuserdata(L, db);
	lua_pushcclosure(L, lGameInfoIndex, 1);
	lua_setfield(L, -2, "__index");
	lua_setmetatable(L, -2);
	lua_setglobal(L, "GameInfo");

	lua_settop(L, top);
	return true;
}

}  // namespace lekmod_portable
