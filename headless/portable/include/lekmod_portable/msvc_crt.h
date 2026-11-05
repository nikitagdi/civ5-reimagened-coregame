// MSVC CRT extensions used by the gamecore and the Firaxis headers, mapped onto the C library.
// Truncation follows MSVC: the *_s functions always terminate; _snprintf/_vsnprintf return -1 on overflow.
#ifndef LEKMOD_PORTABLE_MSVC_CRT_H
#define LEKMOD_PORTABLE_MSVC_CRT_H

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>
#if !defined(__wasi__)
#include <alloca.h>
#endif

typedef int errno_t;
#ifndef _TRUNCATE
#define _TRUNCATE ((size_t)-1)
#endif
#ifndef _countof
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#endif
#ifndef _alloca
#define _alloca(n) __builtin_alloca(n)
#endif
#define __debugbreak() ((void)0)
#define __assume(x) ((void)0)
#define _ReturnAddress() __builtin_return_address(0)
#define _MAX_PATH 260
#define _MAX_FNAME 256
#define _MAX_EXT 256
#define _MAX_DIR 256
#define _MAX_DRIVE 3

// ---- formatted output --------------------------------------------------------------------------
inline int lekmod_vsnprintf_s(char* buf, size_t size, const char* fmt, va_list ap)
{
	if (!buf || size == 0) return -1;
	int n = vsnprintf(buf, size, fmt, ap);
	if (n < 0 || (size_t)n >= size)
	{
		buf[size - 1] = 0;
		return -1;
	}
	return n;
}
inline int vsprintf_s(char* buf, size_t size, const char* fmt, va_list ap) { return lekmod_vsnprintf_s(buf, size, fmt, ap); }
template <size_t N>
inline int vsprintf_s(char (&buf)[N], const char* fmt, va_list ap) { return lekmod_vsnprintf_s(buf, N, fmt, ap); }
inline int sprintf_s(char* buf, size_t size, const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = lekmod_vsnprintf_s(buf, size, fmt, ap);
	va_end(ap);
	return n;
}
template <size_t N>
inline int sprintf_s(char (&buf)[N], const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = lekmod_vsnprintf_s(buf, N, fmt, ap);
	va_end(ap);
	return n;
}
inline int _vsnprintf(char* buf, size_t count, const char* fmt, va_list ap)
{
	int n = vsnprintf(buf, count, fmt, ap);
	return (n < 0 || (size_t)n >= count) ? -1 : n;  // MSVC: no terminator written on overflow is allowed; we still terminate
}
inline int _snprintf(char* buf, size_t count, const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = _vsnprintf(buf, count, fmt, ap);
	va_end(ap);
	return n;
}
inline int _vsnprintf_s(char* buf, size_t size, size_t count, const char* fmt, va_list ap)
{
	size_t limit = (count == _TRUNCATE || count + 1 > size) ? size : count + 1;
	return lekmod_vsnprintf_s(buf, limit, fmt, ap);
}
inline int vsnprintf_s(char* buf, size_t size, size_t count, const char* fmt, va_list ap) { return _vsnprintf_s(buf, size, count, fmt, ap); }
inline int _snprintf_s(char* buf, size_t size, size_t count, const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = _vsnprintf_s(buf, size, count, fmt, ap);
	va_end(ap);
	return n;
}
template <size_t N>
inline int _snprintf_s(char (&buf)[N], size_t count, const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = _vsnprintf_s(buf, N, count, fmt, ap);
	va_end(ap);
	return n;
}
inline int _vscprintf(const char* fmt, va_list ap)
{
	va_list cp;
	va_copy(cp, ap);
	int n = vsnprintf(0, 0, fmt, cp);
	va_end(cp);
	return n;
}
inline int _scprintf(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(0, 0, fmt, ap);
	va_end(ap);
	return n;
}
inline int swprintf_s(wchar_t* buf, size_t size, const wchar_t* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vswprintf(buf, size, fmt, ap);
	va_end(ap);
	if (n < 0 && size) buf[size - 1] = 0;
	return n;
}
template <size_t N>
inline int swprintf_s(wchar_t (&buf)[N], const wchar_t* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vswprintf(buf, N, fmt, ap);
	va_end(ap);
	if (n < 0) buf[N - 1] = 0;
	return n;
}
inline int fprintf_s(FILE* f, const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vfprintf(f, fmt, ap);
	va_end(ap);
	return n;
}
inline int printf_s(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vprintf(fmt, ap);
	va_end(ap);
	return n;
}
inline int _vsnwprintf(wchar_t* buf, size_t count, const wchar_t* fmt, va_list ap) { return vswprintf(buf, count, fmt, ap); }
inline int _snwprintf(wchar_t* buf, size_t count, const wchar_t* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vswprintf(buf, count, fmt, ap);
	va_end(ap);
	return n;
}
inline int vswprintf_s(wchar_t* buf, size_t size, const wchar_t* fmt, va_list ap) { return vswprintf(buf, size, fmt, ap); }

