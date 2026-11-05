// FDataStream / FMemoryStream / FFileStream (FireWorks), rewritten for the portable build.
// Stream format is the Win32 one: little-endian, 4-byte int/long/size counts, strings as INT32 length +
// bytes, wide strings as INT32 length + UTF-16 code units (wchar_t is 4 bytes outside Win32).
// The FireWorks headers expect the gamecore precompiled-header environment.
#include "CvGameCoreDLLPCH.h"

#include <stdexcept>
#include <string>
#include <vector>

#include "FireWorks/FFireTypes.h"
#include "FireWorks/FAssert.h"
#include "FireWorks/FDataStream.h"
#include "FireWorks/FMemoryStream.h"
#include "FireWorks/FFileStream.h"
#include "FireWorks/FString.h"
#include "FireWorks/FStlContainerSerialization.h"

// ---- FEndian -------------------------------------------------------------------------------------
FEndian::EEndianness FEndian::ms_eTargetEndianness = FEndian::ENDIAN_LITTLE;

FEndian::EEndianness FEndian::GetEndianness()
{
	return ENDIAN_LITTLE;  // every portable target (x86_64, wasm32) is little-endian
}

// ---- helpers -------------------------------------------------------------------------------------
namespace {

template <class T>
void swap_bytes(T& v)
{
	unsigned char* p = reinterpret_cast<unsigned char*>(&v);
	for (size_t i = 0; i < sizeof(T) / 2; ++i)
	{
		unsigned char t = p[i];
		p[i] = p[sizeof(T) - 1 - i];
		p[sizeof(T) - 1 - i] = t;
	}
}

// UTF-16 <-> wchar_t (UTF-32 here). Lone surrogates pass through unchanged.
void append_utf16(std::vector<unsigned short>& out, const wchar_t* s, size_t n)
{
	for (size_t i = 0; i < n; ++i)
	{
		unsigned int c = (unsigned int)s[i];
		if (c >= 0x10000 && c <= 0x10FFFF)
		{
			c -= 0x10000;
			out.push_back((unsigned short)(0xD800 + (c >> 10)));
			out.push_back((unsigned short)(0xDC00 + (c & 0x3FF)));
		}
		else
		{
			out.push_back((unsigned short)c);
		}
	}
}

std::wstring from_utf16(const std::vector<unsigned short>& in)
{
	std::wstring out;
	out.reserve(in.size());
	for (size_t i = 0; i < in.size(); ++i)
	{
		unsigned int c = in[i];
		if (c >= 0xD800 && c < 0xDC00 && i + 1 < in.size() && in[i + 1] >= 0xDC00 && in[i + 1] < 0xE000)
		{
			c = 0x10000 + ((c - 0xD800) << 10) + (in[i + 1] - 0xDC00);
			++i;
		}
		out.push_back((wchar_t)c);
	}
	return out;
}

}  // namespace

// ---- FDataStream: base ---------------------------------------------------------------------------
FDataStream::FDataStream(void)
	: m_bEnableCB(false), m_eEndianness(FEndian::ENDIAN_LITTLE), m_uiBytesRead(0), m_uiPosition(0)
{
}

FDataStream::~FDataStream(void) {}

unsigned int FDataStream::GetBytesUsed() const { return GetEOF(); }
unsigned int FDataStream::GetPosition() const { return m_uiPosition; }
void FDataStream::SetPosition(unsigned int position)
{
	Rewind();
	Skip(position);
}
void FDataStream::Truncate() {}
void FDataStream::Flush() {}
unsigned int FDataStream::GetEOF() const { return 0; }
unsigned int FDataStream::GetSizeLeft() const
{
	unsigned int eof = GetEOF(), pos = GetPosition();
	return eof > pos ? eof - pos : 0;
}
void FDataStream::CopyToMem(void*) {}
void FDataStream::RaiseException(std::exception& kException) const
{
	throw std::runtime_error(kException.what());
}

