// Force-included (-include) into every translation unit of the portable gamecore build.
// Defines LEKMOD_PORTABLE and supplies what MSVC 2008 provided implicitly.
#ifndef LEKMOD_PORTABLE_PRELUDE_H
#define LEKMOD_PORTABLE_PRELUDE_H

#ifndef LEKMOD_PORTABLE
#define LEKMOD_PORTABLE 1
#endif

#if !defined(__cplusplus)
#error "the portable gamecore prelude is C++ only"
#endif

#include "lekmod_portable/msvc_crt.h"
#include "lekmod_portable/sal.h"
#include "lekmod_portable/tr1_memory.h"

#include <float.h>
#include <limits.h>
#include <new>

// Imports from the closed Firaxis DLLs are plain symbols in libgamecore.a.
#ifndef CvGameDatabaseAPI
#define CvGameDatabaseAPI
#endif
#ifndef CvLocalizationAPI
#define CvLocalizationAPI
#endif

#ifndef __forceinline
#define __forceinline inline __attribute__((always_inline))
#endif

// Fixed by the port everywhere (lua_Integer is 4 bytes as on Win32); see headless/portable/README.md.
#ifndef LEKMOD_LUA_INTEGER
#define LEKMOD_LUA_INTEGER int
#endif

#include <iterator>
namespace lekmod_portable {
// C++03 containers erase only through a mutable iterator; MSVC 2008 also accepted a const_iterator.
template <class C>
typename C::iterator mutable_iterator(C& c, typename C::const_iterator it)
{
	typename C::iterator m = c.begin();
	std::advance(m, std::distance(typename C::const_iterator(c.begin()), it));
	return m;
}
}  // namespace lekmod_portable

// MSVC parses template default arguments lazily, so FireWorks container headers use the pool enum of
// FMemHooks.h without including it. Make it visible up front.
typedef unsigned int uint;
#include <FireWorks/FMemHooks.h>

// The portable build is not LEKMOD_HEADLESS_BUILD, so _Defines.h does not pull in HeadlessImprovements.h / HeadlessDiag.h;
// the FIX code it enables (build.sh DEFINES) still reports its trigger. No diag sink here: a no-op, as in HeadlessDiag.h.
#ifndef HEADLESS_FIX_TRIGGER
#define HEADLESS_FIX_TRIGGER(iFixId)
#endif
// The OPT code (build.sh OPT_DEFINES) fences its shadow checks with HEADLESS_OPT_VERIFY; without LEKMOD_HEADLESS_OPT_VERIFY
// it is a no-op in HeadlessDiag.h as well (the condition is not evaluated).
#ifndef HEADLESS_OPT_VERIFY
#define HEADLESS_OPT_VERIFY(cond, iOptId, pszWhat)
#endif

// FIX-VALIDATION helpers (FIX-0101..0113). CvDllNetMessageHandler.cpp defines them under LEKMOD_HEADLESS_BUILD, which the
// portable build does not set. Same pure range checks as macros (MAX_PLAYERS etc. are visible at the use site); the
// rejection is silent here: no net_message_debug.log in the portable build, no diag sink (HEADLESS_FIX_TRIGGER above).
#ifndef LEKMOD_HEADLESS_BUILD
#define HeadlessIsPlayerId(iPlayer) ((iPlayer) >= 0 && (iPlayer) < MAX_PLAYERS)
#define HeadlessIsMajorId(iPlayer) ((iPlayer) >= 0 && (iPlayer) < MAX_MAJOR_CIVS)
#define HeadlessIsMinorId(iPlayer) ((iPlayer) >= MAX_MAJOR_CIVS && (iPlayer) < MAX_CIV_PLAYERS)
#define HeadlessIsTeamId(iTeam) ((iTeam) >= 0 && (iTeam) < MAX_TEAMS)
#define HeadlessRejectResponse(iFixId, szHandler, iPlayer) HEADLESS_FIX_TRIGGER(iFixId)
#endif

#endif  // LEKMOD_PORTABLE_PRELUDE_H
