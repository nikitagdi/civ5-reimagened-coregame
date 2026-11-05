// rt_db: the game database from memory buffers (rt_db).
// The gameplay DB snapshot is copied into the connection with sqlite3_deserialize; the en_US localization DB
// is attached as "Localization" the same way. Ported from headless/host/src/data/HstDatabase.cpp, with
// buffers in place of files.
#pragma once

#include <stddef.h>

#include "CvGameDatabase.h"

struct sqlite3;

namespace lekmod_portable {

class RtDatabase
{
public:
	RtDatabase();
	~RtDatabase();

	// Opens ":memory:" and loads the gameplay DB image through Database::BinaryIO::Load.
	bool OpenGameplay(const void* image, size_t size);
	// ATTACH ':memory:' AS Localization and fills it from the localization DB image (table Language_<type>).
	bool AttachLocalization(const void* image, size_t size);
	// Localization::Initialize for the attached DB.
	bool InitLocalization(const char* language);

	Database::Connection* Connection() { return m_db; }
	int TableCount();                 // user tables of the gameplay DB (main schema)
	int Count(const char* table);     // select count(*) from <table>, -1 on error

private:
	Database::Connection* m_db;
	bool m_localization;
};

namespace rt_db {

// sqlite3_deserialize of a copy of the image into schema (e.g. "main", "Localization"); the copy is owned by
// sqlite and freed with the connection. readOnly images reject writes.
bool Deserialize(sqlite3* db, const char* schema, const void* image, size_t size, bool readOnly);

// BinaryIO::Load looks the file name up here first: a name registered with a buffer is loaded from memory,
// anything else is a file. The buffer must stay valid until UnregisterImage.
void RegisterImage(const char* name, const void* image, size_t size);
void UnregisterImage(const char* name);
bool FindImage(const char* name, const void** image, size_t* size);

}  // namespace rt_db
}  // namespace lekmod_portable