// ---- scalar writes -------------------------------------------------------------------------------
#define LEKMOD_WRITE_SCALAR(T)                                       \
	void FDataStream::Write(const T& value)                          \
	{                                                                \
		T v = value;                                                 \
		if (m_eEndianness != FEndian::GetEndianness()) swap_bytes(v); \
		WriteIt(sizeof(T), &v);                                      \
	}
#define LEKMOD_READ_SCALAR(T)                                        \
	void FDataStream::Read(T& value)                                 \
	{                                                                \
		ReadIt(sizeof(T), &value);                                   \
		if (m_eEndianness != FEndian::GetEndianness()) swap_bytes(value); \
	}

void FDataStream::Write(const bool& value) { WriteIt(1, &value); }
void FDataStream::Write(const unsigned char& value) { WriteIt(1, &value); }
void FDataStream::Write(const char& value) { WriteIt(1, &value); }
LEKMOD_WRITE_SCALAR(INT32)
LEKMOD_WRITE_SCALAR(UINT32)
LEKMOD_WRITE_SCALAR(UINT64)
LEKMOD_WRITE_SCALAR(INT64)
LEKMOD_WRITE_SCALAR(INT16)
LEKMOD_WRITE_SCALAR(UINT16)
LEKMOD_WRITE_SCALAR(float)
LEKMOD_WRITE_SCALAR(double)
void FDataStream::Write(const unsigned long& value) { Write((const UINT32&)(UINT32)value); }
void FDataStream::Write(const long& value) { Write((const INT32&)(INT32)value); }
void FDataStream::Write(const WCHAR& value) { Write((const UINT16&)(UINT16)value); }
void FDataStream::Write(const GUID& value)
{
	Write(value.Data1);
	Write(value.Data2);
	Write(value.Data3);
	WriteIt(8, value.Data4);
}
void FDataStream::Write(const YouMustImplementASerializeOperatorForThisTypeError&) {}

void FDataStream::Read(bool& value)
{
	unsigned char b = 0;
	ReadIt(1, &b);
	value = b != 0;
}
void FDataStream::Read(unsigned char& value) { ReadIt(1, &value); }
void FDataStream::Read(char& value) { ReadIt(1, &value); }
LEKMOD_READ_SCALAR(INT32)
LEKMOD_READ_SCALAR(UINT32)
LEKMOD_READ_SCALAR(UINT64)
LEKMOD_READ_SCALAR(INT64)
LEKMOD_READ_SCALAR(INT16)
LEKMOD_READ_SCALAR(UINT16)
LEKMOD_READ_SCALAR(float)
LEKMOD_READ_SCALAR(double)
void FDataStream::Read(unsigned long& value)
{
	UINT32 v = 0;
	Read(v);
	value = v;
}
void FDataStream::Read(long& value)
{
	INT32 v = 0;
	Read(v);
	value = v;
}
void FDataStream::Read(WCHAR& value)
{
	UINT16 v = 0;
	Read(v);
	value = (WCHAR)v;
}
void FDataStream::Read(GUID& value)
{
	Read(value.Data1);
	Read(value.Data2);
	Read(value.Data3);
	ReadIt(8, value.Data4);
}
void FDataStream::Read(YouMustImplementASerializeOperatorForThisTypeError&) {}

// ---- array writes --------------------------------------------------------------------------------
#define LEKMOD_ARRAY_IO(T)                                                       \
	void FDataStream::Write(int count, const T values[])                         \
	{                                                                            \
		for (int i = 0; i < count; ++i) Write(values[i]);                        \
	}                                                                            \
	void FDataStream::Read(int count, T values[])                                \
	{                                                                            \
		for (int i = 0; i < count; ++i) Read(values[i]);                         \
	}
