// rt_json: minimal JSON reader (objects keep their key order). Ported from headless/host/src/game/HstJson.{h,cpp}
// (host-only: HstCommon / Win32) for the portable runtime; adds surrogate pairs in \u escapes.
#pragma once

#include <stddef.h>

#include <string>
#include <utility>
#include <vector>

namespace lekmod_portable {

class RtJson
{
public:
	enum Type { JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT };

	RtJson() : m_type(JSON_NULL), m_bool(false), m_num(0.0) {}

	static bool Parse(const std::string& text, RtJson& out, std::string& error);

	Type GetType() const { return m_type; }
	bool IsNull() const { return m_type == JSON_NULL; }
	const RtJson* Get(const char* key) const;  // object member or NULL
	size_t Size() const { return m_type == JSON_ARRAY ? m_items.size() : m_members.size(); }
	const RtJson& At(size_t i) const { return m_items[i]; }
	const std::string& KeyAt(size_t i) const { return m_members[i].first; }
	const RtJson& ValueAt(size_t i) const { return m_members[i].second; }

	bool AsBool(bool def = false) const { return m_type == JSON_BOOL ? m_bool : def; }
	double AsNumber(double def = 0.0) const { return m_type == JSON_NUMBER ? m_num : def; }
	int AsInt(int def = 0) const { return m_type == JSON_NUMBER ? (int)m_num : def; }
	std::string AsString(const char* def = "") const { return m_type == JSON_STRING ? m_str : std::string(def); }

private:
	Type m_type;
	bool m_bool;
	double m_num;
	std::string m_str;
	std::vector<RtJson> m_items;
	std::vector<std::pair<std::string, RtJson> > m_members;

	friend class RtJsonParser;
};

}  // namespace lekmod_portable
