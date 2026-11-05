// rt_lua_gameinfo: the exe-Lua `GameInfo` and `DB.Query` over sqlite (rt_lua; task A5).
// The original is closed code (CvGameDatabase: Database::Scripting::Lua::PushDatabase / PushDatabaseRow, exe RVA 0x7ccf0),
// so this is a rewrite whose behaviour was recorded from civhost with headless/tools/clang/runtime/lua/gameinfo/
// gameinfo_probe.lua (scenario `gameinfo` compares against civhost_reference.txt):
//   GameInfo           table, metatable {__db, __index}; GameInfo.X is created on first use and rawset into GameInfo.
//                      X must be a table of the main DB, case-sensitive (attached Localization tables: nil); otherwise nil.
//   GameInfo.X         userdata, metatable {__call, __db, __index, __len, __newindex, __rowdata, __tablename}.
//     X[n]             row by rowid (= ID); n rounded to nearest, ties to even (x87 fistp of the original). Missing: nil.
//     X["T"]           row whose Type = T; a table without a Type column raises "Indexing by type is not available ...".
//                      Other key types raise "Key must be of type 'number' or 'string'.".
//     X() X{k=v,...} X("sql")  one iterator function over "select * from X [where ...]"; a table filter is "k = ?" joined
//                      by AND, strings bound as text, anything else as the rounded integer of lua_tonumber (booleans: 0).
//                      Query errors (empty filter table, bad SQL) are raised by the first iterator call; another filter
//                      type raises "Filter must be of type 'string' or 'table'".
//     #X               count(*). Assigning a field raises "Database table is read-only from Lua.".
//   rows               tables with every non-NULL column; INTEGER values of a column declared "boolean" (exact case:
//                      "BOOLEAN" is not) are Lua booleans, other values keep their sqlite storage type. Metatable {__index, __newindex}: read-only. In a table
//                      with an ID column every row object is cached by ID (indexing and iteration return the same table);
//                      rows of other tables are fresh tables on each access.
//   DB.Query(sql, ...) civhost's lDbQuery (HstExeLua.cpp): iterator over the statement, `?` bound from the extra arguments
//                      (integral number -> int, other number -> double, string, boolean -> 0/1, else NULL). Rows as above
//                      but writable. A prepare error raises "DB.Query: <sqlite message> (<sql>)".
#pragma once

#include "CvGameDatabase.h"

struct lua_State;

namespace lekmod_portable {

// Sets the globals DB and GameInfo of L over db (not owned; it must outlive L). false when db has no connection.
bool RegisterGameInfo(lua_State* L, Database::Connection* db);

}  // namespace lekmod_portable