LEKMOD_ARRAY_IO(INT32)
LEKMOD_ARRAY_IO(UINT32)
LEKMOD_ARRAY_IO(INT64)
LEKMOD_ARRAY_IO(UINT64)
LEKMOD_ARRAY_IO(unsigned long)
LEKMOD_ARRAY_IO(long)
LEKMOD_ARRAY_IO(INT16)
LEKMOD_ARRAY_IO(UINT16)
LEKMOD_ARRAY_IO(WCHAR)
LEKMOD_ARRAY_IO(float)
LEKMOD_ARRAY_IO(double)
LEKMOD_ARRAY_IO(GUID)

// ---- strings -------------------------------------------------------------------------------------
namespace {
INT32 checked_count(FDataStream& s, INT32 count, unsigned int unit)
{
	unsigned int left = s.GetSizeLeft();
	if (count < 0 || (unsigned int)count > left / unit)
	{
		std::runtime_error e("FDataStream ReadString error");
		s.RaiseException(e);
		return -1;
	}
	return count;
}
}  // namespace

unsigned int FDataStream::WriteString(const char* szName)
{
	INT32 n = szName ? (INT32)strlen(szName) : 0;
	Write(n);
	if (n > 0) WriteIt((unsigned int)n, szName);
	return (unsigned int)n;
}
unsigned int FDataStream::WriteString(const wchar* szName)
{
	std::vector<unsigned short> u;
	if (szName) append_utf16(u, szName, wcslen(szName));
	INT32 n = (INT32)u.size();
	Write(n);
	for (size_t i = 0; i < u.size(); ++i) Write(u[i]);
	return (unsigned int)n;
}
char* FDataStream::ReadString()
{
	std::string s;
	ReadString(s);
	char* p = new char[s.size() + 1];
	memcpy(p, s.c_str(), s.size() + 1);
	return p;
}
wchar* FDataStream::ReadWideString()
{
	std::wstring s;
	ReadString(s);
	wchar* p = new wchar[s.size() + 1];
	memcpy(p, s.c_str(), (s.size() + 1) * sizeof(wchar));
	return p;
}
unsigned int FDataStream::ReadString(char* szName)
{
	std::string s;
	ReadString(s);
	memcpy(szName, s.c_str(), s.size() + 1);
	return (unsigned int)s.size();
}
unsigned int FDataStream::ReadString(wchar* szName)
{
	std::wstring s;
	ReadString(s);
	memcpy(szName, s.c_str(), (s.size() + 1) * sizeof(wchar));
	return (unsigned int)s.size();
}

unsigned int FDataStream::WriteString(const std::string& szName)
{
	INT32 n = (INT32)szName.size();
	Write(n);
	if (n > 0) WriteIt((unsigned int)n, szName.data());
	return (unsigned int)n;
}
unsigned int FDataStream::WriteString(const std::wstring& szName)
{
	std::vector<unsigned short> u;
	append_utf16(u, szName.data(), szName.size());
	INT32 n = (INT32)u.size();
	Write(n);
	for (size_t i = 0; i < u.size(); ++i) Write(u[i]);
	return (unsigned int)n;
}
unsigned int FDataStream::WriteString(const FStringA& szName) { return WriteString(std::string(szName.c_str(), szName.GetLength())); }
unsigned int FDataStream::WriteString(const FStringW& szName) { return WriteString(std::wstring(szName.c_str(), szName.GetLength())); }
unsigned int FDataStream::WriteString(int count, const FStringA values[])
{
	unsigned int n = 0;
	for (int i = 0; i < count; ++i) n += WriteString(values[i]);
	return n;
}
unsigned int FDataStream::WriteString(int count, const FStringW values[])
{
	unsigned int n = 0;
	for (int i = 0; i < count; ++i) n += WriteString(values[i]);
	return n;
}
unsigned int FDataStream::WriteString(int count, const std::string values[])
{
	unsigned int n = 0;
	for (int i = 0; i < count; ++i) n += WriteString(values[i]);
	return n;
}
unsigned int FDataStream::WriteString(int count, const std::wstring values[])
{
	unsigned int n = 0;
	for (int i = 0; i < count; ++i) n += WriteString(values[i]);
	return n;
}

