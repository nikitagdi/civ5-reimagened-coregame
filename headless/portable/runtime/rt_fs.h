// rt_fs: in-memory files for ./Civ5SavedGameDatabase.db (rt_fs).
// The gamecore reaches that file three ways, all served from one memory file table here:
//   - sqlite (Database::Connection::Open): a memory VFS registered as the default VFS (Install);
//   - Win32 (CreateFileW/ReadFile/WriteFile/DeleteFileW in Win32Compat.cpp): the Win32* hooks below;
//   - FIFileSystem (FFILESYSTEM.Create in CvGame::Read): RtFileSystem, installed by Install.
// Nothing touches the host file system; the saved-game DB lives and dies with the process.
#pragma once

#include <stddef.h>

namespace lekmod_portable {
namespace rt_fs {

// Registers the memory VFS as sqlite's default and installs the FIFileSystem. Idempotent.
// Call before the first Database::Connection that opens a file name.
bool Install();

// Drops every memory file (a fresh game instance starts without a saved-game DB).
void Reset();

// True for the files rt_fs serves through the Win32/FIFileSystem paths: the final path component starts with
// "Civ5SavedGameDatabase.db" (the database and its -journal), case-insensitive, any directory and slash style.
// (The sqlite VFS serves every name from memory.)
bool IsMemPath(const char* utf8);

// Whole-file access for hosts and tests.
bool Exists(const char* utf8);
bool GetSize(const char* utf8, size_t* size);
bool Remove(const char* utf8);
// Replaces the content of a (possibly new) file.
void Put(const char* utf8, const void* data, size_t size);
// Copies the content out; returns false when the file does not exist.
bool Get(const char* utf8, unsigned char** data, size_t* size);  // *data: new[]; caller delete[]s

// ---- Win32 handle hooks (Win32Compat.cpp). Handles are opaque; IsHandle tells them from FILE*. ----
bool Win32IsHandle(void* h);
// disposition: Win32 CREATE_NEW(1) / CREATE_ALWAYS(2) / OPEN_EXISTING(3) / OPEN_ALWAYS(4) / TRUNCATE_EXISTING(5).
// Returns NULL on failure and sets *lastError to the Win32 code.
void* Win32Open(const char* utf8, unsigned disposition, unsigned* lastError);
bool Win32Read(void* h, void* buf, unsigned n, unsigned* got);
bool Win32Write(void* h, const void* buf, unsigned n, unsigned* put);
unsigned Win32Size(void* h);
// method: Win32 FILE_BEGIN(0) / FILE_CURRENT(1) / FILE_END(2). Returns the new position or 0xFFFFFFFF.
unsigned Win32Seek(void* h, long dist, int method);
bool Win32Close(void* h);
bool Win32Delete(const char* utf8, unsigned* lastError);
// File size for GetFileAttributesEx (false if absent).
bool Win32Stat(const char* utf8, unsigned long long* size);

}  // namespace rt_fs
}  // namespace lekmod_portable
