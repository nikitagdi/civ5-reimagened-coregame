// Portable replacement for <tchar.h>: TCHAR is char (the gamecore is built without UNICODE).
#ifndef LEKMOD_PORTABLE_TCHAR_H
#define LEKMOD_PORTABLE_TCHAR_H

#include "lekmod_portable/prelude.h"

#if !defined(LEKMOD_PORTABLE_TCHAR_DEFINED)
#define LEKMOD_PORTABLE_TCHAR_DEFINED
typedef char TCHAR;
typedef char _TCHAR;
typedef const char* LPCTSTR;
typedef char* LPTSTR;
#endif

#define _T(x) x
#define _TEXT(x) x
#define TEXT(x) x
#define _tcslen strlen
#define _tcscpy strcpy
#define _tcscpy_s strcpy_s
#define _tcsncpy strncpy
#define _tcsncpy_s strncpy_s
#define _tcscat strcat
#define _tcscat_s strcat_s
#define _tcsncat strncat
#define _tcscmp strcmp
#define _tcsicmp _stricmp
#define _tcsnicmp _strnicmp
#define _tcsncmp strncmp
#define _tcschr strchr
#define _tcsrchr strrchr
#define _tcsstr strstr
#define _tcstok strtok
#define _tcstok_s strtok_s
#define _tcsdup _strdup
#define _tprintf printf
#define _stprintf sprintf
#define _stprintf_s sprintf_s
#define _sntprintf _snprintf
#define _vstprintf vsprintf
#define _ftprintf fprintf
#define _ttoi atoi
#define _tstoi atoi
#define _tcstol strtol
#define _istspace isspace
#define _istdigit isdigit
#define _istalpha isalpha
#define _totlower tolower
#define _totupper toupper

#endif  // LEKMOD_PORTABLE_TCHAR_H