unsigned int FDataStream::ReadString(std::string& szName)
{
	INT32 n = 0;
	Read(n);
	szName.clear();
	n = checked_count(*this, n, 1);
	if (n > 0)
	{
		szName.resize((size_t)n);
		ReadIt((unsigned int)n, &szName[0]);
	}
	return n > 0 ? (unsigned int)n : 0;
}
unsigned int FDataStream::ReadString(std::wstring& szName)
{
	INT32 n = 0;
	Read(n);
	szName.clear();
	n = checked_count(*this, n, 2);
	if (n > 0)
	{
		std::vector<unsigned short> u((size_t)n);
		for (INT32 i = 0; i < n; ++i) Read(u[(size_t)i]);
		szName = from_utf16(u);
	}
	return n > 0 ? (unsigned int)n : 0;
}
unsigned int FDataStream::ReadString(FStringA& szName)
{
	std::string s;
	unsigned int n = ReadString(s);
	szName.Copy((int)s.size(), s.c_str());
	return n;
}
unsigned int FDataStream::ReadString(FStringW& szName)
{
	std::wstring s;
	unsigned int n = ReadString(s);
	szName.Copy((int)s.size(), s.c_str());
	return n;
}
unsigned int FDataStream::ReadString(int count, FStringA values[])
{
	unsigned int n = 0;
	for (int i = 0; i < count; ++i) n += ReadString(values[i]);
	return n;
}
unsigned int FDataStream::ReadString(int count, FStringW values[])
{
	unsigned int n = 0;
	for (int i = 0; i < count; ++i) n += ReadString(values[i]);
	return n;
}
unsigned int FDataStream::ReadString(int count, std::string values[])
{
	unsigned int n = 0;
	for (int i = 0; i < count; ++i) n += ReadString(values[i]);
	return n;
}
unsigned int FDataStream::ReadString(int count, std::wstring values[])
{
	unsigned int n = 0;
	for (int i = 0; i < count; ++i) n += ReadString(values[i]);
	return n;
}

void FDataStream::Write(const FStringA& v) { WriteString(v); }
void FDataStream::Write(int count, const FStringA values[]) { WriteString(count, values); }
void FDataStream::Write(const FStringW& v) { WriteString(v); }
void FDataStream::Write(int count, const FStringW values[]) { WriteString(count, values); }
void FDataStream::Write(const std::string& v) { WriteString(v); }
void FDataStream::Write(int count, const std::string values[]) { WriteString(count, values); }
void FDataStream::Write(const std::wstring& v) { WriteString(v); }
void FDataStream::Write(int count, const std::wstring values[]) { WriteString(count, values); }
void FDataStream::Read(FStringA& v) { ReadString(v); }
void FDataStream::Read(int count, FStringA values[]) { ReadString(count, values); }
void FDataStream::Read(FStringW& v) { ReadString(v); }
void FDataStream::Read(int count, FStringW values[]) { ReadString(count, values); }
void FDataStream::Read(std::string& v) { ReadString(v); }
void FDataStream::Read(int count, std::string values[]) { ReadString(count, values); }
void FDataStream::Read(std::wstring& v) { ReadString(v); }
void FDataStream::Read(int count, std::wstring values[]) { ReadString(count, values); }

// ---- stream to stream ----------------------------------------------------------------------------
FDataStream& operator<<(FDataStream& saveTo, FDataStream& readFrom)
{
	unsigned int n = readFrom.GetSizeLeft();
	saveTo << n;
	std::vector<char> buf(n ? n : 1);
	readFrom.ReadIt(n, &buf[0]);
	saveTo.WriteIt(n, &buf[0]);
	return saveTo;
}
FDataStream& operator>>(FDataStream& loadFrom, FDataStream& writeTo)
{
	unsigned int n = 0;
	loadFrom >> n;
	std::vector<char> buf(n ? n : 1);
	loadFrom.ReadIt(n, &buf[0]);
	writeTo.WriteIt(n, &buf[0]);
	return loadFrom;
}

