// rt_fs: in-memory ./Civ5SavedGameDatabase.db. See rt_fs.h.
// The FireWorks headers expect the gamecore precompiled-header environment.
#include "CvGameCoreDLLPCH.h"

#include <string.h>
#include <time.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "FireWorks/FFileDefines.h"
#include "FireWorks/FIFile.h"

#include "rt_fs.h"
#include "lekmod_portable/host_log.h"

namespace lekmod_portable {
namespace rt_fs {

namespace {

// ---- memory file table ----------------------------------------------------------------------------
// refs counts the table entry plus every open handle, so a file deleted while open stays readable.
struct MemFile
{
	std::vector<unsigned char> data;
	int refs;
	MemFile() : refs(1) {}
};

typedef std::map<std::string, MemFile*> FileMap;
FileMap& files()
{
	static FileMap* s_files = new FileMap();  // leaked on purpose: sqlite may close files at exit
	return *s_files;
}

void release(MemFile* f)
{
	if (f && --f->refs == 0) delete f;
}

// "./Civ5SavedGameDatabase.db", ".\\Civ5SavedGameDatabase.db" and "C:\\x\\civ5savedgamedatabase.db" differ only in
// spelling: key = forward slashes, lowercase, no leading "./".
std::string normalize(const char* name)
{
	std::string s(name ? name : "");
	for (size_t i = 0; i < s.size(); ++i)
	{
		char c = s[i];
		if (c == '\\') c = '/';
		if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
		s[i] = c;
	}
	while (s.size() >= 2 && s[0] == '.' && s[1] == '/') s.erase(0, 2);
	return s;
}

MemFile* find(const std::string& key)
{
	FileMap::iterator it = files().find(key);
	return it == files().end() ? NULL : it->second;
}

MemFile* create_entry(const std::string& key)
{
	MemFile* f = new MemFile();
	files()[key] = f;  // the table's reference
	return f;
}

bool remove_entry(const std::string& key)
{
	FileMap::iterator it = files().find(key);
	if (it == files().end()) return false;
	MemFile* f = it->second;
	files().erase(it);
	release(f);
	return true;
}

std::string narrow16(const wchar_t* w)
{
	if (!w) return std::string();
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
	if (n <= 1) return std::string();
	std::string s((size_t)n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, NULL, NULL);
	s.resize((size_t)n - 1);
	return s;
}

// ---- Win32 handles ----------------------------------------------------------------------------------
struct MemHandle
{
	MemFile* file;
	size_t pos;
};

std::set<void*>& handles()
{
	static std::set<void*>* s_handles = new std::set<void*>();
	return *s_handles;
}

// ---- sqlite memory VFS --------------------------------------------------------------------------------
struct VfsFile
{
	sqlite3_file base;
	MemFile* file;
	std::string* deleteKey;  // set for SQLITE_OPEN_DELETEONCLOSE files that have a name
};

int vfs_close(sqlite3_file* p)
{
	VfsFile* vf = (VfsFile*)p;
	if (vf->deleteKey)
	{
		if (find(*vf->deleteKey) == vf->file) remove_entry(*vf->deleteKey);
		delete vf->deleteKey;
		vf->deleteKey = NULL;
	}
	release(vf->file);
	vf->file = NULL;
	return SQLITE_OK;
}

int vfs_read(sqlite3_file* p, void* buf, int amt, sqlite3_int64 off)
{
	std::vector<unsigned char>& d = ((VfsFile*)p)->file->data;
	size_t have = 0;
	if (off >= 0 && (size_t)off < d.size()) have = d.size() - (size_t)off;
	if (have > (size_t)amt) have = (size_t)amt;
	if (have) memcpy(buf, &d[(size_t)off], have);
	if (have < (size_t)amt)
	{
		memset((unsigned char*)buf + have, 0, (size_t)amt - have);
		return SQLITE_IOERR_SHORT_READ;
	}
	return SQLITE_OK;
}

int vfs_write(sqlite3_file* p, const void* buf, int amt, sqlite3_int64 off)
{
	std::vector<unsigned char>& d = ((VfsFile*)p)->file->data;
	if (off < 0) return SQLITE_IOERR_WRITE;
	size_t end = (size_t)off + (size_t)amt;
	if (end > d.size()) d.resize(end, 0);
	if (amt) memcpy(&d[(size_t)off], buf, (size_t)amt);
	return SQLITE_OK;
}

int vfs_truncate(sqlite3_file* p, sqlite3_int64 size)
{
	((VfsFile*)p)->file->data.resize((size_t)size, 0);
	return SQLITE_OK;
}

int vfs_sync(sqlite3_file*, int) { return SQLITE_OK; }

int vfs_file_size(sqlite3_file* p, sqlite3_int64* size)
{
	*size = (sqlite3_int64)((VfsFile*)p)->file->data.size();
	return SQLITE_OK;
}

int vfs_lock(sqlite3_file*, int) { return SQLITE_OK; }  // single thread, single process: no locking
int vfs_unlock(sqlite3_file*, int) { return SQLITE_OK; }
int vfs_check_reserved(sqlite3_file*, int* res)
{
	*res = 0;
	return SQLITE_OK;
}
int vfs_file_control(sqlite3_file*, int, void*) { return SQLITE_NOTFOUND; }
int vfs_sector_size(sqlite3_file*) { return 4096; }
int vfs_device_characteristics(sqlite3_file*) { return 0; }

const sqlite3_io_methods g_io_methods = {
	1, vfs_close, vfs_read, vfs_write, vfs_truncate, vfs_sync, vfs_file_size, vfs_lock, vfs_unlock, vfs_check_reserved,
	vfs_file_control, vfs_sector_size, vfs_device_characteristics, 0, 0, 0, 0, 0, 0
};

int vfs_open(sqlite3_vfs*, const char* name, sqlite3_file* p, int flags, int* outFlags)
{
	VfsFile* vf = (VfsFile*)p;
	vf->base.pMethods = NULL;
	vf->file = NULL;
	vf->deleteKey = NULL;
	if (!name)  // anonymous temp file: private, dies on close
	{
		vf->file = new MemFile();
	}
	else
	{
		std::string key = normalize(name);
		MemFile* f = find(key);
		if (!f)
		{
			if (!(flags & SQLITE_OPEN_CREATE)) return SQLITE_CANTOPEN;
			f = create_entry(key);
		}
		else if ((flags & SQLITE_OPEN_EXCLUSIVE) && (flags & SQLITE_OPEN_CREATE))
		{
			return SQLITE_CANTOPEN;
		}
		++f->refs;
		vf->file = f;
		if (flags & SQLITE_OPEN_DELETEONCLOSE) vf->deleteKey = new std::string(key);
	}
	if (outFlags) *outFlags = flags;
	vf->base.pMethods = &g_io_methods;
	return SQLITE_OK;
}

int vfs_delete(sqlite3_vfs*, const char* name, int)
{
	return remove_entry(normalize(name)) ? SQLITE_OK : SQLITE_IOERR_DELETE_NOENT;
}

int vfs_access(sqlite3_vfs*, const char* name, int, int* res)
{
	*res = find(normalize(name)) ? 1 : 0;
	return SQLITE_OK;
}

int vfs_full_pathname(sqlite3_vfs*, const char* name, int n, char* out)
{
	size_t len = strlen(name);
	if ((int)len + 1 > n) return SQLITE_CANTOPEN;
	memcpy(out, name, len + 1);
	return SQLITE_OK;
}

int vfs_randomness(sqlite3_vfs*, int n, char* out)
{
	static unsigned int s = 0x9e3779b9u;
	for (int i = 0; i < n; ++i)
	{
		s = s * 1664525u + 1013904223u;
		out[i] = (char)(s >> 24);
	}
	return n;
}

int vfs_sleep(sqlite3_vfs*, int micros) { return micros; }  // nobody to wait for

int vfs_current_time(sqlite3_vfs*, double* t)
{
	*t = 2440587.5 + (double)time(NULL) / 86400.0;  // Julian day from the Unix clock
	return SQLITE_OK;
}

int vfs_get_last_error(sqlite3_vfs*, int, char*) { return 0; }

sqlite3_vfs g_vfs = {
	1, sizeof(VfsFile), 512, NULL, "rtmem", NULL, vfs_open, vfs_delete, vfs_access, vfs_full_pathname,
	NULL, NULL, NULL, NULL, vfs_randomness, vfs_sleep, vfs_current_time, vfs_get_last_error, NULL, NULL, NULL
};

// ---- FIFile / FIFileSystem -----------------------------------------------------------------------------
class MemFIFile : public FIFile
{
public:
	MemFIFile() : m_file(NULL), m_pos(0) {}
	virtual ~MemFIFile() { Close(); }

