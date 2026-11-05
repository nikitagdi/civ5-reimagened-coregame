//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstCommon.h
//!  \brief     Common includes for civhost: Win32, CRT/STL and the gamecore SDK interface headers.
//!
//!		STL settings must match the gamecore DLL (CvGameCoreDLLPCH.h): CvString / std::string / std::vector
//!		objects cross the DLL boundary by value, so their layout has to be identical.
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#pragma once
#ifndef HSTCOMMON_H
#define HSTCOMMON_H

#pragma warning(disable : 4100)	// unreferenced formal parameter (generated stubs)
#pragma warning(disable : 4800)	// forcing value to bool
#pragma warning(disable : 4355)	// 'this' used in base member initializer list
#pragma warning(disable : 4996)	// deprecated CRT / DECLSPEC_DEPRECATED interface methods
#pragma warning(disable : 4251)	// STL members in CvLocalization exported classes

#if !defined(_SECURE_SCL)
#define _SECURE_SCL 0
#endif
#if !defined(_HAS_ITERATOR_DEBUGGING)
#define _HAS_ITERATOR_DEBUGGING 0
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <MMSystem.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tchar.h>
#include <algorithm>
#include <string>
#include <vector>
#include <list>
#include <map>
#include <set>
#include <memory>
#include <limits>

typedef unsigned char    byte;
typedef unsigned int     uint;
typedef wchar_t          wchar;

// FireWorks (types used by the interface signatures; FireWorksWin32.lib is linked statically)
#include <FireWorks/FDefNew.h>
#include <FireWorks/FFireTypes.h>
#include <FireWorks/FAssert.h>
#include <FireWorks/FILogFile.h>
#include <FireWorks/FFreeListArrayBase.h>
#include <FireWorks/FDataStream.h>
#include <FireWorks/FFastList.h>
#include <FireWorks/FFastVector.h>
#include <FireWorks/FMemoryStream.h>

extern "C"
{
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

// SDK: database (CvGameDatabaseWin32Final Release.dll), core util, localization, engine interfaces
#include "CvGameDatabase.h"
#include "CvGameCoreDLLUtil.h"
#include "CvDllInterfaces.h"
#include "ICvDLLUtility.h"
#include "ICvDLLUserInterface.h"
#include "ICvDLLScriptSystem.h"
#include "ICvDLLDatabaseUtility.h"
#include "CvLocalization.h"


#define HST_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#endif // HSTCOMMON_H
