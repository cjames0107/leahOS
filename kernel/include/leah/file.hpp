#pragma once

#include <leah/types.hpp>

// Per-process open files and working directory, and the operations the file
// syscalls are built from. The VFS underneath is path-based, so an open file
// descriptor is just a remembered path plus a cursor - no VFS handle to leak.

namespace files {

constexpr int   kMaxFds   = 16;
constexpr usize kPathMax  = 128;

enum class Kind : u8 {
    None,
    ConsoleIn,      // fd 0: the keyboard
    ConsoleOut,     // fd 1, 2: the console
    File,
};

struct Descriptor {
    Kind  kind;
    u64   offset;
    u32   flags;
    char  path[kPathMax];
};

// A process's file table and its current directory, carried in its task.
struct Table {
    Descriptor fds[kMaxFds];
    char       cwd[kPathMax];
};

// open() flags. Small subset, values chosen to be our own.
constexpr u32 kRead   = 1 << 0;
constexpr u32 kWrite  = 1 << 1;
constexpr u32 kCreate = 1 << 2;
constexpr u32 kTrunc  = 1 << 3;
constexpr u32 kAppend = 1 << 4;

// Set up a brand-new table: stdin/stdout/stderr wired to the console, cwd "/".
void init_table(Table& table);

// The dirent a getdents call fills for user space. Kept flat and fixed-size so
// it copies straight out to a user array.
struct [[gnu::packed]] Dirent {
    u32  type;      // 0 = file, 1 = directory
    u32  reserved;
    u64  size;
    char name[kPathMax];
};

// These operate on the calling process's table (fetched from the scheduler) and
// on user pointers in the active address space. Return values follow the usual
// convention: a non-negative result, or -1 on error.
i64 open(const char* path, u32 flags);
i64 close(int fd);
i64 read(int fd, void* buffer, usize count);
i64 write(int fd, const void* buffer, usize count);
i64 lseek(int fd, i64 offset, int whence);
i64 stat(const char* path, void* statbuf);
i64 getdents(const char* path, void* buffer, usize max_entries);
i64 chdir(const char* path);
i64 getcwd(char* buffer, usize size);
i64 mkdir(const char* path);
i64 unlink(const char* path);

} // namespace files