	bool OpenKey(const std::string& key, const std::string& display, OpenMode mode, FileErr* err)
	{
		MemFile* f = find(key);
		if (!f)
		{
			if (mode == modeRead)
			{
				*err = FILE_NOTFOUND;
				return false;
			}
			f = create_entry(key);
		}
		else if (mode == modeWrite)
		{
			f->data.clear();
		}
		++f->refs;
		m_file = f;
		m_pos = 0;
		m_name = display;
		m_wname.assign(display.begin(), display.end());
		*err = FILE_OK;
		return true;
	}

	virtual FileErr Open(const char* name, OpenMode mode, dword, uint)
	{
		FileErr err = FILE_BADNAME;
		if (!name || !IsMemPath(name)) return FILE_NOTFOUND;
		Close();
		OpenKey(normalize(name), name, mode, &err);
		return err;
	}
	virtual FileErr Open(const wchar* name, OpenMode mode, dword flags, uint size) { return Open(narrow16(name).c_str(), mode, flags, size); }
	virtual FileErr Close()
	{
		release(m_file);
		m_file = NULL;
		return FILE_OK;
	}
	virtual const wchar_t* GetFileName() { return m_wname.c_str(); }
	virtual byte* GetViewCaret() const { return m_file && m_pos < m_file->data.size() ? &m_file->data[m_pos] : NULL; }
	virtual byte* GetViewEnd() const { return m_file && !m_file->data.empty() ? &m_file->data[0] + m_file->data.size() : NULL; }
	virtual byte* GetViewPtr() const { return m_file && !m_file->data.empty() ? &m_file->data[0] : NULL; }
	virtual uint GetLength() const { return m_file ? (uint)m_file->data.size() : 0; }
	virtual void SetLength(uint size)
	{
		if (m_file) m_file->data.resize(size, 0);
	}
	virtual bool IsOpen() const { return m_file != NULL; }
	virtual bool IsMapped() const { return false; }
	virtual uint Seek(long off, SeekMode origin)
	{
		if (!m_file) return 0;
		long base = origin == begin ? 0 : (origin == current ? (long)m_pos : (long)m_file->data.size());
		long np = base + off;
		m_pos = np < 0 ? 0 : (size_t)np;
		return (uint)m_pos;
	}
	virtual uint SeekToBegin() { return Seek(0, begin); }
	virtual uint SeekToEnd() { return Seek(0, end); }
	virtual uint GetPosition() { return (uint)m_pos; }
	virtual void Rewind() { m_pos = 0; }
	virtual bool IsEOF() { return !m_file || m_pos >= m_file->data.size(); }
	virtual uint Read(void* p, uint size)
	{
		if (!m_file || m_pos >= m_file->data.size()) return 0;
		size_t n = m_file->data.size() - m_pos;
		if (n > size) n = size;
		if (n) memcpy(p, &m_file->data[m_pos], n);
		m_pos += n;
		return (uint)n;
	}
	virtual uint Write(const void* p, uint size)
	{
		if (!m_file) return 0;
		if (m_pos + size > m_file->data.size()) m_file->data.resize(m_pos + size, 0);
		if (size) memcpy(&m_file->data[m_pos], p, size);
		m_pos += size;
		return size;
	}
	virtual uint Flush() { return 0; }

private:
	MemFile* m_file;
	size_t m_pos;
	std::string m_name;
	std::wstring m_wname;
};

class RtFileSystem : public FIFileSystem
{
public:
	explicit RtFileSystem(FIFileSystem* fallback) : m_fallback(fallback) {}

