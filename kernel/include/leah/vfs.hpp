#pragma once

#include <leah/types.hpp>

// The filesystem-independent layer.
//
// Deliberately path-based rather than handle-based for now: open/close/seek
// needs a file descriptor table, and that belongs with processes rather than
// ahead of them. FAT32 is the first implementation; exFAT and ext2/3/4 slot in
// behind the same interface.

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

class FileSystem {
public:
    virtual ~FileSystem() = default;

    virtual const char* type_name() const = 0;
    virtual const char* volume_label() const = 0;

    virtual bool stat(const char* path, Stat& out) = 0;

    // Returns bytes read, or -1 on error. A short read means end of file.
    virtual isize read(const char* path, u64 offset, void* buffer, usize bytes) = 0;

    virtual bool list(const char* path, Entry* out, usize max, usize& count) = 0;

    // Writing past the end extends the file. Returns bytes written, or -1.
    virtual isize write(const char* path, u64 offset, const void* buffer, usize bytes) = 0;

    virtual bool create(const char* path, Type type) = 0;
    virtual bool remove(const char* path) = 0;

    // Move an entry to a new path, no data copy. Default: unsupported.
    virtual bool rename(const char* old_path, const char* new_path)
    {
        (void)old_path;
        (void)new_path;
        return false;
    }

    // Change permission bits / ownership. Default: unsupported, which is the
    // honest answer for a filesystem with nowhere to record them.
    virtual bool chmod(const char* path, u16 mode)
    {
        (void)path;
        (void)mode;
        return false;
    }
    virtual bool chown(const char* path, u32 uid, u32 gid)
    {
        (void)path;
        (void)uid;
        (void)gid;
        return false;
    }
};

// Single root mount. A real mount table arrives with the second filesystem.
void mount(FileSystem* filesystem);
FileSystem* mounted();

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
