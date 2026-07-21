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
};

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

// Create (or truncate) a file and write the whole buffer in one call.
bool write_entire_file(const char* path, const void* buffer, usize bytes);

// Convenience: allocates, reads the whole file, NUL-terminates. Caller frees.
char* read_entire_file(const char* path, u64* size_out);

} // namespace vfs