// ---- string copy -------------------------------------------------------------------------------
inline errno_t strncpy_s(char* dst, size_t size, const char* src, size_t count)
{
	if (!dst || size == 0) return EINVAL;
	if (!src)
	{
		dst[0] = 0;
		return EINVAL;
	}
	size_t n = strlen(src);
	if (count != _TRUNCATE && count < n) n = count;
	if (n >= size)
	{
		if (count == _TRUNCATE)
		{
			n = size - 1;
		}
		else
		{
			dst[0] = 0;
			return ERANGE;
		}
	}
	memcpy(dst, src, n);
	dst[n] = 0;
	return 0;
}
inline errno_t strcpy_s(char* dst, size_t size, const char* src) { return strncpy_s(dst, size, src, _TRUNCATE); }
template <size_t N>
inline errno_t strcpy_s(char (&dst)[N], const char* src) { return strncpy_s(dst, N, src, _TRUNCATE); }
template <size_t N>
inline errno_t strncpy_s(char (&dst)[N], const char* src, size_t count) { return strncpy_s(dst, N, src, count); }
inline errno_t strcat_s(char* dst, size_t size, const char* src)
{
	size_t len = strnlen(dst, size);
	if (len >= size) return EINVAL;
	return strncpy_s(dst + len, size - len, src, _TRUNCATE);
}
template <size_t N>
inline errno_t strcat_s(char (&dst)[N], const char* src) { return strcat_s(dst, N, src); }
inline errno_t strncat_s(char* dst, size_t size, const char* src, size_t count)
{
	size_t len = strnlen(dst, size);
	if (len >= size) return EINVAL;
	return strncpy_s(dst + len, size - len, src, count);
}
inline errno_t wcsncpy_s(wchar_t* dst, size_t size, const wchar_t* src, size_t count)
{
	if (!dst || size == 0) return EINVAL;
	size_t n = src ? wcslen(src) : 0;
	if (count != _TRUNCATE && count < n) n = count;
	if (n >= size) n = size - 1;
	if (n) memcpy(dst, src, n * sizeof(wchar_t));
	dst[n] = 0;
	return 0;
}
inline errno_t wcscpy_s(wchar_t* dst, size_t size, const wchar_t* src) { return wcsncpy_s(dst, size, src, _TRUNCATE); }
template <size_t N>
inline errno_t wcscpy_s(wchar_t (&dst)[N], const wchar_t* src) { return wcsncpy_s(dst, N, src, _TRUNCATE); }
inline errno_t wcscat_s(wchar_t* dst, size_t size, const wchar_t* src)
{
	size_t len = wcslen(dst);
	if (len >= size) return EINVAL;
	return wcsncpy_s(dst + len, size - len, src, _TRUNCATE);
}
inline errno_t memcpy_s(void* dst, size_t size, const void* src, size_t count)
{
	if (count > size) return ERANGE;
	memcpy(dst, src, count);
	return 0;
}
inline char* strtok_s(char* s, const char* delim, char** ctx) { return strtok_r(s, delim, ctx); }
inline wchar_t* wcstok_s(wchar_t* s, const wchar_t* delim, wchar_t** ctx) { return wcstok(s, delim, ctx); }

