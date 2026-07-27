#include <leah/heap.hpp>
#include <leah/vfs.hpp>

namespace vfs {
namespace {

FileSystem* g_root = nullptr;

} // namespace

void mount(FileSystem* filesystem) { g_root = filesystem; }

FileSystem* mounted() { return g_root; }

bool stat(const char* path, Stat& out)
{
    return g_root != nullptr && g_root->stat(path, out);
}

isize read(const char* path, u64 offset, void* buffer, usize bytes)
{
    return g_root != nullptr ? g_root->read(path, offset, buffer, bytes) : -1;
}

bool list(const char* path, Entry* out, usize max, usize& count)
{
    count = 0;
    return g_root != nullptr && g_root->list(path, out, max, count);
}

isize write(const char* path, u64 offset, const void* buffer, usize bytes)
{
    return g_root != nullptr ? g_root->write(path, offset, buffer, bytes) : -1;
}

bool create(const char* path, Type type)
{
    return g_root != nullptr && g_root->create(path, type);
}

bool remove(const char* path)
{
    return g_root != nullptr && g_root->remove(path);
}

bool rename(const char* old_path, const char* new_path)
{
    return g_root != nullptr && g_root->rename(old_path, new_path);
}

bool chmod(const char* path, u16 mode)
{
    return g_root != nullptr && g_root->chmod(path, mode);
}

bool chown(const char* path, u32 uid, u32 gid)
{
    return g_root != nullptr && g_root->chown(path, uid, gid);
}

bool write_entire_file(const char* path, const void* buffer, usize bytes)
{
    // Removing first rather than truncating keeps this honest: a shorter
    // rewrite must not leave the tail of the previous contents behind.
    Stat existing{};
    if (stat(path, existing))
        remove(path);

    if (!create(path, Type::File))
        return false;
    if (bytes == 0)
        return true;

    const isize written = write(path, 0, buffer, bytes);
    return written >= 0 && static_cast<usize>(written) == bytes;
}

char* read_entire_file(const char* path, u64* size_out)
{
    Stat info{};
    if (!stat(path, info) || info.type != Type::File)
        return nullptr;

    // One extra byte so the result is always safe to treat as a C string.
    auto* buffer = static_cast<char*>(kmalloc(info.size + 1));
    if (buffer == nullptr)
        return nullptr;

    const isize got = read(path, 0, buffer, info.size);
    if (got < 0) {
        kfree(buffer);
        return nullptr;
    }

    buffer[got] = '\0';
    if (size_out != nullptr)
        *size_out = static_cast<u64>(got);
    return buffer;
}

} // namespace vfs
