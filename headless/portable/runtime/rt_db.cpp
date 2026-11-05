// Ported from headless/host/src/data/HstDatabase.cpp (buffers in place of files)
// The FireWorks headers expect the gamecore precompiled-header environment.
#include "CvGameCoreDLLPCH.h"

#include <string.h>

#include <map>
#include <string>

#include <sqlite3.h>

#include "CvLocalization.h"
#include "rt_db.h"
#include "lekmod_portable/host_log.h"

namespace lekmod_portable {

namespace rt_db {

namespace {
typedef std::map<std::string, std::pair<const void*, size_t> > ImageMap;
ImageMap& images()
{
	static ImageMap s_images;
	return s_images;
}
}  // namespace

void RegisterImage(const char* name, const void* image, size_t size) { images()[name] = std::make_pair(image, size); }
void UnregisterImage(const char* name) { images().erase(name); }

bool FindImage(const char* name, const void** image, size_t* size)
{
	if (!name) return false;
	ImageMap::const_iterator it = images().find(name);
	if (it == images().end()) return false;
	*image = it->second.first;
	*size = it->second.second;
	return true;
}

bool Deserialize(sqlite3* db, const char* schema, const void* image, size_t size, bool readOnly)
{
	if (!db || !image || size == 0) return false;
	unsigned char* copy = (unsigned char*)sqlite3_malloc64((sqlite3_uint64)size);
	if (!copy) return false;
	memcpy(copy, image, size);
	unsigned flags = SQLITE_DESERIALIZE_FREEONCLOSE | (readOnly ? SQLITE_DESERIALIZE_READONLY : SQLITE_DESERIALIZE_RESIZEABLE);
	// On failure sqlite frees the copy itself (FREEONCLOSE).
	int rc = sqlite3_deserialize(db, schema, copy, (sqlite3_int64)size, (sqlite3_int64)size, flags);
	if (rc != SQLITE_OK)
	{
		Log(LOG_ERROR, "db: sqlite3_deserialize(%s) failed: %s", schema, sqlite3_errstr(rc));
		return false;
	}
	return true;
}

}  // namespace rt_db

RtDatabase::RtDatabase() : m_db(NULL), m_localization(false) {}

RtDatabase::~RtDatabase()
{
	if (m_localization) Localization::ShutdownDictionary();
	delete m_db;
}

bool RtDatabase::OpenGameplay(const void* image, size_t size)
{
	// SIMPLIFIED(SIMP-016): the gameplay DB is a snapshot the real game built (Civ5DebugDatabase.db), not assembled from XML/SQL.
	static const char kName[] = "rt_db:gameplay";
	m_db = new Database::Connection();
	if (!m_db->Open(":memory:", Database::OPEN_READWRITE | Database::OPEN_CREATE))
	{
		Log(LOG_ERROR, "db: Open(:memory:) failed: %s", m_db->ErrorMessage() ? m_db->ErrorMessage() : "?");
		return false;
	}
	rt_db::RegisterImage(kName, image, size);
	Database::BinaryIO io(kName);
	bool ok = io.Load(*m_db);
	rt_db::UnregisterImage(kName);
	if (!ok)
	{
		Log(LOG_ERROR, "db: BinaryIO::Load(gameplay image, %u bytes) failed", (unsigned)size);
		return false;
	}
	// Temp tables and sorts stay in memory (nothing to spill to).
	m_db->Execute("PRAGMA temp_store = MEMORY");
	return true;
}

bool RtDatabase::AttachLocalization(const void* image, size_t size)
{
	if (!m_db) return false;
	// SIMPLIFIED(SIMP-017): one language image (en_US) is attached, not the merged all-language localization DB.
	if (!m_db->Execute("ATTACH ':memory:' AS Localization")) return false;
	return rt_db::Deserialize(m_db->GetSQLite3(), "Localization", image, size, true);
}

bool RtDatabase::InitLocalization(const char* language)
{
	if (!m_db) return false;
	m_localization = Localization::Initialize(L"", *m_db, "Languages", language, false);
	return m_localization;
}

int RtDatabase::TableCount()
{
	Database::Results r;
	if (!m_db || !m_db->Execute(r, "select count(*) from sqlite_master where type = 'table'") || !r.Step()) return -1;
	return r.GetInt(0);
}

int RtDatabase::Count(const char* table) { return m_db ? m_db->Count(table, false) : -1; }

}  // namespace lekmod_portable
