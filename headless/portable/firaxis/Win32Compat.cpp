// Out-of-line parts of headless/portable/include/windows.h (code pages, stdio-backed file handles, GUIDs)
// and CvStringUtils (CvGameCoreDLLUtil).
// The FireWorks headers expect the gamecore precompiled-header environment.
#include "CvGameCoreDLLPCH.h"

#include <stdio.h>
#include <sys/stat.h>
#include <string>

#include "CvStringUtils.h"
#include "../runtime/rt_fs.h"

using lekmod_portable::rt_fs::IsMemPath;
namespace rtfs = lekmod_portable::rt_fs;

namespace {

// UTF-8 -> code points; an invalid or truncated sequence yields U+FFFD and resumes at the next byte.
void decode_utf8(const char* s, size_t n, std::wstring& out)
{
	size_t i = 0;
	while (i < n)
	{
		unsigned char c = (unsigned char)s[i];
		unsigned int cp;
		size_t extra;
		if (c < 0x80) { cp = c; extra = 0; }
		else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
		else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
		else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
		else { out.push_back((wchar_t)0xFFFD); ++i; continue; }
		bool ok = i + extra < n;
		for (size_t k = 1; ok && k <= extra; ++k)
		{
			unsigned char cc = (unsigned char)s[i + k];
			if ((cc & 0xC0) != 0x80) ok = false;
			else cp = (cp << 6) | (cc & 0x3F);
		}
		if (!ok)
		{
			out.push_back((wchar_t)0xFFFD);
			++i;
			continue;
		}
		out.push_back((wchar_t)cp);
		i += extra + 1;
	}
}

void encode_utf8(const wchar_t* s, size_t n, std::string& out)
{
	for (size_t i = 0; i < n; ++i)
	{
		unsigned int c = (unsigned int)s[i];
		if (c < 0x80) out += (char)c;
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
}

std::string narrow(LPCWSTR w)
{
	std::string s;
	if (w) encode_utf8(w, wcslen(w), s);
	return s;
}

}  // namespace

int MultiByteToWideChar(UINT, DWORD, LPCSTR src, int srcLen, LPWSTR dst, int dstLen)
{
	if (!src) return 0;
	size_t n = srcLen < 0 ? strlen(src) + 1 : (size_t)srcLen;  // -1 includes the terminator
	std::wstring w;
	decode_utf8(src, n, w);
	if (dstLen == 0) return (int)w.size();
	if ((int)w.size() > dstLen)
	{
		SetLastError(122);  // ERROR_INSUFFICIENT_BUFFER
		return 0;
	}
	if (!w.empty()) memcpy(dst, w.data(), w.size() * sizeof(wchar_t));
	return (int)w.size();
}

int WideCharToMultiByte(UINT, DWORD, LPCWSTR src, int srcLen, LPSTR dst, int dstLen, LPCSTR, LPBOOL usedDefault)
{
	if (usedDefault) *usedDefault = FALSE;
	if (!src) return 0;
	size_t n = srcLen < 0 ? wcslen(src) + 1 : (size_t)srcLen;
	std::string s;
	encode_utf8(src, n, s);
	if (dstLen == 0) return (int)s.size();
	if ((int)s.size() > dstLen)
	{
		SetLastError(122);
		return 0;
	}
	if (!s.empty()) memcpy(dst, s.data(), s.size());
	return (int)s.size();
}

// ---- files ---------------------------------------------------------------------------------------
HANDLE CreateFileA(LPCSTR name, DWORD access, DWORD, LPSECURITY_ATTRIBUTES, DWORD disposition, DWORD, HANDLE)
{
	if (!name) return INVALID_HANDLE_VALUE;
	if (IsMemPath(name))  // ./Civ5SavedGameDatabase.db lives in rt_fs memory
	{
		unsigned err = 0;
		void* h = rtfs::Win32Open(name, disposition, &err);
		if (!h) SetLastError(err);
		return h ? (HANDLE)h : INVALID_HANDLE_VALUE;
	}
	struct stat st;
	bool exists = stat(name, &st) == 0;
	const char* mode = "rb";
	bool write = (access & GENERIC_WRITE) != 0;
	switch (disposition)
	{
	case CREATE_NEW:
		if (exists)
		{
			SetLastError(ERROR_ALREADY_EXISTS);
			return INVALID_HANDLE_VALUE;
		}
		mode = "w+b";
		break;
	case CREATE_ALWAYS:
	case TRUNCATE_EXISTING:
		mode = "w+b";
		break;
	case OPEN_ALWAYS:
		mode = exists ? (write ? "r+b" : "rb") : "w+b";
		break;
	default:  // OPEN_EXISTING
		if (!exists)
		{
			SetLastError(ERROR_FILE_NOT_FOUND);
			return INVALID_HANDLE_VALUE;
		}
		mode = write ? "r+b" : "rb";
		break;
	}
	FILE* f = fopen(name, mode);
	if (!f)
	{
		SetLastError(ERROR_ACCESS_DENIED);
		return INVALID_HANDLE_VALUE;
	}
	return (HANDLE)f;
}

HANDLE CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags, HANDLE templ)
{
	return CreateFileA(narrow(name).c_str(), access, share, sa, disposition, flags, templ);
}