	virtual FIFile* Create(const char* name, FIFile::OpenMode mode, dword flags, uint size)
	{
		if (!name || !IsMemPath(name)) return m_fallback ? m_fallback->Create(name, mode, flags, size) : NULL;
		MemFIFile* f = new MemFIFile();
		if (f->Open(name, mode, flags, size) != FILE_OK)
		{
			delete f;
			return NULL;
		}
		return f;
	}
	virtual FIFile* Create(const wchar* name, FIFile::OpenMode mode, dword flags, uint size) { return Create(narrow16(name).c_str(), mode, flags, size); }

	virtual FileErr Delete(const char* name)
	{
		if (!name || !IsMemPath(name)) return m_fallback ? m_fallback->Delete(name) : FILE_NOTFOUND;
		return remove_entry(normalize(name)) ? FILE_OK : FILE_NOTFOUND;
	}
	virtual FileErr Delete(const wchar* name) { return Delete(narrow16(name).c_str()); }

	virtual FileErr ReadAll(const char* name, byte** out, size_t* outSize, dword flags)
	{
		if (!name || !IsMemPath(name)) return m_fallback ? m_fallback->ReadAll(name, out, outSize, flags) : FILE_NOTFOUND;
		return ReadAllImpl(name, out, outSize, NULL);
	}
	virtual FileErr ReadAll(const wchar* name, byte** out, size_t* outSize, dword flags) { return ReadAll(narrow16(name).c_str(), out, outSize, flags); }
	virtual FileErr ReadAll(const char* name, byte** out, size_t* outSize, dword flags, FIFile::Allocator& alloc)
	{
		if (!name || !IsMemPath(name)) return m_fallback ? m_fallback->ReadAll(name, out, outSize, flags, alloc) : FILE_NOTFOUND;
		return ReadAllImpl(name, out, outSize, &alloc);
	}
	virtual FileErr ReadAll(const wchar* name, byte** out, size_t* outSize, dword flags, FIFile::Allocator& alloc)
	{
		return ReadAll(narrow16(name).c_str(), out, outSize, flags, alloc);
	}

