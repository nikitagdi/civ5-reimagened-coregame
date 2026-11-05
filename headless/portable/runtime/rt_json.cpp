#include "rt_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace lekmod_portable {

class RtJsonParser
{
public:
	explicit RtJsonParser(const std::string& text) : m_s(text), m_i(0) {}

	bool Document(RtJson& out, std::string& error)
	{
		bool ok = Value(out, 0);
		if (ok)
		{
			SkipWs();
			if (m_i != m_s.size()) ok = Fail("trailing characters");
		}
		if (!ok) error = m_error;
		return ok;
	}

private:
	bool Fail(const char* what)
	{
		char buf[128];
		snprintf(buf, sizeof(buf), "%s at offset %u", what, (unsigned)m_i);
		m_error = buf;
		return false;
	}

	void SkipWs()
	{
		while (m_i < m_s.size())
		{
			char c = m_s[m_i];
			if (c == ' ' || c == '\t' || c == '\r' || c == '\n') ++m_i;
			else break;
		}
	}

	static void AppendUtf8(std::string& out, unsigned code)
	{
		if (code < 0x80) out += (char)code;
		else if (code < 0x800)
		{
			out += (char)(0xC0 | (code >> 6));
			out += (char)(0x80 | (code & 0x3F));
		}
		else if (code < 0x10000)
		{
			out += (char)(0xE0 | (code >> 12));
			out += (char)(0x80 | ((code >> 6) & 0x3F));
			out += (char)(0x80 | (code & 0x3F));
		}
		else
		{
			out += (char)(0xF0 | (code >> 18));
			out += (char)(0x80 | ((code >> 12) & 0x3F));
			out += (char)(0x80 | ((code >> 6) & 0x3F));
			out += (char)(0x80 | (code & 0x3F));
		}
	}

	bool Hex4(unsigned& code)
	{
		if (m_i + 4 > m_s.size()) return Fail("bad unicode escape");
		code = (unsigned)strtoul(m_s.substr(m_i, 4).c_str(), NULL, 16);
		m_i += 4;
		return true;
	}

	bool String(std::string& out)
	{
		if (m_i >= m_s.size() || m_s[m_i] != '"') return Fail("expected string");
		++m_i;
		out.clear();
		while (m_i < m_s.size())
		{
			char c = m_s[m_i++];
			if (c == '"') return true;
			if (c != '\\')
			{
				out += c;
				continue;
			}
			if (m_i >= m_s.size()) break;
			char e = m_s[m_i++];
			switch (e)
			{
			case '"': out += '"'; break;
			case '\\': out += '\\'; break;
			case '/': out += '/'; break;
			case 'b': out += '\b'; break;
			case 'f': out += '\f'; break;
			case 'n': out += '\n'; break;
			case 'r': out += '\r'; break;
			case 't': out += '\t'; break;
			case 'u':
			{
				unsigned code;
				if (!Hex4(code)) return false;
				if (code >= 0xD800 && code < 0xDC00 && m_i + 1 < m_s.size() && m_s[m_i] == '\\' && m_s[m_i + 1] == 'u')
				{
					size_t save = m_i;
					m_i += 2;
					unsigned low;
					if (!Hex4(low)) return false;
					if (low >= 0xDC00 && low < 0xE000) code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
					else m_i = save;
				}
				AppendUtf8(out, code);
				break;
			}
			default: return Fail("bad escape");
			}
		}
		return Fail("unterminated string");
	}

	bool Value(RtJson& out, int depth)
	{
		if (depth > 64) return Fail("nesting too deep");
		SkipWs();
		if (m_i >= m_s.size()) return Fail("unexpected end");
		char c = m_s[m_i];
		if (c == '{')
		{
			out.m_type = RtJson::JSON_OBJECT;
			++m_i;
			SkipWs();
			if (m_i < m_s.size() && m_s[m_i] == '}')
			{
				++m_i;
				return true;
			}
			for (;;)
			{
				SkipWs();
				std::string key;
				if (!String(key)) return false;
				SkipWs();
				if (m_i >= m_s.size() || m_s[m_i] != ':') return Fail("expected ':'");
				++m_i;
				out.m_members.push_back(std::make_pair(key, RtJson()));
				if (!Value(out.m_members.back().second, depth + 1)) return false;
				SkipWs();
				if (m_i < m_s.size() && m_s[m_i] == ',') { ++m_i; continue; }
				if (m_i < m_s.size() && m_s[m_i] == '}') { ++m_i; return true; }
				return Fail("expected ',' or '}'");
			}
		}
		if (c == '[')
		{
			out.m_type = RtJson::JSON_ARRAY;
			++m_i;
			SkipWs();
			if (m_i < m_s.size() && m_s[m_i] == ']')
			{
				++m_i;
				return true;
			}
			for (;;)
			{
				out.m_items.push_back(RtJson());
				if (!Value(out.m_items.back(), depth + 1)) return false;
				SkipWs();
				if (m_i < m_s.size() && m_s[m_i] == ',') { ++m_i; continue; }
				if (m_i < m_s.size() && m_s[m_i] == ']') { ++m_i; return true; }
				return Fail("expected ',' or ']'");
			}
		}
		if (c == '"')
		{
			out.m_type = RtJson::JSON_STRING;
			return String(out.m_str);
		}
		if (m_s.compare(m_i, 4, "true") == 0) { out.m_type = RtJson::JSON_BOOL; out.m_bool = true; m_i += 4; return true; }
		if (m_s.compare(m_i, 5, "false") == 0) { out.m_type = RtJson::JSON_BOOL; out.m_bool = false; m_i += 5; return true; }
		if (m_s.compare(m_i, 4, "null") == 0) { out.m_type = RtJson::JSON_NULL; m_i += 4; return true; }
		const char* start = m_s.c_str() + m_i;
		char* end = NULL;
		double d = strtod(start, &end);
		if (end == start) return Fail("unexpected character");
		out.m_type = RtJson::JSON_NUMBER;
		out.m_num = d;
		m_i += (size_t)(end - start);
		return true;
	}

	const std::string& m_s;
	size_t m_i;
	std::string m_error;
};

bool RtJson::Parse(const std::string& text, RtJson& out, std::string& error)
{
	out = RtJson();
	RtJsonParser parser(text);
	return parser.Document(out, error);
}

const RtJson* RtJson::Get(const char* key) const
{
	if (m_type != JSON_OBJECT) return NULL;
	for (size_t i = 0; i < m_members.size(); ++i)
		if (m_members[i].first == key) return &m_members[i].second;
	return NULL;
}

}  // namespace lekmod_portable