BOOL ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD read, LPOVERLAPPED)
{
	if (h == INVALID_HANDLE_VALUE || !h) return FALSE;
	if (rtfs::Win32IsHandle(h)) return rtfs::Win32Read(h, buf, n, (unsigned*)read);
	size_t got = fread(buf, 1, n, (FILE*)h);
	if (read) *read = (DWORD)got;
	return !ferror((FILE*)h);
}

BOOL WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD written, LPOVERLAPPED)
{
	if (h == INVALID_HANDLE_VALUE || !h) return FALSE;
	if (rtfs::Win32IsHandle(h)) return rtfs::Win32Write(h, buf, n, (unsigned*)written);
	size_t put = fwrite(buf, 1, n, (FILE*)h);
	if (written) *written = (DWORD)put;
	return put == n;
}

DWORD GetFileSize(HANDLE h, LPDWORD high)
{
	if (h == INVALID_HANDLE_VALUE || !h) return INVALID_FILE_SIZE;
	if (rtfs::Win32IsHandle(h))
	{
		if (high) *high = 0;
		return rtfs::Win32Size(h);
	}
	FILE* f = (FILE*)h;
	long pos = ftell(f);
	fseek(f, 0, SEEK_END);
	long end = ftell(f);
	fseek(f, pos, SEEK_SET);
	if (high) *high = 0;
	return end < 0 ? INVALID_FILE_SIZE : (DWORD)end;
}

DWORD SetFilePointer(HANDLE h, LONG dist, PLONG distHigh, DWORD method)
{
	if (h == INVALID_HANDLE_VALUE || !h) return INVALID_FILE_SIZE;
	if (distHigh) *distHigh = 0;
	if (rtfs::Win32IsHandle(h)) return rtfs::Win32Seek(h, dist, method == FILE_END ? 2 : (method == FILE_CURRENT ? 1 : 0));
	int whence = method == FILE_END ? SEEK_END : (method == FILE_CURRENT ? SEEK_CUR : SEEK_SET);
	if (fseek((FILE*)h, dist, whence) != 0) return INVALID_FILE_SIZE;
	return (DWORD)ftell((FILE*)h);
}

BOOL CloseHandle(HANDLE h)
{
	if (h == INVALID_HANDLE_VALUE || !h) return FALSE;
	if (rtfs::Win32IsHandle(h)) return rtfs::Win32Close(h);
	return fclose((FILE*)h) == 0;
}

BOOL DeleteFileA(LPCSTR name)
{
	if (name && IsMemPath(name))
	{
		unsigned err = 0;
		if (rtfs::Win32Delete(name, &err)) return TRUE;
		SetLastError(err);
		return FALSE;
	}
	if (name && remove(name) == 0) return TRUE;
	SetLastError(ERROR_FILE_NOT_FOUND);
	return FALSE;
}
BOOL DeleteFileW(LPCWSTR name) { return DeleteFileA(narrow(name).c_str()); }