	virtual bool Exist(const char* name) { return name && IsMemPath(name) ? find(normalize(name)) != NULL : (m_fallback ? m_fallback->Exist(name) : false); }
	virtual bool Exist(const wchar* name) { return Exist(narrow16(name).c_str()); }
	virtual uint GetLength(const char* name)
	{
		if (!name || !IsMemPath(name)) return m_fallback ? m_fallback->GetLength(name) : 0;
		MemFile* f = find(normalize(name));
		return f ? (uint)f->data.size() : 0;
	}
	virtual uint GetLength(const wchar* name) { return GetLength(narrow16(name).c_str()); }
	virtual FFileTimestamp GetLastWriteTime(const char* name) { return m_fallback ? m_fallback->GetLastWriteTime(name) : FFileTimestamp(); }
	virtual bool IsFileCatalogEnabled() { return false; }
	virtual FFileErrorHandler* GetErrorHandler() { return m_fallback ? m_fallback->GetErrorHandler() : NULL; }

private:
	FileErr ReadAllImpl(const char* name, byte** out, size_t* outSize, FIFile::Allocator* alloc)
	{
		MemFile* f = find(normalize(name));
		if (!f) return FILE_NOTFOUND;
		size_t n = f->data.size();
		byte* buf = alloc ? alloc->Allocate((uint)n) : new byte[n ? n : 1];
		if (n) memcpy(buf, &f->data[0], n);
		*out = buf;
		*outSize = n;
		return FILE_OK;
	}