// ---- case-insensitive compare / case conversion ------------------------------------------------
inline int _stricmp(const char* a, const char* b) { return strcasecmp(a, b); }
inline int _strnicmp(const char* a, const char* b, size_t n) { return strncasecmp(a, b, n); }
inline int stricmp(const char* a, const char* b) { return strcasecmp(a, b); }
inline int strnicmp(const char* a, const char* b, size_t n) { return strncasecmp(a, b, n); }
inline int _strcmpi(const char* a, const char* b) { return strcasecmp(a, b); }
inline int _wcsicmp(const wchar_t* a, const wchar_t* b)
{
	for (;; ++a, ++b)
	{
		wint_t ca = towlower((wint_t)*a), cb = towlower((wint_t)*b);
		if (ca != cb) return ca < cb ? -1 : 1;
		if (!ca) return 0;
	}
}
inline int _wcsnicmp(const wchar_t* a, const wchar_t* b, size_t n)
{
	for (; n; --n, ++a, ++b)
	{
		wint_t ca = towlower((wint_t)*a), cb = towlower((wint_t)*b);
		if (ca != cb) return ca < cb ? -1 : 1;
		if (!ca) return 0;
	}
	return 0;
}
inline char* _strdup(const char* s) { return strdup(s); }
inline wchar_t* _wcsdup(const wchar_t* s)
{
	size_t n = wcslen(s) + 1;
	wchar_t* d = (wchar_t*)malloc(n * sizeof(wchar_t));
	if (d) memcpy(d, s, n * sizeof(wchar_t));
	return d;
}
inline char* _strlwr(char* s)
{
	for (char* p = s; *p; ++p) *p = (char)tolower((unsigned char)*p);
	return s;
}
inline char* _strupr(char* s)
{
	for (char* p = s; *p; ++p) *p = (char)toupper((unsigned char)*p);
	return s;
}
inline errno_t _strlwr_s(char* s, size_t) { _strlwr(s); return 0; }
inline errno_t _strupr_s(char* s, size_t) { _strupr(s); return 0; }
inline wchar_t* _wcslwr(wchar_t* s)
{
	for (wchar_t* p = s; *p; ++p) *p = (wchar_t)towlower((wint_t)*p);
	return s;
}
inline wchar_t* _wcsupr(wchar_t* s)
{
	for (wchar_t* p = s; *p; ++p) *p = (wchar_t)towupper((wint_t)*p);
	return s;
}

inline char* _strnset(char* s, int c, size_t n)
{
	for (size_t i = 0; i < n && s[i]; ++i) s[i] = (char)c;
	return s;
}
inline wchar_t* _wcsnset(wchar_t* s, wchar_t c, size_t n)
{
	for (size_t i = 0; i < n && s[i]; ++i) s[i] = c;
	return s;
}
inline char* _strset(char* s, int c)
{
	for (char* p = s; *p; ++p) *p = (char)c;
	return s;
}
inline char* _strrev(char* s)
{
	size_t n = strlen(s);
	for (size_t i = 0; i < n / 2; ++i)
	{
		char t = s[i];
		s[i] = s[n - 1 - i];
		s[n - 1 - i] = t;
	}
	return s;
}
inline wchar_t* _wcsrev(wchar_t* s)
{
	size_t n = wcslen(s);
	for (size_t i = 0; i < n / 2; ++i)
	{
		wchar_t t = s[i];
		s[i] = s[n - 1 - i];
		s[n - 1 - i] = t;
	}
	return s;
}
// Locale-aware MSVC collation is reduced to the C locale.
inline int _stricoll(const char* a, const char* b) { return strcasecmp(a, b); }
inline int _strcoll(const char* a, const char* b) { return strcmp(a, b); }
inline int _wcsicoll(const wchar_t* a, const wchar_t* b) { return _wcsicmp(a, b); }