DWORD GetFileAttributesA(LPCSTR name)
{
	if (name && IsMemPath(name))
	{
		unsigned long long size;
		if (rtfs::Win32Stat(name, &size)) return FILE_ATTRIBUTE_NORMAL;
		SetLastError(ERROR_FILE_NOT_FOUND);
		return INVALID_FILE_ATTRIBUTES;
	}
	struct stat st;
	if (!name || stat(name, &st) != 0)
	{
		SetLastError(ERROR_FILE_NOT_FOUND);
		return INVALID_FILE_ATTRIBUTES;
	}
	return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}
DWORD GetFileAttributesW(LPCWSTR name) { return GetFileAttributesA(narrow(name).c_str()); }

BOOL GetFileAttributesExA(LPCSTR name, GET_FILEEX_INFO_LEVELS, LPVOID info)
{
	if (name && IsMemPath(name))
	{
		unsigned long long size;
		if (!rtfs::Win32Stat(name, &size))
		{
			SetLastError(ERROR_FILE_NOT_FOUND);
			return FALSE;
		}
		WIN32_FILE_ATTRIBUTE_DATA* d = (WIN32_FILE_ATTRIBUTE_DATA*)info;
		memset(d, 0, sizeof(*d));
		d->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
		d->nFileSizeLow = (DWORD)size;
		d->nFileSizeHigh = (DWORD)(size >> 32);
		return TRUE;
	}
	struct stat st;
	if (!name || stat(name, &st) != 0)
	{
		SetLastError(ERROR_FILE_NOT_FOUND);
		return FALSE;
	}
	WIN32_FILE_ATTRIBUTE_DATA* d = (WIN32_FILE_ATTRIBUTE_DATA*)info;
	memset(d, 0, sizeof(*d));
	d->dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
	d->nFileSizeLow = (DWORD)st.st_size;
	d->nFileSizeHigh = (DWORD)((unsigned long long)st.st_size >> 32);
	return TRUE;
}
BOOL GetFileAttributesExW(LPCWSTR name, GET_FILEEX_INFO_LEVELS level, LPVOID info)
{
	return GetFileAttributesExA(narrow(name).c_str(), level, info);
}

BOOL CreateDirectoryA(LPCSTR name, LPSECURITY_ATTRIBUTES)
{
	return name && mkdir(name, 0777) == 0;
}

// ---- GUIDs ---------------------------------------------------------------------------------------
// Deterministic sequence (the map GUID only identifies a game; it must not differ between replicas
// that construct the same map, and Win32's random GUIDs are not reproducible anyway).
HRESULT CoCreateGuid(GUID* guid)
{
	static unsigned int s_counter = 0;
	if (!guid) return E_FAIL;
	++s_counter;
	unsigned int x = 0x4c454b4du ^ (s_counter * 2654435761u);
	guid->Data1 = x;
	guid->Data2 = (unsigned short)(s_counter >> 16);
	guid->Data3 = (unsigned short)(0x4000 | (s_counter & 0x0fff));
	for (int i = 0; i < 8; ++i)
	{
		x = x * 1103515245u + 12345u;
		guid->Data4[i] = (unsigned char)(x >> 16);
	}
	guid->Data4[0] = (unsigned char)(0x80 | (guid->Data4[0] & 0x3f));
	return S_OK;
}

// ---- CvStringUtils -------------------------------------------------------------------------------
namespace CvStringUtils {
std::wstring FromACPToUTF16(const std::string& s)
{
	std::wstring w;
	decode_utf8(s.data(), s.size(), w);
	return w;
}
std::wstring FromUTF8ToUTF16(const std::string& s) { return FromACPToUTF16(s); }
std::string FromUTF16ToACP(const std::wstring& w)
{
	std::string s;
	encode_utf8(w.data(), w.size(), s);
	return s;
}
std::string FromUTF16ToUTF8(const std::wstring& w) { return FromUTF16ToACP(w); }
}  // namespace CvStringUtils
