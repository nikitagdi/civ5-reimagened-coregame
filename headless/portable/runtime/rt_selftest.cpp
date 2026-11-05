// Self-checks behind the harness scenarios (run.sh --scenario db). Exercises rt_db and rt_fs through the same
// gamecore calls the game makes (CvGame.cpp reset / Write / Read, Database::Connection, FFILESYSTEM).
#include "CvGameCoreDLLPCH.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "CvLocalization.h"
#include "FireWorks/FFileDefines.h"
#include "FireWorks/FIFile.h"
#include "rt_db.h"
#include "rt_fs.h"
#include "rt_selftest.h"
#include "lekmod_portable/host_log.h"

using namespace lekmod_portable;

namespace {

struct Check
{
	std::string json;
	int failed;
	Check() : failed(0) {}

	void add(const char* name, bool ok, const char* fmt, ...)
	{
		char detail[256];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(detail, sizeof(detail), fmt, ap);
		va_end(ap);
		char line[400];
		snprintf(line, sizeof(line), "%s{\"check\":\"%s\",\"ok\":%s,\"detail\":\"%s\"}", json.empty() ? "" : ",", name, ok ? "true" : "false", detail);
		json += line;
		printf("db: %-4s %s: %s\n", ok ? "ok" : "FAIL", name, detail);
		if (!ok) ++failed;
	}
};

bool read_game_file(const gc_host* host, const char* path, std::vector<unsigned char>& out)
{
	gc_buf buf = {0, 0, 0, 0};
	if (host->read_file(host->user, path, &buf) != GC_OK) return false;
	out.assign((const unsigned char*)buf.data, (const unsigned char*)buf.data + buf.len);
	gc_buf_release(&buf);
	return true;
}

int scalar_int(Database::Connection& db, const char* sql)
{
	Database::Results r;
	if (!db.Execute(r, sql) || !r.Step()) return -1;
	return r.GetInt(0);
}

}  // namespace