// ---- integer <-> text --------------------------------------------------------------------------
inline char* lekmod_u64toa(unsigned long long v, char* buf, int radix, bool neg)
{
	char tmp[72];
	int i = 0;
	if (radix < 2 || radix > 36) radix = 10;
	do
	{
		int d = (int)(v % (unsigned)radix);
		tmp[i++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
		v /= (unsigned)radix;
	} while (v);
	char* p = buf;
	if (neg) *p++ = '-';
	while (i) *p++ = tmp[--i];
	*p = 0;
	return buf;
}
inline char* _i64toa(long long v, char* buf, int radix)
{
	bool neg = radix == 10 && v < 0;
	unsigned long long u = neg ? 0ull - (unsigned long long)v : (unsigned long long)v;
	return lekmod_u64toa(u, buf, radix, neg);
}
inline char* _ui64toa(unsigned long long v, char* buf, int radix) { return lekmod_u64toa(v, buf, radix, false); }
// MSVC formats negative numbers in non-decimal radix as their 32-bit two's complement.
inline char* _itoa(int v, char* buf, int radix)
{
	if (radix == 10) return _i64toa(v, buf, 10);
	return lekmod_u64toa((unsigned int)v, buf, radix, false);
}
inline char* _ltoa(long v, char* buf, int radix) { return _itoa((int)v, buf, radix); }
inline char* _ultoa(unsigned long v, char* buf, int radix) { return lekmod_u64toa((unsigned int)v, buf, radix, false); }
inline errno_t _itoa_s(int v, char* buf, size_t size, int radix)
{
	char tmp[72];
	_itoa(v, tmp, radix);
	return strncpy_s(buf, size, tmp, _TRUNCATE);
}
template <size_t N>
inline errno_t _itoa_s(int v, char (&buf)[N], int radix) { return _itoa_s(v, buf, N, radix); }
inline errno_t _i64toa_s(long long v, char* buf, size_t size, int radix)
{
	char tmp[72];
	_i64toa(v, tmp, radix);
	return strncpy_s(buf, size, tmp, _TRUNCATE);
}
inline errno_t _ui64toa_s(unsigned long long v, char* buf, size_t size, int radix)
{
	char tmp[72];
	_ui64toa(v, tmp, radix);
	return strncpy_s(buf, size, tmp, _TRUNCATE);
}
inline long long _atoi64(const char* s) { return strtoll(s, 0, 10); }
inline long long _strtoi64(const char* s, char** e, int b) { return strtoll(s, e, b); }
inline unsigned long long _strtoui64(const char* s, char** e, int b) { return strtoull(s, e, b); }
inline int _wtoi(const wchar_t* s) { return (int)wcstol(s, 0, 10); }
inline long _wtol(const wchar_t* s) { return wcstol(s, 0, 10); }
inline double _wtof(const wchar_t* s) { return wcstod(s, 0); }

// ---- math --------------------------------------------------------------------------------------
inline int _isnan(double x) { return x != x; }
inline int _finite(double x) { return __builtin_isfinite(x); }
inline double _copysign(double a, double b) { return copysign(a, b); }
inline double _hypot(double a, double b) { return hypot(a, b); }
inline float _hypotf(float a, float b) { return hypotf(a, b); }
#define _MCW_PC 0x00030000
#define _MCW_RC 0x00000300
#define _PC_24 0x00020000
#define _PC_53 0x00010000
#define _PC_64 0x00000000
#define _RC_NEAR 0x00000000
inline errno_t _controlfp_s(unsigned int* current, unsigned int, unsigned int)
{
	if (current) *current = _PC_53 | _RC_NEAR;  // SSE/WASM doubles: always 53-bit, round to nearest
	return 0;
}
inline unsigned int _controlfp(unsigned int, unsigned int) { return _PC_53 | _RC_NEAR; }

// ---- memory ------------------------------------------------------------------------------------
inline void* _aligned_malloc(size_t size, size_t alignment)
{
	if (alignment < sizeof(void*)) alignment = sizeof(void*);
	void* p = 0;
	if (posix_memalign(&p, alignment, size ? size : 1) != 0) return 0;
	return p;
}
inline void _aligned_free(void* p) { free(p); }
inline size_t _msize(void*) { return 0; }
#define _malloca(n) malloc(n)
#define _freea(p) free(p)

// ---- time --------------------------------------------------------------------------------------
inline errno_t localtime_s(struct tm* out, const time_t* t) { return localtime_r(t, out) ? 0 : EINVAL; }
inline errno_t gmtime_s(struct tm* out, const time_t* t) { return gmtime_r(t, out) ? 0 : EINVAL; }
typedef long long __time64_t;
inline __time64_t _time64(__time64_t* t)
{
	__time64_t v = (__time64_t)time(0);
	if (t) *t = v;
	return v;
}

// ---- files -------------------------------------------------------------------------------------
inline errno_t fopen_s(FILE** f, const char* name, const char* mode)
{
	*f = fopen(name, mode);
	return *f ? 0 : errno;
}
#define _unlink unlink
#define _access access
#define _getcwd getcwd
#define _fileno fileno
#define _stat stat

#endif  // LEKMOD_PORTABLE_MSVC_CRT_H