	FIFileSystem* m_fallback;
};

bool g_installed = false;
RtFileSystem* g_fileSystem = NULL;

}  // namespace

// ---- public ------------------------------------------------------------------------------------------
bool Install()
{
	if (g_installed) return true;
	if (sqlite3_initialize() != SQLITE_OK) return false;
	if (sqlite3_vfs_register(&g_vfs, 1) != SQLITE_OK) return false;
	g_fileSystem = new RtFileSystem(&FIFileSystem::GetInstance());
	FIFileSystem::SetInstance(g_fileSystem);
	g_installed = true;
	return true;
}

void Reset()
{
	while (!files().empty()) remove_entry(files().begin()->first);
}

bool IsMemPath(const char* utf8)
{
	if (!utf8) return false;
	std::string key = normalize(utf8);
	size_t slash = key.rfind('/');
	if (slash != std::string::npos) key.erase(0, slash + 1);
	static const char kName[] = "civ5savedgamedatabase.db";
	return key.compare(0, sizeof(kName) - 1, kName) == 0;
}

bool Exists(const char* utf8) { return find(normalize(utf8)) != NULL; }

bool GetSize(const char* utf8, size_t* size)
{
	MemFile* f = find(normalize(utf8));
	if (!f) return false;
	*size = f->data.size();
	return true;
}

bool Remove(const char* utf8) { return remove_entry(normalize(utf8)); }

void Put(const char* utf8, const void* data, size_t size)
{
	std::string key = normalize(utf8);
	MemFile* f = find(key);
	if (!f) f = create_entry(key);
	f->data.assign((const unsigned char*)data, (const unsigned char*)data + size);
}

bool Get(const char* utf8, unsigned char** data, size_t* size)
{
	MemFile* f = find(normalize(utf8));
	if (!f) return false;
	*size = f->data.size();
	*data = new unsigned char[*size ? *size : 1];
	if (*size) memcpy(*data, &f->data[0], *size);
	return true;
}

bool Win32IsHandle(void* h) { return h && handles().count(h) != 0; }

void* Win32Open(const char* utf8, unsigned disposition, unsigned* lastError)
{
	std::string key = normalize(utf8);
	MemFile* f = find(key);
	switch (disposition)
	{
	case 1:  // CREATE_NEW
		if (f) { *lastError = 183; return NULL; }  // ERROR_ALREADY_EXISTS
		f = create_entry(key);
		break;
	case 2:  // CREATE_ALWAYS
		if (f) f->data.clear();
		else f = create_entry(key);
		break;
	case 4:  // OPEN_ALWAYS
		if (!f) f = create_entry(key);
		break;
	case 5:  // TRUNCATE_EXISTING
		if (!f) { *lastError = 2; return NULL; }
		f->data.clear();
		break;
	default:  // OPEN_EXISTING
		if (!f) { *lastError = 2; return NULL; }  // ERROR_FILE_NOT_FOUND
		break;
	}
	++f->refs;
	MemHandle* h = new MemHandle();
	h->file = f;
	h->pos = 0;
	handles().insert(h);
	return h;
}

bool Win32Read(void* h, void* buf, unsigned n, unsigned* got)
{
	MemHandle* m = (MemHandle*)h;
	std::vector<unsigned char>& d = m->file->data;
	size_t have = m->pos < d.size() ? d.size() - m->pos : 0;
	if (have > n) have = n;
	if (have) memcpy(buf, &d[m->pos], have);
	m->pos += have;
	if (got) *got = (unsigned)have;
	return true;
}

bool Win32Write(void* h, const void* buf, unsigned n, unsigned* put)
{
	MemHandle* m = (MemHandle*)h;
	std::vector<unsigned char>& d = m->file->data;
	if (m->pos + n > d.size()) d.resize(m->pos + n, 0);
	if (n) memcpy(&d[m->pos], buf, n);
	m->pos += n;
	if (put) *put = n;
	return true;
}

unsigned Win32Size(void* h) { return (unsigned)((MemHandle*)h)->file->data.size(); }

unsigned Win32Seek(void* h, long dist, int method)
{
	MemHandle* m = (MemHandle*)h;
	long base = method == 2 ? (long)m->file->data.size() : (method == 1 ? (long)m->pos : 0);
	long np = base + dist;
	if (np < 0) return 0xFFFFFFFFu;
	m->pos = (size_t)np;
	return (unsigned)np;
}

bool Win32Close(void* h)
{
	MemHandle* m = (MemHandle*)h;
	handles().erase(h);
	release(m->file);
	delete m;
	return true;
}

bool Win32Delete(const char* utf8, unsigned* lastError)
{
	if (remove_entry(normalize(utf8))) return true;
	*lastError = 2;  // ERROR_FILE_NOT_FOUND
	return false;
}

bool Win32Stat(const char* utf8, unsigned long long* size)
{
	MemFile* f = find(normalize(utf8));
	if (!f) return false;
	*size = f->data.size();
	return true;
}

}  // namespace rt_fs
}  // namespace lekmod_portable
