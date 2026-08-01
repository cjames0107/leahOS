#include <leah/heap.hpp>
#include <leah/ipc.hpp>
#include <leah/memory.hpp>
#include <leah/shm.hpp>
#include <leah/string.hpp>
#include <leah/vfs.hpp>

// The filesystem, from the kernel's side of the boundary.
//
// There is no filesystem in here any more. Every one of these calls is a
// message to vfsd, which reads ext4, and which reaches the disk by asking
// blockd, which owns the drive. The kernel's part is the message passing and
// the file descriptor table - which is the part that is genuinely its own,
// because a descriptor is an entry in a process's table and whose table it is
// belongs to the kernel.
//
// The interface above this file did not change when the implementation moved
// out. That is the whole reason it was worth having: files.cpp, accounts.cpp
// and the ELF loader are the same code they were when ext4 was compiled in.

namespace vfs {
namespace {

// Mirrors <vfsd.h> in the userland tree. Two copies of a protocol is one copy
// too many, but the kernel cannot include a userland header and this is the
// smaller wrong than a build-time contortion to share one.
constexpr u32 kShmKey = 0x5646u;
constexpr usize kChunk = 8192;

constexpr u32 kStat = 1, kRead = 2, kList = 3, kMounted = 4;
constexpr u32 kCreate = 5, kWrite = 6, kMkdir = 7, kUnlink = 8;
constexpr u32 kChmod = 10, kChown = 11;

constexpr i64 kKindDir = 1;

i32 g_port = -1;
i32 g_shm  = -1;

// The server's transfer buffer, reached through the direct map. The kernel does
// not map it into any address space: it owns the physical frames' addresses
// already, which is the one advantage of being the thing that handed them out.
u8* shared_at(u64 offset)
{
    if (g_shm < 0)
        return nullptr;
    const usize page = static_cast<usize>(offset / 4096);
    const paddr_t frame = shm::frame_of(g_shm, page);
    if (frame == 0)
        return nullptr;
    return reinterpret_cast<u8*>(memory::phys_to_direct(frame)) + (offset % 4096);
}

bool connect()
{
    if (g_port >= 0)
        return true;
    const i64 port = ipc::port_open(ipc::kPortVfs);
    if (port < 0)
        return false;
    g_port = static_cast<i32>(port);
    // Root, because the kernel is asking on behalf of whoever called - the
    // permission check has already happened a layer up in files.cpp.
    g_shm = shm::open(kShmKey, kChunk, 0, 0);
    return g_shm >= 0;
}

// Fill a message with a path and send it.
bool ask(u32 tag, const char* path, i64 w1, i64 w2, ipc::Message& reply)
{
    if (!connect())
        return false;
    ipc::Message request{};
    request.tag = tag;
    request.word[1] = w1;
    request.word[2] = w2;
    usize n = 0;
    while (path != nullptr && path[n] != '\0' && n < sizeof(request.data) - 1) {
        request.data[n] = path[n];
        ++n;
    }
    request.data[n] = '\0';
    request.bytes = static_cast<u32>(n);
    return ipc::call(g_port, &request, &reply) == 0;
}

} // namespace

bool stat(const char* path, Stat& out)
{
    ipc::Message reply{};
    if (!ask(kStat, path, 0, 0, reply) || reply.word[0] < 0)
        return false;
    out.size = static_cast<u64>(reply.word[0]);
    out.type = reply.word[1] == kKindDir ? Type::Directory : Type::File;
    out.mode = static_cast<u16>(reply.word[2]);
    out.uid  = static_cast<u32>(reply.word[3]) >> 16;
    out.gid  = static_cast<u32>(reply.word[3]) & 0xFFFF;
    return true;
}

isize read(const char* path, u64 offset, void* buffer, usize bytes)
{
    auto* out = static_cast<u8*>(buffer);
    usize done = 0;
    while (done < bytes) {
        usize want = bytes - done;
        if (want > kChunk)
            want = kChunk;
        ipc::Message reply{};
        if (!ask(kRead, path, static_cast<i64>(offset + done),
                 static_cast<i64>(want), reply) || reply.word[0] < 0)
            return done > 0 ? static_cast<isize>(done) : -1;
        const usize got = static_cast<usize>(reply.word[0]);
        if (got == 0)
            break;                          // the end of the file
        for (usize i = 0; i < got; ++i) {
            const u8* src = shared_at(i);
            if (src == nullptr)
                return -1;
            out[done + i] = *src;
        }
        done += got;
        if (got < want)
            break;
    }
    return static_cast<isize>(done);
}

isize write(const char* path, u64 offset, const void* buffer, usize bytes)
{
    const auto* in = static_cast<const u8*>(buffer);
    usize done = 0;
    while (done < bytes) {
        usize want = bytes - done;
        if (want > kChunk)
            want = kChunk;
        for (usize i = 0; i < want; ++i) {
            u8* dst = shared_at(i);
            if (dst == nullptr)
                return -1;
            *dst = in[done + i];
        }
        ipc::Message reply{};
        if (!ask(kWrite, path, static_cast<i64>(offset + done),
                 static_cast<i64>(want), reply) || reply.word[0] <= 0)
            return done > 0 ? static_cast<isize>(done) : -1;
        done += static_cast<usize>(reply.word[0]);
    }
    return static_cast<isize>(done);
}

bool list(const char* path, Entry* out, usize max, usize& count)
{
    count = 0;
    for (usize i = 0; i < max; ++i) {
        ipc::Message reply{};
        if (!ask(kList, path, static_cast<i64>(i), 0, reply) || reply.word[0] < 0)
            break;
        usize n = 0;
        while (n < kMaxName - 1 && reply.data[n] != '\0') {
            out[count].name[n] = reply.data[n];
            ++n;
        }
        out[count].name[n] = '\0';
        out[count].type = reply.word[0] == kKindDir ? Type::Directory : Type::File;
        out[count].size = 0;
        ++count;
    }
    return true;
}

bool create(const char* path, Type type)
{
    ipc::Message reply{};
    return ask(type == Type::Directory ? kMkdir : kCreate, path, 0, 0, reply) &&
           reply.word[0] == 0;
}

bool remove(const char* path)
{
    ipc::Message reply{};
    return ask(kUnlink, path, 0, 0, reply) && reply.word[0] == 0;
}

bool rename(const char* old_path, const char* new_path)
{
    // Not a move on disk yet: copy, then unlink. Correct and slow, and honest
    // about which - a rename that silently did nothing would be worse.
    Stat info{};
    if (!stat(old_path, info) || info.type != Type::File)
        return false;
    auto* data = static_cast<u8*>(kmalloc(info.size > 0 ? info.size : 1));
    if (data == nullptr)
        return false;
    const isize got = read(old_path, 0, data, info.size);
    bool ok = got >= 0 && static_cast<u64>(got) == info.size &&
              create(new_path, Type::File) &&
              (info.size == 0 || write(new_path, 0, data, info.size) ==
                                     static_cast<isize>(info.size)) &&
              remove(old_path);
    kfree(data);
    return ok;
}

bool chmod(const char* path, u16 mode)
{
    ipc::Message reply{};
    return ask(kChmod, path, mode, 0, reply) && reply.word[0] == 0;
}

bool chown(const char* path, u32 uid, u32 gid)
{
    ipc::Message reply{};
    return ask(kChown, path, uid, gid, reply) && reply.word[0] == 0;
}

bool write_entire_file(const char* path, const void* buffer, usize bytes)
{
    Stat info{};
    if (!stat(path, info) && !create(path, Type::File))
        return false;
    if (bytes == 0)
        return true;
    return write(path, 0, buffer, bytes) == static_cast<isize>(bytes);
}

char* read_entire_file(const char* path, u64* size_out)
{
    Stat info{};
    if (!stat(path, info) || info.type != Type::File)
        return nullptr;
    auto* text = static_cast<char*>(kmalloc(info.size + 1));
    if (text == nullptr)
        return nullptr;
    const isize got = read(path, 0, text, info.size);
    if (got < 0) {
        kfree(text);
        return nullptr;
    }
    text[got] = '\0';
    if (size_out != nullptr)
        *size_out = static_cast<u64>(got);
    return text;
}

} // namespace vfs
