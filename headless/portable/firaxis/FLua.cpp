// FLua (FireWorks Lua helpers): error handling, protected calls and registry-backed values, rewritten for the
// portable build. Lua itself is not part of libgamecore.a; the consumer links Lua 5.1.5 with C linkage.
// The FireWorks headers expect the gamecore precompiled-header environment.
#include "CvGameCoreDLLPCH.h"

#include <stdarg.h>
#include <stdio.h>
#include <string>

extern "C" {
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
}

#include "FireWorks/FFireTypes.h"
#include "FireWorks/FAssert.h"
#include "FireWorks/FCriticalSection.h"
#include "FireWorks/FString.h"
#include "FireWorks/Flua/include/FLua.h"
#include "lekmod_portable/host_log.h"

namespace FLua {

FCriticalSection ms_CriticalSection;
bool SharedLuaRegistry::sm_bAllowed = false;

void ErrorHandler::operator()(const TCHAR* szError) const
{
	if (m_pfnStatic) m_pfnStatic(szError);
	else if (m_pkClass && m_pfnFunc) (m_pkClass->*m_pfnFunc)(szError);
}

ScopedErrorHandler::ScopedErrorHandler(ErrorHandler kErrorHandler) : m_kPrevErrorHandler(GetErrorHandler()) { SetErrorHandler(kErrorHandler); }
ScopedErrorHandler::~ScopedErrorHandler() { SetErrorHandler(m_kPrevErrorHandler); }

namespace Details {

namespace {
char g_callstack[4096];
}

void Error(const TCHAR* szFormat, ...)
{
	char msg[1024];
	va_list ap;
	va_start(ap, szFormat);
	vsnprintf(msg, sizeof(msg), szFormat, ap);
	va_end(ap);
	lekmod_portable::Log(lekmod_portable::LOG_ERROR, "lua: %s", msg);
	ErrorHandlerHolder::Get()(msg);
}

void Error(lua_State* L, const TCHAR* szFormat, ...)
{
	char msg[1024];
	va_list ap;
	va_start(ap, szFormat);
	vsnprintf(msg, sizeof(msg), szFormat, ap);
	va_end(ap);
	SaveCallStack(L);
	lekmod_portable::Log(lekmod_portable::LOG_ERROR, "lua: %s", msg);
	ErrorHandlerHolder::Get()(msg);
}

const TCHAR* SaveCallStack(lua_State* L)
{
	std::string s;
	lua_Debug ar;
	for (int level = 0; L && lua_getstack(L, level, &ar) && level < 32; ++level)
	{
		lua_getinfo(L, "Sl", &ar);
		char line[256];
		snprintf(line, sizeof(line), "%s:%d\n", ar.short_src, ar.currentline);
		s += line;
	}
	strncpy_s(g_callstack, sizeof(g_callstack), s.c_str(), _TRUNCATE);
	return g_callstack;
}

const TCHAR* GetLastCallstackBuffer() { return g_callstack; }

bool CallWithErrorHandling(lua_State* L, unsigned int uiArgCount, unsigned int uiResults)
{
	int rc = lua_pcall(L, (int)uiArgCount, (int)uiResults, 0);
	if (rc != 0)
	{
		Error(L, "%s", lua_isstring(L, -1) ? lua_tostring(L, -1) : "lua error");
		lua_pop(L, 1);
		return false;
	}
	return true;
}

bool MultRetCallWithErrorHandling(lua_State* L, unsigned int uiArgCount, unsigned int& uiReturnCountOut)
{
	int base = lua_gettop(L) - (int)uiArgCount - 1;
	int rc = lua_pcall(L, (int)uiArgCount, LUA_MULTRET, 0);
	if (rc != 0)
	{
		Error(L, "%s", lua_isstring(L, -1) ? lua_tostring(L, -1) : "lua error");
		lua_pop(L, 1);
		uiReturnCountOut = 0;
		return false;
	}
	uiReturnCountOut = (unsigned int)(lua_gettop(L) - base);
	return true;
}

bool CCallWithErrorHandling(lua_State* L, lua_CFunction pfn, void* pvUserData)
{
	ms_CriticalSection.Enter();
	int rc = lua_cpcall(L, pfn, pvUserData);
	if (rc != 0)
	{
		HandleCPCallError(L, rc);
		lua_pop(L, 1);
	}
	ms_CriticalSection.Leave();
	return rc == 0;
}

void HandleCPCallError(lua_State* L, int iCPCallResults)
{
	const char* msg = lua_isstring(L, -1) ? lua_tostring(L, -1) : "unknown error";
	Error(L, "protected call failed (%d): %s", iCPCallResults, msg);
}

}  // namespace Details

lua_State* CreateLuaState()
{
	lua_State* L = luaL_newstate();
	if (L) luaL_openlibs(L);
	return L;
}

lua_State* CreateLuaState(lua_Alloc pfnAllocFn, void* pvUserData)
{
	lua_State* L = lua_newstate(pfnAllocFn, pvUserData);
	if (L) luaL_openlibs(L);
	return L;
}

bool Execute(lua_State* L, const char* szCode)
{
	int top = lua_gettop(L);
	if (luaL_loadstring(L, szCode) != 0)
	{
		Details::Error(L, "%s", lua_tostring(L, -1));
		lua_settop(L, top);
		return false;
	}
	bool ok = Details::CallWithErrorHandling(L, 0, 0);
	lua_settop(L, top);
	return ok;
}

bool LoadFile(lua_State* L, const wchar_t* wszFilename)
{
	std::string name;
	for (const wchar_t* p = wszFilename; p && *p; ++p) name += (char)*p;
	int top = lua_gettop(L);
	if (luaL_loadfile(L, name.c_str()) != 0)
	{
		Details::Error(L, "%s", lua_tostring(L, -1));
		lua_settop(L, top);
		return false;
	}
	bool ok = Details::CallWithErrorHandling(L, 0, 0);
	lua_settop(L, top);
	return ok;
}

// Pushes the value of a dotted path (a.b.c) relative to the table at iTableIndex; nil when a link is missing.
void GetField(lua_State* L, int iTableIndex, const char* szName)
{
	lua_pushvalue(L, iTableIndex);
	std::string path = szName ? szName : "";
	size_t start = 0;
	for (;;)
	{
		size_t dot = path.find('.', start);
		std::string key = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
		if (!lua_istable(L, -1))
		{
			lua_pop(L, 1);
			lua_pushnil(L);
			return;
		}
		lua_getfield(L, -1, key.c_str());
		lua_remove(L, -2);
		if (dot == std::string::npos) return;
		start = dot + 1;
	}
}

StackValue::StackValue(lua_State* L, int iStackIndex) : m_pkLuaState(L), m_iStackIndex(iStackIndex < 0 && iStackIndex > LUA_REGISTRYINDEX ? lua_gettop(L) + iStackIndex + 1 : iStackIndex) {}

// ---- Value: a registry reference ------------------------------------------------------------------
Value::Value() : m_pkLuaState(NULL), m_iRef(LUA_NOREF) {}
Value::Value(const Value& kRhs) : m_pkLuaState(NULL), m_iRef(LUA_NOREF)
{
	if (kRhs.Push())
	{
		m_pkLuaState = kRhs.m_pkLuaState;
		m_iRef = luaL_ref(m_pkLuaState, LUA_REGISTRYINDEX);
	}
}
Value::Value(const StackValue& kStackValue) : m_pkLuaState(NULL), m_iRef(LUA_NOREF) { Set(kStackValue.GetLuaState(), kStackValue.GetIndex()); }
Value::Value(lua_State* L, int iStackIndex) : m_pkLuaState(NULL), m_iRef(LUA_NOREF) { Set(L, iStackIndex); }
Value::Value(lua_State* L, const char* szName) : m_pkLuaState(NULL), m_iRef(LUA_NOREF) { Set(L, szName); }
Value::~Value() { Unref(); }

void Value::Unref()
{
	if (m_pkLuaState && m_iRef != LUA_NOREF && m_iRef != LUA_REFNIL) luaL_unref(m_pkLuaState, LUA_REGISTRYINDEX, m_iRef);
	m_iRef = LUA_NOREF;
	m_pkLuaState = NULL;
}

void Value::Swap(Value& kRhs)
{
	lua_State* L = m_pkLuaState;
	int r = m_iRef;
	m_pkLuaState = kRhs.m_pkLuaState;
	m_iRef = kRhs.m_iRef;
	kRhs.m_pkLuaState = L;
	kRhs.m_iRef = r;
}

const Value& Value::operator=(const Value& kRhs)
{
	if (this != &kRhs)
	{
		Value copy(kRhs);
		Swap(copy);
	}
	return *this;
}

void Value::Set(lua_State* L, int iStackIndex)
{
	Unref();
	if (!L) return;
	m_pkLuaState = L;
	lua_checkstack(L, 1);
	lua_pushvalue(L, iStackIndex);
	m_iRef = luaL_ref(L, LUA_REGISTRYINDEX);
}

void Value::Set(lua_State* L, const char* szName)
{
	Unref();
	if (!L) return;
	GetField(L, LUA_GLOBALSINDEX, szName);
	m_pkLuaState = L;
	m_iRef = luaL_ref(L, LUA_REGISTRYINDEX);
}

void Value::LuaStateDestroyed()
{
	m_pkLuaState = NULL;
	m_iRef = LUA_NOREF;
}

const bool Value::operator==(const Value& kRhs) const
{
	if (!Valid() || !kRhs.Valid()) return Valid() == kRhs.Valid();
	if (m_pkLuaState != kRhs.m_pkLuaState) return false;
	Push();
	kRhs.Push();
	bool eq = lua_rawequal(m_pkLuaState, -1, -2) != 0;
	lua_pop(m_pkLuaState, 2);
	return eq;
}

bool Value::Push() const
{
	if (!m_pkLuaState || m_iRef == LUA_NOREF) return false;
	lua_checkstack(m_pkLuaState, 1);
	lua_rawgeti(m_pkLuaState, LUA_REGISTRYINDEX, m_iRef);
	return true;
}

Type Value::GetType() const
{
	if (!Push()) return TYPE_NONE;
	Type t = (Type)lua_type(m_pkLuaState, -1);
	lua_pop(m_pkLuaState, 1);
	return t;
}

const char* Value::GetTypeName() const
{
	if (!m_pkLuaState) return "none";
	return lua_typename(m_pkLuaState, GetType());
}

bool Value::IsNil() const { return GetType() == TYPE_NIL || GetType() == TYPE_NONE; }

const void* Value::ToPointer() const
{
	if (!Push()) return NULL;
	const void* p = lua_topointer(m_pkLuaState, -1);
	lua_pop(m_pkLuaState, 1);
	return p;
}

bool String::Push() const
{
	if (!m_kLuaVal.Push()) return false;
	if (lua_type(m_kLuaVal.GetLuaState(), -1) == LUA_TSTRING) return true;
	lua_pop(m_kLuaVal.GetLuaState(), 1);
	return false;
}

bool Table::Push() const
{
	if (!m_kLuaVal.Push()) return false;
	if (lua_type(m_kLuaVal.GetLuaState(), -1) == LUA_TTABLE) return true;
	lua_pop(m_kLuaVal.GetLuaState(), 1);
	return false;
}

}  // namespace FLua
