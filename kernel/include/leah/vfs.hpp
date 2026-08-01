#pragma once

#include <leah/types.hpp>

// The filesystem, as the kernel sees it.
//
// There is no filesystem behind this any more. Every call here is a message to
// vfsd, which reads and writes ext4, and which reaches the disk by asking
// blockd. What is left in the kernel is the file descriptor table, which is
// genuinely its own: a descriptor is an entry in a process's table, and whose
// table it is is a question about processes rather than about storage.
//
// Path-based rather than handle-based, which used to be a simplification and
// is now the point: it is what let the implementation move into another
// process without a single caller above this line changing.

namespace vfs {

constexpr usize kMaxName = 256;
constexpr usize kMaxPath = 1024;

enum class Type : u8 {
    File,
    Directory,
};

struct Stat {
    Type type;
    u64  size;
    // Ownership and permission bits, in the usual UNIX encoding. A filesystem
    // that cannot store them (FAT32) reports a fixed, permissive default; ext
    // reports what is really on disk, which is why it is the root filesystem.
    u16  mode;
    u32  uid;
    u32  gid;
};

// Permission bits within Stat::mode.
constexpr u16 kModeOtherExec  = 0001;
constexpr u16 kModeOtherWrite = 0002;
constexpr u16 kModeOtherRead  = 0004;
constexpr u16 kModeGroupExec  = 0010;
constexpr u16 kModeGroupWrite = 0020;
constexpr u16 kModeGroupRead  = 0040;
constexpr u16 kModeOwnerExec  = 0100;
constexpr u16 kModeOwnerWrite = 0200;
constexpr u16 kModeOwnerRead  = 0400;
constexpr u16 kModePermissionBits = 0777;

struct Entry {
    char name[kMaxName];
    Type type;
    u64  size;
};

bool  stat(const char* path, Stat& out);
isize read(const char* path, u64 offset, void* buffer, usize bytes);
bool  list(const char* path, Entry* out, usize max, usize& count);
isize write(const char* path, u64 offset, const void* buffer, usize bytes);
bool  create(const char* path, Type type);
bool  remove(const char* path);
bool  rename(const char* old_path, const char* new_path);
bool  chmod(const char* path, u16 mode);
bool  chown(const char* path, u32 uid, u32 gid);

// Create (or truncate) a file and write the whole buffer in one call.
bool write_entire_file(const char* path, const void* buffer, usize bytes);

// Convenience: allocates, reads the whole file, NUL-terminates. Caller frees.
char* read_entire_file(const char* path, u64* size_out);

} // namespace vfs
