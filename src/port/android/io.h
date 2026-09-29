// The Windows CRT's low-level file functions (io.h), for the Android build: wide paths are
// converted to UTF-8 and passed to POSIX. See windows.h next to this file.
#pragma once
#include <windows.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>

#define _O_RDONLY O_RDONLY
#define _O_WRONLY O_WRONLY
#define _O_RDWR O_RDWR
#define _O_APPEND O_APPEND
#define _O_CREAT O_CREAT
#define _O_TRUNC O_TRUNC
#define _O_EXCL O_EXCL
#define _O_BINARY 0
#define _O_NOINHERIT O_CLOEXEC
#define _S_IREAD 0400
#define _S_IWRITE 0200
#define _S_IFMT S_IFMT
#define _S_IFDIR S_IFDIR
#define _stat64 stat
#define _fstat64 fstat

int _wopen(const wchar_t* path, int flags, int mode);
int _wstat64(const wchar_t* path, struct stat* s);
int _waccess(const wchar_t* path, int mode);
int _wmkdir(const wchar_t* path);
int _wrmdir(const wchar_t* path);
int _wunlink(const wchar_t* path);
FILE* _wfopen(const wchar_t* path, const wchar_t* mode);
inline int _close(int fd) { return close(fd); }
inline int _read(int fd, void* buf, unsigned n) { return (int)read(fd, buf, n); }
inline int _write(int fd, const void* buf, unsigned n) { return (int)write(fd, buf, n); }
inline int64_t _lseeki64(int fd, int64_t off, int whence) { return lseek64(fd, off, whence); }
inline int _commit(int fd) { return fsync(fd); }

// Directory listing and renames (Win32 names, as the file HLE uses them).
#define INVALID_HANDLE_VALUE (reinterpret_cast<HANDLE>(-1))
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
#define MOVEFILE_REPLACE_EXISTING 0x1u
struct WIN32_FIND_DATAW {
    DWORD dwFileAttributes;
    wchar_t cFileName[MAX_PATH];
};
HANDLE FindFirstFileW(LPCWSTR pattern, WIN32_FIND_DATAW* data);  // pattern: "<dir>\*"
BOOL FindNextFileW(HANDLE find, WIN32_FIND_DATAW* data);
BOOL FindClose(HANDLE find);
BOOL MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags);
