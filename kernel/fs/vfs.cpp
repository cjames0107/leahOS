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
