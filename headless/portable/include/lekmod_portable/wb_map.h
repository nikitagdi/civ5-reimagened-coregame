// WorldBuilder .Civ5Map files for the portable CvWorldBuilderMap::Load (headless/portable/firaxis/CvWorldBuilderMapLoad.cpp).
// The portable build has no file system: the runtime installs the host's read_file, and Load reads the whole file through it.
#ifndef LEKMOD_PORTABLE_WB_MAP_H
#define LEKMOD_PORTABLE_WB_MAP_H

#include <string>

namespace lekmod_portable {

// Reads the whole file at a game-data path ('/' separated, as gc_host.read_file takes it). False when it is missing.
typedef bool (*WbMapReadFn)(void* user, const char* path, std::string& bytes);

// Installed by RtSession::Load; NULL uninstalls (Load then fails).
void SetWbMapReader(WbMapReadFn fn, void* user);

// The path CvWorldBuilderMap::Load gets (wide, '\' or '/') as the game-data path handed to the reader.
std::string WbMapPath(const wchar_t* path);

// The bytes of a map file through the installed reader.
bool ReadWbMapFile(const wchar_t* path, std::string& bytes);

}  // namespace lekmod_portable

#endif
