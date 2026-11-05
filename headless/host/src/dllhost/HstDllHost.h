//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstDllHost.h
//!  \brief     Loads the gamecore DLL (D1-D2) and runs the DLL init sequence (I1-I8).
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#pragma once
#ifndef HSTDLLHOST_H
#define HSTDLLHOST_H

#include "HstCommon.h"

class HstEngineUtility;
class HstUserInterface;
class HstDatabase;

class HstDllHost
{
public:
	HstDllHost();

	//! Loads the three game DLLs from the game root (sandbox-patching each), then the gamecore DLL (D1) and QIs (D2).
	bool Load(const std::wstring& wstrDllPath);
	//! I1-I8. pkEngine/pkUI/pkDb stay owned by the caller and must outlive the DLL.
	bool Init(HstEngineUtility* pkEngine, HstUserInterface* pkUI, HstDatabase* pkDb);

	ICvGameContext1* GetContext() const { return m_pkCtx; }
	ICvGameContext3* GetContext3() const { return m_pkCtx3; }
	HMODULE GetModule() const { return m_hDll; }
	std::string GetVersion() const;

protected:
	HMODULE m_hDll;
	ICvGameContext1* m_pkCtx;
	ICvGameContext3* m_pkCtx3;
};

#endif // HSTDLLHOST_H