// std::string as UINT32 size + chars (FStlContainerSerialization), the same bytes as WriteString.
FDataStream& operator<<(FDataStream& saveTo, const std::string& readFrom)
{
	UINT32 size = (UINT32)readFrom.size();
	saveTo << size;
	if (size) saveTo.WriteIt(size, readFrom.data());
	return saveTo;
}
FDataStream& operator>>(FDataStream& loadFrom, std::string& writeTo)
{
	UINT32 size = 0;
	loadFrom >> size;
	writeTo.clear();
	if (size > loadFrom.GetSizeLeft())
	{
		std::runtime_error e("FDataStream string size error");
		loadFrom.RaiseException(e);
		return loadFrom;
	}
	if (size)
	{
		writeTo.resize(size);
		loadFrom.ReadIt(size, &writeTo[0]);
	}
	return loadFrom;
}
FDataStream& operator<<(FDataStream& saveTo, const std::wstring& readFrom)
{
	std::vector<unsigned short> u;
	append_utf16(u, readFrom.data(), readFrom.size());
	UINT32 size = (UINT32)u.size();
	saveTo << size;
	for (size_t i = 0; i < u.size(); ++i) saveTo << u[i];
	return saveTo;
}
FDataStream& operator>>(FDataStream& loadFrom, std::wstring& writeTo)
{
	UINT32 size = 0;
	loadFrom >> size;
	if (size > loadFrom.GetSizeLeft() / 2)
	{
		std::runtime_error e("FDataStream wstring size error");
		loadFrom.RaiseException(e);
		return loadFrom;
	}
	std::vector<unsigned short> u(size);
	for (UINT32 i = 0; i < size; ++i) loadFrom >> u[i];
	writeTo = from_utf16(u);
	return loadFrom;
}

// ---- FMemoryStream -------------------------------------------------------------------------------
FMemoryStream::FMemoryStream(int size, const void* data, uint uMemoryPool, uint uMemoryTag, bool bRealloc, bool bReallocMin)
	: m_pStart(0), m_pData(0), m_pStop(0), m_pEndOfBuffer(0), m_bRealloc(bRealloc), m_bReallocMin(bReallocMin), m_uMemoryPool(uMemoryPool), m_uMemoryTag(uMemoryTag)
{
	Init(size, data, bRealloc, bReallocMin);
}
FMemoryStream::FMemoryStream(uint uMemoryPool, uint uMemoryTag, bool bRealloc, bool bReallocMin)
	: m_pStart(0), m_pData(0), m_pStop(0), m_pEndOfBuffer(0), m_bRealloc(bRealloc), m_bReallocMin(bReallocMin), m_uMemoryPool(uMemoryPool), m_uMemoryTag(uMemoryTag)
{
}
FMemoryStream::FMemoryStream(int size, const void* data, bool bRealloc, bool bReallocMin)
	: m_pStart(0), m_pData(0), m_pStop(0), m_pEndOfBuffer(0), m_bRealloc(bRealloc), m_bReallocMin(bReallocMin), m_uMemoryPool(2), m_uMemoryTag(0)
{
	Init(size, data, bRealloc, bReallocMin);
}
FMemoryStream::FMemoryStream(int size, const void* data, bool bRealloc)
	: m_pStart(0), m_pData(0), m_pStop(0), m_pEndOfBuffer(0), m_bRealloc(bRealloc), m_bReallocMin(false), m_uMemoryPool(2), m_uMemoryTag(0)
{
	Init(size, data, bRealloc, false);
}
FMemoryStream::FMemoryStream(int size, const void* data)
	: m_pStart(0), m_pData(0), m_pStop(0), m_pEndOfBuffer(0), m_bRealloc(false), m_bReallocMin(false), m_uMemoryPool(2), m_uMemoryTag(0)
{
	Init(size, data, false, false);
}
FMemoryStream::FMemoryStream(bool bRealloc, bool bReallocMin)
	: m_pStart(0), m_pData(0), m_pStop(0), m_pEndOfBuffer(0), m_bRealloc(bRealloc), m_bReallocMin(bReallocMin), m_uMemoryPool(2), m_uMemoryTag(0)
{
}
FMemoryStream::FMemoryStream(bool bRealloc)
	: m_pStart(0), m_pData(0), m_pStop(0), m_pEndOfBuffer(0), m_bRealloc(bRealloc), m_bReallocMin(false), m_uMemoryPool(2), m_uMemoryTag(0)
{
}
FMemoryStream::FMemoryStream()
	: m_pStart(0), m_pData(0), m_pStop(0), m_pEndOfBuffer(0), m_bRealloc(true), m_bReallocMin(false), m_uMemoryPool(2), m_uMemoryTag(0)
{
}
FMemoryStream::FMemoryStream(const FMemoryStream& o)
	: FDataStream(o), m_pStart(0), m_pData(0), m_pStop(0), m_pEndOfBuffer(0), m_bRealloc(true), m_bReallocMin(o.m_bReallocMin), m_uMemoryPool(o.m_uMemoryPool), m_uMemoryTag(o.m_uMemoryTag)
{
	Init((int)o.GetEOF(), o.m_pStart, true, o.m_bReallocMin);
	m_pData = m_pStart + (o.m_pData - o.m_pStart);
	m_uiPosition = (unsigned int)(m_pData - m_pStart);
}
FMemoryStream& FMemoryStream::operator=(const FMemoryStream& o)
{
	if (this != &o)
	{
		Init((int)o.GetEOF(), o.m_pStart, true, o.m_bReallocMin);
		m_pData = m_pStart + (o.m_pData - o.m_pStart);
		m_uiPosition = (unsigned int)(m_pData - m_pStart);
		m_eEndianness = o.m_eEndianness;
	}
	return *this;
}
FMemoryStream::~FMemoryStream(void)
{
	Uninit();
}

