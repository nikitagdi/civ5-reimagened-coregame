// FStringA / FStringW out-of-line members (FireWorks), rewritten for the portable build.
// Layout as in FString.h: m_pszString points just past an FStringXData header; the shared empty string is
// FStringX_GetStringInitData(). Only the members the gamecore and the rest of headless/portable use are here.
// The FireWorks headers expect the gamecore precompiled-header environment.
#include "CvGameCoreDLLPCH.h"

#include <stdarg.h>
#include <string>
#include <vector>

#include "FireWorks/FFireTypes.h"
#include "FireWorks/FAssert.h"
#include "FireWorks/FString.h"

namespace {

// The empty-string singletons: header followed by the terminator.
struct InitDataA
{
	FStringA::FStringAData header;
	char terminator[4];
};
struct InitDataW
{
	FStringW::FStringWData header;
	wchar terminator[4];
};
InitDataA g_initA = {{0, 0, 0, 1}, {0, 0, 0, 0}};
InitDataW g_initW = {{0, 0, 0, 1}, {0, 0, 0, 0}};

template <class Data, class Char>
Data* alloc_data(size_t nChars)
{
	byte* raw = new byte[sizeof(Data) + (nChars + 1) * sizeof(Char)];
	Data* d = reinterpret_cast<Data*>(raw);
	d->m_iAllocLength = (int)nChars;
	d->m_iDataLength = 0;
	d->m_bLocked = 0;
	d->m_bFixed = 0;
	reinterpret_cast<Char*>(d + 1)[0] = 0;
	return d;
}

void utf8_append(std::string& out, unsigned int c)
{
	if (c < 0x80)
		out += (char)c;
	else if (c < 0x800)
	{
		out += (char)(0xC0 | (c >> 6));
		out += (char)(0x80 | (c & 0x3F));
	}
	else if (c < 0x10000)
	{
		out += (char)(0xE0 | (c >> 12));
		out += (char)(0x80 | ((c >> 6) & 0x3F));
		out += (char)(0x80 | (c & 0x3F));
	}
	else
	{
		out += (char)(0xF0 | (c >> 18));
		out += (char)(0x80 | ((c >> 12) & 0x3F));
		out += (char)(0x80 | ((c >> 6) & 0x3F));
		out += (char)(0x80 | (c & 0x3F));
	}
}

}  // namespace

FStringA::FStringAData* FStringA_GetStringInitData() { return &g_initA.header; }
FStringW::FStringWData* FStringW_GetStringInitData() { return &g_initW.header; }

// ---- FStringA ------------------------------------------------------------------------------------
void FStringA::AllocBuffer(size_t nChars)
{
	FStringAData* d = alloc_data<FStringAData, char>(nChars);
	m_pszString = d->StringData();
}

void FStringA::Reallocate(size_t iAllocSize)
{
	FStringAData* old = GetData();
	FStringAData* d = alloc_data<FStringAData, char>(iAllocSize);
	int keep = old->m_iDataLength < (int)iAllocSize ? old->m_iDataLength : (int)iAllocSize;
	if (keep > 0) memcpy(d->StringData(), m_pszString, (size_t)keep);
	d->m_iDataLength = keep;
	d->StringData()[keep] = 0;
	Release(old);
	m_pszString = d->StringData();
}

// Grows (never shrinks) the buffer and sets the length; the terminator is the caller's job.
void FStringA::SetLength(size_t iLength)
{
	FStringAData* d = GetData();
	if ((int)iLength > d->m_iAllocLength || d == FStringA_GetStringInitData())
	{
		if (iLength == 0) return;
		size_t cap = iLength < 16 ? 16 : iLength + iLength / 2;
		Reallocate(cap);
		d = GetData();
	}
	d->m_iDataLength = (int)iLength;
	m_pszString[iLength] = 0;
}

void FStringA::Copy(int iLength, LPCSTR lpszSource, int iFirst)
{
	if (iLength < 0 || iFirst < 0) return;
	if (m_pszString == lpszSource) return;
	if (iLength == 0 || lpszSource == NULL)
	{
		Empty();
		return;
	}
	std::string tmp(lpszSource + iFirst, (size_t)iLength);  // the source may live in this buffer
	SetLength((size_t)iLength);
	memcpy(m_pszString, tmp.data(), (size_t)iLength);
	m_pszString[GetLength()] = '\0';
}

void FStringA::FormatV(LPCSTR lpszFormat, va_list vaArgList)
{
	va_list cp;
	va_copy(cp, vaArgList);
	int n = vsnprintf(NULL, 0, lpszFormat, cp);
	va_end(cp);
	if (n <= 0)
	{
		Empty();
		return;
	}
	std::vector<char> buf((size_t)n + 1);
	vsnprintf(&buf[0], buf.size(), lpszFormat, vaArgList);
	Copy(n, &buf[0]);
}