extern "C" int rt_selftest_db(const gc_host* host, char* report, size_t report_len)
{
	Check c;
	std::vector<unsigned char> gameplay, loc;
	const bool haveGameplay = read_game_file(host, "db/gameplay.db", gameplay);
	const bool haveLoc = read_game_file(host, "db/loc-en_us.db", loc);
	c.add("read_gameplay_db", haveGameplay, "%u bytes", (unsigned)gameplay.size());
	c.add("read_loc_db", haveLoc, "%u bytes", (unsigned)loc.size());

	int tables = -1;
	if (haveGameplay && haveLoc)
	{
		RtDatabase rdb;
		bool opened = rdb.OpenGameplay(&gameplay[0], gameplay.size());
		c.add("open_gameplay_from_buffer", opened, "sqlite3_deserialize via BinaryIO::Load");
		if (opened)
		{
			tables = rdb.TableCount();
			c.add("table_count", tables == 427, "%d tables (expect 427)", tables);
			c.add("units", rdb.Count("Units") > 0, "Units=%d", rdb.Count("Units"));
			c.add("civilizations", rdb.Count("Civilizations") > 0, "Civilizations=%d", rdb.Count("Civilizations"));
			c.add("map_sizes", rdb.Count("Map_Sizes") == 70, "Map_Sizes=%d (expect 70)", rdb.Count("Map_Sizes"));
			c.add("map_scripts", rdb.Count("MapScripts") > 0, "MapScripts=%d", rdb.Count("MapScripts"));
			Database::Results w;
			bool warrior = rdb.Connection()->Execute(w, "select ID from Units where Type = 'UNIT_WARRIOR'") && w.Step();
			c.add("unit_warrior", warrior, "Units.UNIT_WARRIOR ID=%d", warrior ? w.GetInt(0) : -1);

			bool attached = rdb.AttachLocalization(&loc[0], loc.size());
			c.add("attach_localization", attached, "ATTACH + deserialize as Localization");
			if (attached)
			{
				int rows = scalar_int(*rdb.Connection(), "select count(*) from Localization.Language_en_US");
				c.add("localization_rows", rows == 30012, "Language_en_US=%d (expect 30012)", rows);
				bool init = rdb.InitLocalization("en_US");
				std::string text = init ? std::string(Localization::Lookup("TXT_KEY_UNIT_WARRIOR").toUTF8()) : std::string();
				c.add("localization_lookup", init && !text.empty() && text != "TXT_KEY_UNIT_WARRIOR", "TXT_KEY_UNIT_WARRIOR -> '%s'", text.c_str());
			}
		}
	}

	// ---- rt_fs: ./Civ5SavedGameDatabase.db through sqlite, Win32 and FIFileSystem -----------------------
	c.add("fs_install", rt_fs::Install(), "memory VFS default + FIFileSystem");
	rt_fs::Reset();
	const char* kPath = "./Civ5SavedGameDatabase.db";  // GetCacheFolderPath() + "Civ5SavedGameDatabase.db"
	std::wstring wpath = L"./Civ5SavedGameDatabase.db";

	// CvGame::reset
	SetLastError(0);
	BOOL del = DeleteFileW(wpath.c_str());
	c.add("fs_reset_delete_absent", del == FALSE && GetLastError() == ERROR_FILE_NOT_FOUND, "DeleteFileW on absent file");
	{
		Database::Connection db;
		bool opened = db.Open(kPath, Database::OPEN_CREATE | Database::OPEN_READWRITE | Database::OPEN_FULLMUTEX);
		bool created = opened && db.Execute("CREATE TABLE SimpleValues(Name TEXT Primary Key, Value VARIANT)");
		bool inserted = created && db.Execute("INSERT INTO SimpleValues(Name, Value) VALUES('answer', 42)");
		c.add("fs_sqlite_create", inserted, "CREATE TABLE + INSERT in %s", kPath);
	}
	size_t memSize = 0;
	c.add("fs_in_memory", rt_fs::GetSize(kPath, &memSize) && memSize > 0, "%u bytes in rt_fs", (unsigned)memSize);
	{
		FILE* f = fopen("Civ5SavedGameDatabase.db", "rb");  // must not exist on the host file system
		c.add("fs_not_on_disk", f == NULL, "no host file");
		if (f) fclose(f);
	}

	// CvGame::Write
	std::vector<unsigned char> blob;
	HANDLE h = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
	if (h != INVALID_HANDLE_VALUE)
	{
		DWORD size = GetFileSize(h, NULL), got = 0;
		blob.resize(size);
		BOOL read = size ? ReadFile(h, &blob[0], size, &got, NULL) : TRUE;
		CloseHandle(h);
		if (!read || got != size) blob.clear();
	}
	c.add("fs_win32_read", !blob.empty() && blob.size() == memSize && !memcmp(&blob[0], "SQLite format 3", 15), "CreateFileW/GetFileSize/ReadFile -> %u bytes", (unsigned)blob.size());

	// CvGame::Read: reset() deletes the file, then FFILESYSTEM.Create(modeWrite) writes the embedded copy back
	c.add("fs_win32_delete", DeleteFileW(wpath.c_str()) != FALSE && !rt_fs::Exists(kPath), "DeleteFileW of existing file");
	FIFile* pkFile = FIFileSystem::GetInstance().Create(wpath.c_str(), FIFile::modeWrite);
	bool wrote = false;
	if (pkFile)
	{
		wrote = !blob.empty() && pkFile->Write(&blob[0], (uint)blob.size()) == blob.size();
		pkFile->Close();
		delete pkFile;
	}
	c.add("fs_fifile_write", wrote && FIFileSystem::GetInstance().Exist(wpath.c_str()) && FIFileSystem::GetInstance().GetLength(wpath.c_str()) == blob.size(),
		"FFILESYSTEM.Create(modeWrite) + Write");
	{
		Database::Connection db;
		bool opened = db.Open(".\\Civ5SavedGameDatabase.db", Database::OPEN_READWRITE | Database::OPEN_FULLMUTEX);  // Modding.OpenSaveData spelling
		int answer = opened ? scalar_int(db, "select Value from SimpleValues where Name = 'answer'") : -1;
		c.add("fs_sqlite_reopen", answer == 42, "SimpleValues.answer=%d (expect 42)", answer);
	}

	char head[96];
	snprintf(head, sizeof(head), "{\"tables\":%d,\"failed\":%d,\"checks\":[", tables, c.failed);
	std::string out = std::string(head) + c.json + "]}";
	if (report && report_len)
	{
		strncpy(report, out.c_str(), report_len - 1);
		report[report_len - 1] = '\0';
	}
	return c.failed == 0 ? 0 : 1;
}