void FMemoryStream::Init(int size, const void* data, bool bRealloc) { Init(size, data, bRealloc, false); }

// A growable stream owns a copy; a fixed one wraps the caller's buffer.
void FMemoryStream::Init(int size, const void* data, bool bRealloc, bool bReallocMin)
{
	Uninit();
	m_bRealloc = bRealloc;
	m_bReallocMin = bReallocMin;
	if (size < 0) size = 0;
	if (bRealloc)
	{
		if (size > 0)
		{
			m_pStart = new char[size];
			if (data) memcpy(m_pStart, data, (size_t)size);
		}
	}
	else
	{
		m_pStart = static_cast<char*>(const_cast<void*>(data));
	}
	m_pData = m_pStart;
	m_pStop = m_pStart ? m_pStart + size : 0;
	m_pEndOfBuffer = m_pStop;
	m_uiPosition = 0;
}

void FMemoryStream::Uninit()
{
	if (m_bRealloc) delete[] m_pStart;
	m_pStart = m_pData = m_pStop = m_pEndOfBuffer = 0;
	m_uiPosition = 0;
}

void FMemoryStream::Grow(uint uiMinAdditionalSizeRequired)
{
	size_t cap = (size_t)(m_pEndOfBuffer - m_pStart);
	size_t used = (size_t)(m_pStop - m_pStart);
	size_t pos = (size_t)(m_pData - m_pStart);
	size_t need = used + uiMinAdditionalSizeRequired;
	size_t ncap = m_bReallocMin ? need : (cap ? cap * 2 : 1024);
	if (ncap < need) ncap = need;
	char* p = new char[ncap ? ncap : 1];
	if (used) memcpy(p, m_pStart, used);
	delete[] m_pStart;
	m_pStart = p;
	m_pData = p + pos;
	m_pStop = p + used;
	m_pEndOfBuffer = p + ncap;
}

