//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstJson.h
//!  \brief     Minimal JSON reader for scenario files (objects keep their key order).
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#pragma once
#ifndef HSTJSON_H
#define HSTJSON_H

#include "HstCommon.h"

class HstJson
{
public:
	enum Type { JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT };

	HstJson() : m_eType(JSON_NULL), m_bValue(false), m_dValue(0.0) {}

	static bool Parse(const std::string& strText, HstJson& kOut, std::string& strError);
	static bool ParseFile(const std::wstring& wstrPath, HstJson& kOut, std::string& strError);

	Type GetType() const { return m_eType; }
	bool IsNull() const { return m_eType == JSON_NULL; }
	const HstJson* Get(const char* szKey) const;	// object member or NULL
	size_t Size() const { return m_eType == JSON_ARRAY ? m_aItems.size() : m_aMembers.size(); }
	const HstJson& At(size_t i) const { return m_aItems[i]; }
	const std::string& KeyAt(size_t i) const { return m_aMembers[i].first; }
	const HstJson& ValueAt(size_t i) const { return m_aMembers[i].second; }

	bool AsBool(bool bDefault = false) const { return m_eType == JSON_BOOL ? m_bValue : bDefault; }
	double AsNumber(double dDefault = 0.0) const { return m_eType == JSON_NUMBER ? m_dValue : dDefault; }
	int AsInt(int iDefault = 0) const { return m_eType == JSON_NUMBER ? (int)m_dValue : iDefault; }
	unsigned int AsUInt(unsigned int uiDefault = 0) const { return m_eType == JSON_NUMBER ? (unsigned int)m_dValue : uiDefault; }
	std::string AsString(const char* szDefault = "") const { return m_eType == JSON_STRING ? m_strValue : std::string(szDefault); }

	// Convenience lookups on objects
	int GetInt(const char* szKey, int iDefault) const { const HstJson* p = Get(szKey); return p ? p->AsInt(iDefault) : iDefault; }
	unsigned int GetUInt(const char* szKey, unsigned int uiDefault) const { const HstJson* p = Get(szKey); return p ? p->AsUInt(uiDefault) : uiDefault; }
	bool GetBool(const char* szKey, bool bDefault) const { const HstJson* p = Get(szKey); return p ? p->AsBool(bDefault) : bDefault; }
	std::string GetString(const char* szKey, const char* szDefault) const { const HstJson* p = Get(szKey); return p ? p->AsString(szDefault) : std::string(szDefault); }

protected:
	Type m_eType;
	bool m_bValue;
	double m_dValue;
	std::string m_strValue;
	std::vector<HstJson> m_aItems;
	std::vector<std::pair<std::string, HstJson> > m_aMembers;

	friend class HstJsonParser;
};

#endif // HSTJSON_H
