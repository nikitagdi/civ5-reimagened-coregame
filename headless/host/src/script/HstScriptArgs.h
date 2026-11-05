//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstScriptArgs.h
//!  \brief     ICvEngineScriptSystemArgs1: tagged argument vector for GameEvents hooks (pooled by HstScriptSystem).
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#pragma once
#ifndef HSTSCRIPTARGS_H
#define HSTSCRIPTARGS_H

#include "HstCommon.h"

class HstScriptArgs : public ICvEngineScriptSystemArgs1
{
public:
	HstScriptArgs() : m_uiCount(0) {}
	virtual ~HstScriptArgs() {}

	virtual unsigned int Push(bool bValue);
	virtual unsigned int Push(const char* szValue, size_t lenValue);
	virtual unsigned int Push(int iValue);
	virtual unsigned int Push(float fValue);
	virtual unsigned int PushNULL();

	virtual unsigned int Count();
	virtual ArgType GetType(unsigned int argIndex);

	virtual bool GetBool(unsigned int argIndex, bool& bValue);
	virtual bool GetFloat(unsigned int argIndex, float& fValue);
	virtual bool GetInt(unsigned int argIndex, int& iValue);
	virtual bool GetString(unsigned int argIndex, char*& szValue);

	void Reset(unsigned int uiReserve);
	//! Push all arguments onto L (bool/string/integer/number/nil). Returns the count.
	int PushToLua(lua_State* L) const;

protected:
	struct Value
	{
		ArgType eType;
		bool bValue;
		int iValue;
		float fValue;
		std::string strValue;
	};
	Value& Add(ArgType eType);

	std::vector<Value> m_aValues;
	unsigned int m_uiCount;
};

#endif // HSTSCRIPTARGS_H
