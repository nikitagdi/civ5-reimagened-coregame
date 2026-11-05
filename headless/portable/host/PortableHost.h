// Exe-side services for the portable gamecore: the ICvEngineUtility4 / ICvUserInterface2 objects the DLL calls
// back into (gDLL, DLLUI) and the replacement for DllMain.
#pragma once

#include "gen/PortableEngineUtility4Gen.h"
#include "gen/PortableUserInterface2Gen.h"

#include <string>

// DllMain(DLL_PROCESS_ATTACH / DLL_PROCESS_DETACH) for builds without a Win32 loader. Attach creates the
// CvDllGameContext singleton; detach destroys it and clears gDLL. Both are idempotent.
extern "C" void LekmodPortable_DllAttach(void);
extern "C" void LekmodPortable_DllDetach(void);
extern "C" int LekmodPortable_IsAttached(void);

namespace lekmod_portable {

class EngineUtility : public PortableEngineUtility4Gen
{
public:
	EngineUtility();

	void SetCacheFolderPath(const std::string& path) { m_cacheFolder = path; }

	virtual const char* GetCacheFolderPath();
	virtual bool IsHost() const;
	virtual void InitGlobals();
	virtual void UninitGlobals();
	virtual CvDLLInterfaceIFaceBase* getInterfaceIFace();

private:
	std::string m_cacheFolder;
};

class UserInterface : public PortableUserInterface2Gen
{
};

}  // namespace lekmod_portable
