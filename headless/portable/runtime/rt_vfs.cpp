#include "CvGameCoreDLLPCH.h"

#include "rt_vfs.h"

#include <ctype.h>
#include <string.h>

#include <algorithm>

#include "rt_json.h"
#include "lekmod_portable/host_log.h"

namespace lekmod_portable {

namespace {

// Lowercase, '\' -> '/', no leading "./" or '/', no trailing '/'.
std::string Norm(const std::string& in)
{
	std::string s(in);
	for (size_t i = 0; i < s.size(); ++i)
	{
		if (s[i] == '\\') s[i] = '/';
		else s[i] = (char)tolower((unsigned char)s[i]);
	}
	while (s.compare(0, 2, "./") == 0) s.erase(0, 2);
	while (!s.empty() && s[0] == '/') s.erase(0, 1);
	while (!s.empty() && s[s.size() - 1] == '/') s.erase(s.size() - 1);
	return s;
}

std::string StemOf(const char* name)
{
	std::string stem = Norm(name ? name : "");
	size_t slash = stem.find_last_of('/');
	if (slash != std::string::npos) stem = stem.substr(slash + 1);
	if (stem.size() > 4 && stem.compare(stem.size() - 4, 4, ".lua") == 0) stem.erase(stem.size() - 4);
	return stem;
}

std::string DirOf(const std::string& path)
{
	size_t slash = path.find_last_of('/');
	return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::vector<std::string> Split(const std::string& s)
{
	std::vector<std::string> parts;
	size_t start = 0;
	for (;;)
	{
		size_t slash = s.find('/', start);
		if (slash == std::string::npos)
		{
			parts.push_back(s.substr(start));
			return parts;
		}
		parts.push_back(s.substr(start, slash - start));
		start = slash + 1;
	}
}

// Depth-first order of HstVfs::IndexDir: in each folder the files (by name) precede the subfolders (by name).
struct Entry
{
	std::vector<std::string> parts;  // path relative to the root
	std::string path;
};

bool EntryLess(const Entry& a, const Entry& b)
{
	size_t n = std::min(a.parts.size(), b.parts.size());
	for (size_t i = 0; i < n; ++i)
	{
		if (a.parts[i] == b.parts[i]) continue;
		bool aFile = i + 1 == a.parts.size();
		bool bFile = i + 1 == b.parts.size();
		if (aFile != bFile) return aFile;
		return a.parts[i] < b.parts[i];
	}
	return a.parts.size() < b.parts.size();
}

void ReadStrings(const RtJson* arr, std::vector<std::string>& out, bool normalize)
{
	if (!arr || arr->GetType() != RtJson::JSON_ARRAY) return;
	for (size_t i = 0; i < arr->Size(); ++i)
	{
		std::string s = arr->At(i).AsString();
		out.push_back(normalize ? Norm(s) : s);
	}
}

}  // namespace

RtVfs::RtVfs() : m_loaded(false) { memset(&m_host, 0, sizeof(m_host)); }

void RtVfs::BuildRoot(Root& root) const
{
	std::vector<Entry> entries;
	const std::string prefix = root.dir.empty() ? std::string() : root.dir + "/";
	for (size_t i = 0; i < m_files.size(); ++i)
	{
		const std::string& p = m_files[i];
		if (p.size() < 4 || p.compare(p.size() - 4, 4, ".lua") != 0) continue;
		if (p.compare(0, prefix.size(), prefix) != 0) continue;
		Entry e;
		e.path = p;
		e.parts = Split(p.substr(prefix.size()));
		if (!root.recursive && e.parts.size() != 1) continue;
		entries.push_back(e);
	}
	std::stable_sort(entries.begin(), entries.end(), EntryLess);
	for (size_t i = 0; i < entries.size(); ++i)
	{
		const std::string& name = entries[i].parts.back();
		std::string stem = name.substr(0, name.size() - 4);
		if (root.stems.find(stem) == root.stems.end()) root.stems[stem] = entries[i].path;
	}
}

bool RtVfs::Load(const gc_host* host, std::string& error, const char* indexPath)
{
	m_loaded = false;
	if (!host || !host->read_file)
	{
		error = "no read_file callback";
		return false;
	}
	m_host = *host;
	gc_buf buf = {0, 0, 0, 0};
	int32_t rc = m_host.read_file(m_host.user, indexPath, &buf);
	if (rc != GC_OK)
	{
		char code[32];
		snprintf(code, sizeof(code), "%d", (int)rc);
		error = std::string("cannot read ") + indexPath + " (code " + code + ")";
		return false;
	}
	std::string text((const char*)buf.data, buf.len);
	gc_buf_release(&buf);

	RtJson doc;
	std::string perr;
	if (!RtJson::Parse(text, doc, perr))
	{
		error = std::string(indexPath) + ": " + perr;
		return false;
	}
	if (doc.GetType() != RtJson::JSON_OBJECT || doc.Get("include_roots") == NULL || doc.Get("files") == NULL)
	{
		error = std::string(indexPath) + ": not a vfs index (include_roots / files missing)";
		return false;
	}

	m_includeRoots.clear();
	m_files.clear();
	m_fileSet.clear();
	m_contexts.clear();
	m_mapScripts.clear();
	m_knownMissing.clear();
	m_roots.clear();
	m_contextEntry.clear();
	m_dbGameplay.clear();
	m_dbLocalization.clear();

	ReadStrings(doc.Get("include_roots"), m_includeRoots, true);
	ReadStrings(doc.Get("contexts"), m_contexts, false);
	const RtJson* files = doc.Get("files");
	for (size_t i = 0; i < files->Size(); ++i)
	{
		const RtJson* p = files->At(i).Get("path");
		if (!p) continue;
		std::string path = Norm(p->AsString());
		if (path.empty()) continue;
		m_files.push_back(path);
		m_fileSet.insert(path);
	}
	std::vector<std::string> missing;
	ReadStrings(doc.Get("known_missing"), missing, true);
	m_knownMissing.insert(missing.begin(), missing.end());
	if (const RtJson* ce = doc.Get("context_entry")) m_contextEntry = ce->AsString();
	if (const RtJson* db = doc.Get("db"))
	{
		if (const RtJson* g = db->Get("gameplay")) m_dbGameplay = g->AsString();
		if (const RtJson* l = db->Get("localization")) m_dbLocalization = l->AsString();
	}
	if (const RtJson* ms = doc.Get("map_scripts"))
		for (size_t i = 0; ms->GetType() == RtJson::JSON_OBJECT && i < ms->Size(); ++i)
			m_mapScripts[Norm(ms->KeyAt(i))] = Norm(ms->ValueAt(i).AsString());

	for (size_t i = 0; i < m_includeRoots.size(); ++i)
	{
		Root r;
		r.dir = m_includeRoots[i];
		r.recursive = i != 0;  // the first root is the script folder: its subfolders are not searched
		BuildRoot(r);
		m_roots.push_back(r);
	}
	m_loaded = true;
	Log(LOG_INFO, "vfs: %u files, %u include roots, %u contexts", (unsigned)m_files.size(), (unsigned)m_includeRoots.size(),
	    (unsigned)m_contexts.size());
	return true;
}

std::string RtVfs::MapScript(const char* name) const
{
	std::map<std::string, std::string>::const_iterator it = m_mapScripts.find(Norm(name ? name : ""));
	return it == m_mapScripts.end() ? std::string() : it->second;
}

bool RtVfs::ResolveInclude(const char* name, std::string& path)
{
	const std::string stem = StemOf(name);
	for (size_t i = 0; i < m_roots.size(); ++i)
	{
		std::map<std::string, std::string>::const_iterator it = m_roots[i].stems.find(stem);
		if (it != m_roots[i].stems.end())
		{
			path = it->second;
			return true;
		}
	}
	return false;
}

bool RtVfs::ResolvePath(const char* name, std::string& path)
{
	if (!name || !*name) return false;
	std::string p = Norm(name);
	if (m_fileSet.count(p))
	{
		path = p;
		return true;
	}
	// The gamecore may spell the path relative to the game's Assets folder.
	if (p.compare(0, 7, "assets/") != 0 && m_fileSet.count("assets/" + p))
	{
		path = "assets/" + p;
		return true;
	}
	// ... or absolute: take the tail starting at the first "assets/" component.
	size_t at = p.find("/assets/");
	if (at != std::string::npos && m_fileSet.count(p.substr(at + 1)))
	{
		path = p.substr(at + 1);
		return true;
	}
	return false;
}

bool RtVfs::ReadSource(const std::string& path, std::string& out)
{
	if (!m_host.read_file) return false;
	gc_buf buf = {0, 0, 0, 0};
	if (m_host.read_file(m_host.user, path.c_str(), &buf) != GC_OK) return false;
	out.assign((const char*)buf.data, buf.len);
	gc_buf_release(&buf);
	// Lua 5.1 does not accept a UTF-8 BOM.
	if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF) out.erase(0, 3);
	return true;
}

void RtVfs::PushFrontIncludeRoot(const std::string& path)
{
	const std::string dir = DirOf(Norm(path));
	for (size_t i = 0; i < m_roots.size(); ++i)
		if (m_roots[i].dir == dir) return;
	Root r;
	r.dir = dir;
	r.recursive = false;
	BuildRoot(r);
	m_roots.insert(m_roots.begin(), r);
}

bool RtVfs::IsKnownMissing(const char* name) { return m_knownMissing.count(StemOf(name) + ".lua") != 0; }

}  // namespace lekmod_portable
