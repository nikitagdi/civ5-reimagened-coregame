// rt_vfs: game paths -> files through the game-data manifest (rt_vfs).
// vfs-index.json and every file it lists come from the host read_file callback; nothing touches the file system.
// Rules of HstVfs and the exporter: include() matches the file stem (case-insensitive, directory and ".lua" ignored);
// the first root of include_roots in order wins; inside a root files come before subfolders and folders go by name;
// the first root (the map script's folder) is not recursive, the others are.
#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "gamecore_abi.h"
#include "rt_lua.h"

namespace lekmod_portable {

class RtVfs : public RtLuaSource
{
public:
	RtVfs();

	// Reads and parses indexPath ("vfs-index.json") through host->read_file (the host struct is copied).
	// False + error on failure.
	bool Load(const gc_host* host, std::string& error, const char* indexPath = "vfs-index.json");

	bool IsLoaded() const { return m_loaded; }
	const std::vector<std::string>& IncludeRoots() const { return m_includeRoots; }
	const std::vector<std::string>& Files() const { return m_files; }
	const std::vector<std::string>& Contexts() const { return m_contexts; }
	const std::string& ContextEntry() const { return m_contextEntry; }
	const std::string& DbGameplay() const { return m_dbGameplay; }
	const std::string& DbLocalization() const { return m_dbLocalization; }
	// "lekmap_pangaea" -> path of the map script file ("" when unknown); names compare case-insensitively.
	std::string MapScript(const char* name) const;

	// RtLuaSource
	virtual bool ResolveInclude(const char* name, std::string& path);
	virtual bool ResolvePath(const char* name, std::string& path);
	virtual bool ReadSource(const std::string& path, std::string& out);
	virtual void PushFrontIncludeRoot(const std::string& path);
	virtual bool IsKnownMissing(const char* name);

private:
	struct Root
	{
		std::string dir;
		bool recursive;
		std::map<std::string, std::string> stems;  // lowercase stem -> path
	};
	void BuildRoot(Root& root) const;

	gc_host m_host;
	bool m_loaded;
	std::vector<std::string> m_includeRoots;
	std::vector<std::string> m_files;  // as listed (lowercase paths)
	std::set<std::string> m_fileSet;
	std::vector<std::string> m_contexts;
	std::string m_contextEntry;
	std::string m_dbGameplay, m_dbLocalization;
	std::map<std::string, std::string> m_mapScripts;  // lowercase name -> path
	std::set<std::string> m_knownMissing;              // lowercase file names
	std::vector<Root> m_roots;                         // include order; PushFrontIncludeRoot inserts at the front
};

}  // namespace lekmod_portable
