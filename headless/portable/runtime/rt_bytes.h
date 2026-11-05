// rt_bytes: little-endian byte writer / bounds-checked reader for the runtime's own save sections (rt_save, task G4a). C++03, header only.
// The reader never reads past its buffer: a short or malformed input clears Ok() and every later read returns 0 / empty.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <string>
#include <vector>

namespace lekmod_portable {

class RtByteWriter
{
public:
	explicit RtByteWriter(std::vector<uint8_t>& out) : m_out(out) {}
	void U8(uint32_t v) { m_out.push_back((uint8_t)v); }
	void U16(uint32_t v) { U8(v & 0xff); U8((v >> 8) & 0xff); }
	void U32(uint32_t v) { for (int i = 0; i < 4; ++i) U8((v >> (8 * i)) & 0xff); }
	void I32(int32_t v) { U32((uint32_t)v); }
	void U64(uint64_t v) { for (int i = 0; i < 8; ++i) U8((uint32_t)((v >> (8 * i)) & 0xff)); }
	void Bool(bool v) { U8(v ? 1 : 0); }
	void F64(double v) { uint64_t u; memcpy(&u, &v, 8); U64(u); }
	void Bytes(const void* p, size_t n) { if (n) m_out.insert(m_out.end(), (const uint8_t*)p, (const uint8_t*)p + n); }
	void Str(const std::string& s) { U32((uint32_t)s.size()); Bytes(s.data(), s.size()); }
	size_t Size() const { return m_out.size(); }

private:
	std::vector<uint8_t>& m_out;
};

class RtByteReader
{
public:
	RtByteReader(const uint8_t* p, size_t n) : m_p(p), m_n(n), m_pos(0), m_ok(true) {}
	bool Ok() const { return m_ok; }
	bool AtEnd() const { return m_pos == m_n; }
	size_t Pos() const { return m_pos; }
	size_t Left() const { return m_n - m_pos; }
	void Fail() { m_ok = false; m_pos = m_n; }
	uint32_t U8() { if (!Need(1)) return 0; return m_p[m_pos++]; }
	uint32_t U16() { if (!Need(2)) return 0; uint32_t v = (uint32_t)m_p[m_pos] | ((uint32_t)m_p[m_pos + 1] << 8); m_pos += 2; return v; }
	uint32_t U32()
	{
		if (!Need(4)) return 0;
		uint32_t v = 0;
		for (int i = 3; i >= 0; --i) v = (v << 8) | m_p[m_pos + i];
		m_pos += 4;
		return v;
	}
	int32_t I32() { return (int32_t)U32(); }
	uint64_t U64()
	{
		if (!Need(8)) return 0;
		uint64_t v = 0;
		for (int i = 7; i >= 0; --i) v = (v << 8) | m_p[m_pos + i];
		m_pos += 8;
		return v;
	}
	bool Bool() { return U8() != 0; }
	double F64() { uint64_t u = U64(); double v; memcpy(&v, &u, 8); return v; }
	// A count that must fit in what is left at minBytes per item (guards allocations against corrupt counts).
	uint32_t Count(size_t minBytes)
	{
		const uint32_t n = U32();
		if (m_ok && minBytes > 0 && (size_t)n > Left() / minBytes) { Fail(); return 0; }
		return n;
	}
	std::string Str(size_t maxLen = 16u << 20)
	{
		const uint32_t n = U32();
		if (!m_ok || n > maxLen || !Need(n)) { Fail(); return std::string(); }
		std::string s((const char*)m_p + m_pos, n);
		m_pos += n;
		return s;
	}
	const uint8_t* Take(size_t n)
	{
		if (!Need(n)) return NULL;
		const uint8_t* p = m_p + m_pos;
		m_pos += n;
		return p;
	}

private:
	bool Need(size_t k)
	{
		if (!m_ok || m_n - m_pos < k) { Fail(); return false; }
		return true;
	}
	const uint8_t* m_p;
	size_t m_n;
	size_t m_pos;
	bool m_ok;
};

}  // namespace lekmod_portable
