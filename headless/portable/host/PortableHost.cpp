#include "CvGameCoreDLLPCH.h"

#include "PortableHost.h"

#include "CvDllContext.h"
#include "lekmod_portable/host_log.h"

// The gamecore's own DllMain (CvGameCoreDLL.cpp) is plain code outside Win32; it is reused as is.
BOOL APIENTRY DllMain(HANDLE hModule, DWORD ul_reason_for_call, LPVOID);

namespace {
bool g_attached = false;
}

extern "C" void LekmodPortable_DllAttach(void)
{
	if (g_attached) return;
	DllMain(NULL, DLL_PROCESS_ATTACH, NULL);
	g_attached = true;
}

extern "C" void LekmodPortable_DllDetach(void)
{
	if (!g_attached) return;
	DllMain(NULL, DLL_PROCESS_DETACH, NULL);
	g_attached = false;
}

extern "C" int LekmodPortable_IsAttached(void) { return g_attached ? 1 : 0; }

namespace lekmod_portable {

EngineUtility::EngineUtility() : m_cacheFolder("./") {}

const char* EngineUtility::GetCacheFolderPath() { return m_cacheFolder.c_str(); }
bool EngineUtility::IsHost() const { return true; }
void EngineUtility::InitGlobals() {}
void EngineUtility::UninitGlobals() {}
CvDLLInterfaceIFaceBase* EngineUtility::getInterfaceIFace() { return NULL; }

}  // namespace lekmod_portable
