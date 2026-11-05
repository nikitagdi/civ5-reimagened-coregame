// GcqBuf: collects fixed-size GCQ1 records and a deduplicated string table, then lays out one reply buffer
//. Header-only so that every abi/gc_query_*.cpp can use it. gc_query.cpp has its own private copy
// (GcqWriter); the two produce identical bytes. C++03.
#pragma once

#include <map>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include "gamecore_abi.h"

namespace lekmod_portable {

class GcqBuf
{
public:
	GcqBuf(int32_t kind, uint32_t recordSize) : m_kind(kind), m_recordSize(recordSize), m_count(0) {}

	void Reserve(uint32_t records) { m_records.reserve((size_t)records * m_recordSize); }

	void Add(const void* record)
	{
		const uint8_t* p = static_cast<const uint8_t*>(record);
		m_records.insert(m_records.end(), p, p + m_recordSize);
		++m_count;
	}

	// Offset into the string table; NULL is "no string".
	uint32_t Str(const char* s)
	{
		if (!s) return GCQ_NO_STRING;
		std::map<std::string, uint32_t>::const_iterator it = m_pool.find(s);
		if (it != m_pool.end()) return it->second;
		const uint32_t off = (uint32_t)m_strings.size();
		m_strings.append(s, strlen(s) + 1);
		m_pool[s] = off;
		return off;
	}

	bool Finish(gc_buf* out)
	{
		const size_t total = sizeof(gcq_header) + m_records.size() + m_strings.size();
		uint8_t* data = static_cast<uint8_t*>(malloc(total > 0 ? total : 1));
		if (!data) return false;
		gcq_header h;
		memcpy(h.magic, "GCQ1", 4);
		h.format = 1;
		h.kind = (uint16_t)m_kind;
		h.count = m_count;
		h.record_size = m_recordSize;
		h.strings_offset = (uint32_t)(sizeof(gcq_header) + m_records.size());
		h.strings_size = (uint32_t)m_strings.size();
		memcpy(data, &h, sizeof(h));
		if (!m_records.empty()) memcpy(data + sizeof(h), &m_records[0], m_records.size());
		if (!m_strings.empty()) memcpy(data + h.strings_offset, m_strings.data(), m_strings.size());
		out->data = data;
		out->len = total;
		out->release = &Release;
		out->owner = NULL;
		return true;
	}

private:
	static void Release(gc_buf* b) { free(b->data); }

	int32_t m_kind;
	uint32_t m_recordSize;
	uint32_t m_count;
	std::vector<uint8_t> m_records;
	std::string m_strings;
	std::map<std::string, uint32_t> m_pool;
};

}  // namespace lekmod_portable
