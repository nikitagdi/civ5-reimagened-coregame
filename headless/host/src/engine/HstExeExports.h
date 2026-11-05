//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstExeExports.h
//!  \brief     FireWorks services exported by civhost.exe for the gamecore DLL (see HstExeExports.cpp).
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#pragma once
#ifndef HSTEXEEXPORTS_H
#define HSTEXEEXPORTS_H

//! Must run before the gamecore DLL is loaded (its static initializers call the exports).
void HstExeExportsInit(bool bEnableDllLogs);
//! Close DLL log files (flushes them).
void HstExeExportsFlush();

#endif // HSTEXEEXPORTS_H