void __cdecl FStringA::Format(LPCSTR lpszFormat, ...)
{
	va_list ap;
	va_start(ap, lpszFormat);
	FormatV(lpszFormat, ap);
	va_end(ap);
}

int FStringA::Replace(char chOld, char chNew)
{
	int iCount = 0;
	if (chOld != chNew)
	{
		for (char *pch = m_pszString, *pchEnd = m_pszString + GetLength(); pch < pchEnd; ++pch)
		{
			if (*pch == chOld)
			{
				*pch = chNew;
				++iCount;
			}
		}
	}
	return iCount;
}

void FStringA::SetAt(int iIndex, int iLength, LPCSTR lpszSource, int iFirst)
{
	if (iLength <= 0 || iFirst < 0) return;
	if ((unsigned int)iIndex > (unsigned int)GetLength() || lpszSource == NULL) return;
	std::string tmp(lpszSource + iFirst, (size_t)iLength);
	if (GetLength() < iLength + iIndex) SetLength((size_t)(iLength + iIndex));
	memcpy(m_pszString + iIndex, tmp.data(), (size_t)iLength);
}

void FStringA::CopyToUTF8(LPCSTR pszSource, FStringA* pkOutput)
{
	if (pkOutput) pkOutput->Copy(SafeStrlen(pszSource), pszSource);  // narrow strings are UTF-8 here
}

// ---- FStringW ------------------------------------------------------------------------------------
void FStringW::AllocBuffer(size_t nChars)
{
	FStringWData* d = alloc_data<FStringWData, wchar>(nChars);
	m_pszString = d->StringData();
}

void FStringW::Reallocate(size_t iAllocSize)
{
	FStringWData* old = GetData();
	FStringWData* d = alloc_data<FStringWData, wchar>(iAllocSize);
	int keep = old->m_iDataLength < (int)iAllocSize ? old->m_iDataLength : (int)iAllocSize;
	if (keep > 0) memcpy(d->StringData(), m_pszString, (size_t)keep * sizeof(wchar));
	d->m_iDataLength = keep;
	d->StringData()[keep] = 0;
	Release(old);
	m_pszString = d->StringData();
}

void FStringW::SetLength(size_t iLength)
{
	FStringWData* d = GetData();
	if ((int)iLength > d->m_iAllocLength || d == FStringW_GetStringInitData())
	{
		if (iLength == 0) return;
		size_t cap = iLength < 16 ? 16 : iLength + iLength / 2;
		Reallocate(cap);
		d = GetData();
	}
	d->m_iDataLength = (int)iLength;
	m_pszString[iLength] = 0;
}

void FStringW::Copy(int iLength, LPCWSTR lpszSource, int iFirst)
{
	if (iLength < 0 || iFirst < 0) return;
	if (m_pszString == lpszSource) return;
	if (iLength == 0 || lpszSource == NULL)
	{
		Empty();
		return;
	}
	std::wstring tmp(lpszSource + iFirst, (size_t)iLength);
	SetLength((size_t)iLength);
	memcpy(m_pszString, tmp.data(), (size_t)iLength * sizeof(wchar));
	m_pszString[GetLength()] = L'\0';
}

void FStringW::CopyToUTF8(LPCWSTR pszSource, FStringA* pkOutput)
{
	if (!pkOutput) return;
	std::string out;
	if (pszSource)
		for (const wchar* p = pszSource; *p; ++p) utf8_append(out, (unsigned int)*p);
	pkOutput->Copy((int)out.size(), out.c_str());
}

void FStringW::CopyToUTF8(FStringA* pkOutput) const
{
	if (pkOutput == NULL) return;
	if (GetLength() > 0)
	{
		CopyToUTF8(m_pszString, pkOutput);
		return;
	}
	if (pkOutput->GetLength() != 0) pkOutput->Empty();
}

void FStringW::ExtractFileName(FStringW& szDest, wchar chPathDelimiter) const
{
	int len = GetLength();
	int start = 0;
	for (int i = len - 1; i >= 0; --i)
	{
		if (m_pszString[i] == chPathDelimiter || m_pszString[i] == L'/' || m_pszString[i] == L'\\')
		{
			start = i + 1;
			break;
		}
	}
	szDest.Copy(len - start, m_pszString, start);
}

FStringW FStringW::ExtractFileName(wchar chPathDelimiter) const
{
	FStringW s;
	ExtractFileName(s, chPathDelimiter);
	return s;
}

void FStringW::ExtractFileTitle(FStringW& szDest, wchar chPathDelimiter) const
{
	FStringW sFileName;
	ExtractFileName(sFileName, chPathDelimiter);
	int len = sFileName.GetLength();
	int dot = -1;
	for (int i = len - 1; i >= 0; --i)
	{
		if (sFileName.c_str()[i] == L'.')
		{
			dot = i;
			break;
		}
	}
	szDest.Copy(dot >= 0 ? dot : len, sFileName.c_str());
}
