//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstJson.cpp
//!  \brief     Recursive-descent JSON parser (UTF-8 passthrough, \uXXXX for BMP only).
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#include "HstCommon.h"
#include "game/HstJson.h"
#include "script/HstVfs.h"

class HstJsonParser
{
public:
	HstJsonParser(const std::string& strText) : m_str(strText), m_i(0) {}

	bool ParseDocument(HstJson& kOut, std::string& strError)
	{
		if(!ParseValue(kOut))
		{
			strError = m_strError;
			return false;
		}
		SkipWs();
		if(m_i != m_str.size())
		{
			Fail("trailing characters");
			strError = m_strError;
			return false;
		}
		return true;
	}

protected:
	bool Fail(const char* szWhat)
	{
		char szBuf[128];
		_snprintf_s(szBuf, sizeof(szBuf), _TRUNCATE, "%s at offset %u", szWhat, (unsigned int)m_i);
		m_strError = szBuf;
		return false;
	}

	void SkipWs()
	{
		while(m_i < m_str.size())
		{
			char c = m_str[m_i];
			if(c == ' ' || c == '\t' || c == '\r' || c == '\n')
				++m_i;
			else if(c == '/' && m_i + 1 < m_str.size() && m_str[m_i + 1] == '/')	// tolerate // comments
			{
				while(m_i < m_str.size() && m_str[m_i] != '\n')
					++m_i;
			}
			else
				break;
		}
	}

	bool ParseString(std::string& strOut)
	{
		if(m_str[m_i] != '"')
			return Fail("expected string");
		++m_i;
		strOut.clear();
		while(m_i < m_str.size())
		{
			char c = m_str[m_i++];
			if(c == '"')
				return true;
			if(c != '\\')
			{
				strOut += c;
				continue;
			}
			if(m_i >= m_str.size())
				break;
			char e = m_str[m_i++];
			switch(e)
			{
			case '"': strOut += '"'; break;
			case '\\': strOut += '\\'; break;
			case '/': strOut += '/'; break;
			case 'b': strOut += '\b'; break;
			case 'f': strOut += '\f'; break;
			case 'n': strOut += '\n'; break;
			case 'r': strOut += '\r'; break;
			case 't': strOut += '\t'; break;
			case 'u':
			{
				if(m_i + 4 > m_str.size())
					return Fail("bad \\u escape");
				unsigned int uiCode = (unsigned int)strtoul(m_str.substr(m_i, 4).c_str(), NULL, 16);
				m_i += 4;
				if(uiCode < 0x80)
					strOut += (char)uiCode;
				else if(uiCode < 0x800)
				{
					strOut += (char)(0xC0 | (uiCode >> 6));
					strOut += (char)(0x80 | (uiCode & 0x3F));
				}
				else
				{
					strOut += (char)(0xE0 | (uiCode >> 12));
					strOut += (char)(0x80 | ((uiCode >> 6) & 0x3F));
					strOut += (char)(0x80 | (uiCode & 0x3F));
				}
				break;
			}
			default:
				return Fail("bad escape");
			}
		}
		return Fail("unterminated string");
	}

	bool ParseValue(HstJson& kOut)
	{
		SkipWs();
		if(m_i >= m_str.size())
			return Fail("unexpected end");
		char c = m_str[m_i];
		if(c == '{')
		{
			kOut.m_eType = HstJson::JSON_OBJECT;
			++m_i;
			SkipWs();
			if(m_i < m_str.size() && m_str[m_i] == '}')
			{
				++m_i;
				return true;
			}
			while(true)
			{
				SkipWs();
				std::string strKey;
				if(m_i >= m_str.size() || !ParseString(strKey))
					return Fail("expected key");
				SkipWs();
				if(m_i >= m_str.size() || m_str[m_i] != ':')
					return Fail("expected ':'");
				++m_i;
				kOut.m_aMembers.push_back(std::make_pair(strKey, HstJson()));
				if(!ParseValue(kOut.m_aMembers.back().second))
					return false;
				SkipWs();
				if(m_i < m_str.size() && m_str[m_i] == ',')
				{
					++m_i;
					continue;
				}
				if(m_i < m_str.size() && m_str[m_i] == '}')
				{
					++m_i;
					return true;
				}
				return Fail("expected ',' or '}'");
			}
		}
		if(c == '[')
		{
			kOut.m_eType = HstJson::JSON_ARRAY;
			++m_i;
			SkipWs();
			if(m_i < m_str.size() && m_str[m_i] == ']')
			{
				++m_i;
				return true;
			}
			while(true)
			{
				kOut.m_aItems.push_back(HstJson());
				if(!ParseValue(kOut.m_aItems.back()))
					return false;
				SkipWs();
				if(m_i < m_str.size() && m_str[m_i] == ',')
				{
					++m_i;
					continue;
				}
				if(m_i < m_str.size() && m_str[m_i] == ']')
				{
					++m_i;
					return true;
				}
				return Fail("expected ',' or ']'");
			}
		}
		if(c == '"')
		{
			kOut.m_eType = HstJson::JSON_STRING;
			return ParseString(kOut.m_strValue);
		}
		if(m_str.compare(m_i, 4, "true") == 0)
		{
			kOut.m_eType = HstJson::JSON_BOOL;
			kOut.m_bValue = true;
			m_i += 4;
			return true;
		}
		if(m_str.compare(m_i, 5, "false") == 0)
		{
			kOut.m_eType = HstJson::JSON_BOOL;
			kOut.m_bValue = false;
			m_i += 5;
			return true;
		}
		if(m_str.compare(m_i, 4, "null") == 0)
		{
			kOut.m_eType = HstJson::JSON_NULL;
			m_i += 4;
			return true;
		}
		const char* szStart = m_str.c_str() + m_i;
		char* szEnd = NULL;
		double d = strtod(szStart, &szEnd);
		if(szEnd == szStart)
			return Fail("unexpected character");
		kOut.m_eType = HstJson::JSON_NUMBER;
		kOut.m_dValue = d;
		m_i += (size_t)(szEnd - szStart);
		return true;
	}

	const std::string& m_str;
	size_t m_i;
	std::string m_strError;
};

bool HstJson::Parse(const std::string& strText, HstJson& kOut, std::string& strError)
{
	HstJsonParser kParser(strText);
	return kParser.ParseDocument(kOut, strError);
}

bool HstJson::ParseFile(const std::wstring& wstrPath, HstJson& kOut, std::string& strError)
{
	std::string strText;
	if(!HstVfs::ReadFile(wstrPath, strText))
	{
		strError = "cannot read file";
		return false;
	}
	return Parse(strText, kOut, strError);
}

const HstJson* HstJson::Get(const char* szKey) const
{
	if(m_eType != JSON_OBJECT)
		return NULL;
	for(size_t i = 0; i < m_aMembers.size(); ++i)
		if(m_aMembers[i].first == szKey)
			return &m_aMembers[i].second;
	return NULL;
}
