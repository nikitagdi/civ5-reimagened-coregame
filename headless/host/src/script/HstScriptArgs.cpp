//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstScriptArgs.cpp
//!  \brief     Hook argument container. Values are kept in a reused vector, so a pooled instance does no heap
//!				traffic after warm-up (strings excepted).
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#include "HstCommon.h"
#include "script/HstScriptArgs.h"

void HstScriptArgs::Reset(unsigned int uiReserve)
{
	m_uiCount = 0;
	if(m_aValues.size() < uiReserve)
		m_aValues.resize(uiReserve);
}

HstScriptArgs::Value& HstScriptArgs::Add(ArgType eType)
{
	if(m_uiCount >= m_aValues.size())
		m_aValues.resize(m_uiCount + 8);
	Value& kValue = m_aValues[m_uiCount++];
	kValue.eType = eType;
	return kValue;
}

unsigned int HstScriptArgs::Push(bool bValue)
{
	Add(ARGTYPE_BOOL).bValue = bValue;
	return m_uiCount;
}

unsigned int HstScriptArgs::Push(const char* szValue, size_t lenValue)
{
	Value& kValue = Add(ARGTYPE_STRING);
	if(szValue)
		kValue.strValue.assign(szValue, lenValue);
	else
		kValue.strValue.clear();
	return m_uiCount;
}

unsigned int HstScriptArgs::Push(int iValue)
{
	Add(ARGTYPE_INT).iValue = iValue;
	return m_uiCount;
}

unsigned int HstScriptArgs::Push(float fValue)
{
	Add(ARGTYPE_FLOAT).fValue = fValue;
	return m_uiCount;
}

unsigned int HstScriptArgs::PushNULL()
{
	Add(ARGTYPE_NULL);
	return m_uiCount;
}

unsigned int HstScriptArgs::Count()
{
	return m_uiCount;
}

ICvEngineScriptSystemArgs1::ArgType HstScriptArgs::GetType(unsigned int argIndex)
{
	return argIndex < m_uiCount ? m_aValues[argIndex].eType : ARGTYPE_NONE;
}

bool HstScriptArgs::GetBool(unsigned int argIndex, bool& bValue)
{
	if(argIndex >= m_uiCount || m_aValues[argIndex].eType != ARGTYPE_BOOL)
		return false;
	bValue = m_aValues[argIndex].bValue;
	return true;
}

bool HstScriptArgs::GetFloat(unsigned int argIndex, float& fValue)
{
	if(argIndex >= m_uiCount || m_aValues[argIndex].eType != ARGTYPE_FLOAT)
		return false;
	fValue = m_aValues[argIndex].fValue;
	return true;
}

bool HstScriptArgs::GetInt(unsigned int argIndex, int& iValue)
{
	if(argIndex >= m_uiCount || m_aValues[argIndex].eType != ARGTYPE_INT)
		return false;
	iValue = m_aValues[argIndex].iValue;
	return true;
}

bool HstScriptArgs::GetString(unsigned int argIndex, char*& szValue)
{
	if(argIndex >= m_uiCount || m_aValues[argIndex].eType != ARGTYPE_STRING)
		return false;
	szValue = const_cast<char*>(m_aValues[argIndex].strValue.c_str());
	return true;
}

int HstScriptArgs::PushToLua(lua_State* L) const
{
	for(unsigned int i = 0; i < m_uiCount; ++i)
	{
		const Value& kValue = m_aValues[i];
		switch(kValue.eType)
		{
		case ARGTYPE_BOOL:
			lua_pushboolean(L, kValue.bValue);
			break;
		case ARGTYPE_STRING:
			lua_pushlstring(L, kValue.strValue.c_str(), kValue.strValue.size());
			break;
		case ARGTYPE_INT:
			lua_pushinteger(L, kValue.iValue);
			break;
		case ARGTYPE_FLOAT:
			lua_pushnumber(L, kValue.fValue);
			break;
		default:
			lua_pushnil(L);
			break;
		}
	}
	return (int)m_uiCount;
}