unsigned int FMemoryStream::ReadIt(unsigned int byteCount, void* buffer)
{
	size_t left = (size_t)(m_pStop - m_pData);
	unsigned int n = byteCount <= left ? byteCount : (unsigned int)left;
	if (n) memcpy(buffer, m_pData, n);
	if (n < byteCount) memset((char*)buffer + n, 0, byteCount - n);
	m_pData += n;
	m_uiPosition = (unsigned int)(m_pData - m_pStart);
	m_uiBytesRead += n;
	return n;
}

unsigned int FMemoryStream::WriteIt(unsigned int byteCount, const void* buffer)
{
	if (!byteCount) return 0;
	if ((size_t)(m_pEndOfBuffer - m_pData) < byteCount)
	{
		if (!m_bRealloc) byteCount = (unsigned int)(m_pEndOfBuffer - m_pData);
		else Grow((uint)(byteCount - (m_pStop - m_pData > 0 ? m_pStop - m_pData : 0)));
	}
	if (byteCount) memcpy(m_pData, buffer, byteCount);
	m_pData += byteCount;
	if (m_pData > m_pStop) m_pStop = m_pData;
	m_uiPosition = (unsigned int)(m_pData - m_pStart);
	return byteCount;
}

void FMemoryStream::Skip(unsigned int deltaByteCount)
{
	size_t left = (size_t)(m_pStop - m_pData);
	m_pData += deltaByteCount <= left ? deltaByteCount : left;
	m_uiPosition = (unsigned int)(m_pData - m_pStart);
}
void FMemoryStream::Rewind()
{
	m_pData = m_pStart;
	m_uiPosition = 0;
}
bool FMemoryStream::AtEnd() const { return m_pData >= m_pStop; }
void FMemoryStream::FastFwd()
{
	m_pData = m_pStop;
	m_uiPosition = (unsigned int)(m_pData - m_pStart);
}
void FMemoryStream::Truncate() { m_pStop = m_pData; }
unsigned int FMemoryStream::GetEOF() const { return (unsigned int)(m_pStop - m_pStart); }
void FMemoryStream::CopyToMem(void* mem)
{
	if (GetEOF()) memcpy(mem, m_pStart, GetEOF());
}
char* FMemoryStream::GetBuf() { return m_pStart; }
const char* FMemoryStream::GetBuf() const { return m_pStart; }

// RLE variant: not used by the gamecore; plain pass-through (the stream is not RLE-compatible with the original).
uint FMemoryStreamRLE::WriteIt(uint byteCount, const void* buffer)
{
	return FMemoryStream::WriteIt(byteCount, buffer);
}
uint FMemoryStreamRLE::ReadIt(uint byteCount, void* buffer)
{
	return FMemoryStream::ReadIt(byteCount, buffer);
}

// ---- FFileStream: unused by the gamecore; every operation fails (there is no FIFile system) ----------
FFileStream::FFileStream() : m_pRef(0) {}
FFileStream::~FFileStream() {}
bool FFileStream::Open(const char*, FIFile::OpenMode, dword, uint) { return false; }
bool FFileStream::Open(const wchar_t*, FIFile::OpenMode, dword, uint) { return false; }
bool FFileStream::Close() { return true; }
unsigned int FFileStream::ReadIt(unsigned int, void*) { return 0; }
unsigned int FFileStream::WriteIt(unsigned int, const void*) { return 0; }
void FFileStream::Skip(unsigned int) {}
bool FFileStream::AtEnd() const { return true; }
void FFileStream::Rewind() {}
void FFileStream::FastFwd() {}
void FFileStream::Truncate() {}
void FFileStream::Flush() {}
unsigned int FFileStream::GetEOF() const { return 0; }
void FFileStream::RaiseException(std::exception& kException) const { throw std::runtime_error(kException.what()); }
FIFile* FFileStream::GetFile() { return m_pRef; }
void FFileStream::SetFile(FIFile* file) { m_pRef = file; }
