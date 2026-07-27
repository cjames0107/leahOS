#include <leah/console.hpp>
#include <leah/cpu.hpp>
#include <leah/file.hpp>
#include <leah/heap.hpp>
#include <leah/keyboard.hpp>
#include <leah/scheduler.hpp>
#include <leah/string.hpp>
#include <leah/tcp.hpp>
#include <leah/usb_hid.hpp>
#include <leah/vfs.hpp>

namespace files {
namespace {

// The layout stat() writes to the user statbuf; mirrored by struct stat in the
// libc's sys/stat.h.
struct [[gnu::packed]] UserStat {
    u32 type;       // 0 = file, 1 = directory
    u32 mode;       // permission bits, 0777
    u64 size;
    u32 uid;
    u32 gid;
};

Table& table() { return scheduler::current_files(); }

bool valid_fd(int fd) { return fd >= 0 && fd < kMaxFds; }

// --- pipes -----------------------------------------------------------------
//
// A bounded ring buffer with counts of how many descriptors hold each end.
// Readers sleep on the read channel and writers wake them; writers sleep on the
// write channel and readers wake them. When the last writer closes, a blocked
// reader wakes to an end-of-file; when the last reader closes, a write fails.

struct Pipe {
    static constexpr usize kSize = 4096;
    u8    buffer[kSize];
    usize head;         // next read
    usize tail;         // next write
    usize count;
    int   readers;
    int   writers;
};

u64 read_channel(Pipe* p)  { return reinterpret_cast<u64>(p); }
u64 write_channel(Pipe* p) { return reinterpret_cast<u64>(p) + 1; }

i64 pipe_read(Pipe* p, void* buffer, usize count)
{
    auto* out = static_cast<u8*>(buffer);
    for (;;) {
        if (p->count > 0) {
            usize n = 0;
            while (n < count && p->count > 0) {
                out[n++] = p->buffer[p->head];
                p->head = (p->head + 1) % Pipe::kSize;
                --p->count;
            }
            scheduler::wake(write_channel(p));      // freed space
            return static_cast<i64>(n);
        }
        if (p->writers == 0)
            return 0;                               // end of file
        scheduler::block_on(read_channel(p));
    }
}

i64 pipe_write(Pipe* p, const void* buffer, usize count)
{
    const auto* in = static_cast<const u8*>(buffer);
    usize written = 0;
    while (written < count) {
        if (p->readers == 0)
            return written > 0 ? static_cast<i64>(written) : -1;   // broken pipe
        if (p->count == Pipe::kSize) {
            scheduler::block_on(write_channel(p));
            continue;
        }
        while (written < count && p->count < Pipe::kSize) {
            p->buffer[p->tail] = in[written++];
            p->tail = (p->tail + 1) % Pipe::kSize;
            ++p->count;
        }
        scheduler::wake(read_channel(p));           // data available
    }
    return static_cast<i64>(written);
}

// Drop this descriptor's hold on its pipe end; free the pipe when both ends are
// fully closed, and wake the other side so it notices.
void release_pipe(Descriptor& d)
{
    auto* p = static_cast<Pipe*>(d.pipe);
    if (p == nullptr)
        return;
    if ((d.flags & kRead) != 0) {
        if (--p->readers == 0)
            scheduler::wake(write_channel(p));
    } else {
        if (--p->writers == 0)
            scheduler::wake(read_channel(p));
    }
    if (p->readers == 0 && p->writers == 0)
        kfree(p);
}

void append(char* out, usize& len, char c)
{
    if (len < kPathMax - 1)
        out[len++] = c;
}

// Resolve a path against the process's cwd and collapse "." and ".." so the
// filesystem only ever sees clean absolute paths. Relative paths are joined to
// the cwd first.
void resolve(const char* path, char* out)
{
    char joined[kPathMax * 2];
    usize j = 0;

    if (path[0] == '/') {
        for (usize i = 0; path[i] != '\0' && j < sizeof(joined) - 1; ++i)
            joined[j++] = path[i];
    } else {
        const char* cwd = table().cwd;
        for (usize i = 0; cwd[i] != '\0' && j < sizeof(joined) - 1; ++i)
            joined[j++] = cwd[i];
        if (j == 0 || joined[j - 1] != '/')
            joined[j++] = '/';
        for (usize i = 0; path[i] != '\0' && j < sizeof(joined) - 1; ++i)
            joined[j++] = path[i];
    }
    joined[j] = '\0';

    // Walk the segments, honouring "." (skip) and ".." (pop the last kept one).
    usize len = 0;
    out[0] = '/';
    len = 1;

    usize i = 0;
    while (joined[i] != '\0') {
        while (joined[i] == '/')
            ++i;
        char seg[kPathMax];
        usize s = 0;
        while (joined[i] != '/' && joined[i] != '\0') {
            if (s < kPathMax - 1)
                seg[s++] = joined[i];
            ++i;
        }
        seg[s] = '\0';
        if (s == 0 || (s == 1 && seg[0] == '.'))
            continue;
        if (s == 2 && seg[0] == '.' && seg[1] == '.') {
            while (len > 1 && out[len - 1] != '/')
                --len;
            if (len > 1)
                --len;            // drop the trailing slash too
            continue;
        }
        if (len > 1)
            append(out, len, '/');
        for (usize k = 0; k < s; ++k)
            append(out, len, seg[k]);
    }
    out[len] = '\0';
    if (out[0] == '\0') {
        out[0] = '/';
        out[1] = '\0';
    }
}

int alloc_fd()
{
    for (int fd = 0; fd < kMaxFds; ++fd) {
        if (table().fds[fd].kind == Kind::None)
            return fd;
    }
    return -1;
}

// A blocking, echoing read from the keyboard, one line's worth at most. Echoing
// here is what makes the shell usable: the user sees what they type, and
// backspace erases on screen.
char read_key()
{
    // Sleep the task (letting others run) until a key is buffered, then take it.
    // Interrupts are masked in the syscall, so the has_input check and the block
    // cannot race the keyboard IRQ that would wake us.
    // USB keyboards are not interrupt-driven here, so this path drives their
    // poll the same way the network waits drive the NIC's.
    while (!keyboard::has_input()) {
        usb::hid::poll();
        if (keyboard::has_input())
            break;
        scheduler::block_on(scheduler::kKeyboardChannel);
    }
    return keyboard::read();
}

i64 read_console(void* buffer, usize count)
{
    auto* out = static_cast<char*>(buffer);
    usize n = 0;
    while (n < count) {
        const char c = read_key();
        if (c == '\b' || c == 0x7F) {       // Backspace, or DEL from some terminals
            if (n > 0) {
                --n;
                console::write("\b \b");     // erase: back up, blank, back up
            }
            continue;
        }
        console::put(c);        // echo
        out[n++] = c;
        if (c == '\n')
            break;
    }
    return static_cast<i64>(n);
}

} // namespace

void init_table(Table& t)
{
    memset(&t, 0, sizeof(t));
    t.fds[0].kind = Kind::ConsoleIn;
    t.fds[1].kind = Kind::ConsoleOut;
    t.fds[2].kind = Kind::ConsoleOut;
    t.cwd[0] = '/';
    t.cwd[1] = '\0';
}

// Whether the calling process may touch a file with these owner and mode bits.
// The usual UNIX rule: root may do anything, otherwise the owner's bits apply to
// the owner, the group's to the group, and the other bits to everyone else -
// and only the first matching class is consulted, so a mode of 0007 really does
// lock the owner out.
bool may_access(const vfs::Stat& st, bool want_write)
{
    const u32 uid = scheduler::current_uid();
    if (uid == 0)
        return true;

    u16 read_bit;
    u16 write_bit;
    if (st.uid == uid) {
        read_bit  = vfs::kModeOwnerRead;
        write_bit = vfs::kModeOwnerWrite;
    } else if (st.gid == scheduler::current_gid()) {
        read_bit  = vfs::kModeGroupRead;
        write_bit = vfs::kModeGroupWrite;
    } else {
        read_bit  = vfs::kModeOtherRead;
        write_bit = vfs::kModeOtherWrite;
    }
    return (st.mode & (want_write ? write_bit : read_bit)) != 0;
}

i64 open(const char* path, u32 flags)
{
    char resolved[kPathMax];
    resolve(path, resolved);

    vfs::Stat st{};
    const bool exists = vfs::stat(resolved, st);

    if (!exists) {
        if ((flags & kCreate) == 0)
            return -1;
        if (!vfs::create(resolved, vfs::Type::File))
            return -1;
    } else if (st.type == vfs::Type::Directory && (flags & kWrite) != 0) {
        return -1;                          // cannot open a directory for writing
    } else if (!may_access(st, (flags & kWrite) != 0)) {
        return -1;                          // permission denied
    } else if ((flags & kTrunc) != 0 && st.type == vfs::Type::File) {
        vfs::write_entire_file(resolved, "", 0);
    }

    const int fd = alloc_fd();
    if (fd < 0)
        return -1;

    Descriptor& d = table().fds[fd];
    d.kind   = Kind::File;
    d.flags  = flags;
    d.offset = 0;
    if ((flags & kAppend) != 0 && vfs::stat(resolved, st))
        d.offset = st.size;
    usize i = 0;
    for (; resolved[i] != '\0' && i < kPathMax - 1; ++i)
        d.path[i] = resolved[i];
    d.path[i] = '\0';
    return fd;
}

i64 close(int fd)
{
    if (!valid_fd(fd) || table().fds[fd].kind == Kind::None)
        return -1;
    Descriptor& d = table().fds[fd];
    if (d.kind == Kind::Pipe)
        release_pipe(d);
    else if (d.kind == Kind::Socket)
        tcp::close(static_cast<int>(d.offset));
    d.kind = Kind::None;
    d.pipe = nullptr;
    return 0;
}

i64 read(int fd, void* buffer, usize count)
{
    if (!valid_fd(fd))
        return -1;
    Descriptor& d = table().fds[fd];

    switch (d.kind) {
    case Kind::ConsoleIn:
        return read_console(buffer, count);

    case Kind::Socket:
        return tcp::recv(static_cast<int>(d.offset), buffer, count);

    case Kind::Pipe:
        if ((d.flags & kRead) == 0)
            return -1;
        return pipe_read(static_cast<Pipe*>(d.pipe), buffer, count);
    case Kind::File: {
        const isize got = vfs::read(d.path, d.offset, buffer, count);
        if (got < 0)
            return -1;
        d.offset += static_cast<u64>(got);
        return got;
    }
    default:
        return -1;
    }
}

i64 write(int fd, const void* buffer, usize count)
{
    if (!valid_fd(fd))
        return -1;
    Descriptor& d = table().fds[fd];

    switch (d.kind) {
    case Kind::ConsoleOut: {
        const char* text = static_cast<const char*>(buffer);
        for (usize i = 0; i < count; ++i)
            console::put(text[i]);
        return static_cast<i64>(count);
    }
    case Kind::Socket:
        return tcp::send(static_cast<int>(d.offset), buffer, count);

    case Kind::Pipe:
        if ((d.flags & kWrite) == 0)
            return -1;
        return pipe_write(static_cast<Pipe*>(d.pipe), buffer, count);
    case Kind::File: {
        const isize wrote = vfs::write(d.path, d.offset, buffer, count);
        if (wrote < 0)
            return -1;
        d.offset += static_cast<u64>(wrote);
        return wrote;
    }
    default:
        return -1;
    }
}

i64 lseek(int fd, i64 offset, int whence)
{
    if (!valid_fd(fd) || table().fds[fd].kind != Kind::File)
        return -1;
    Descriptor& d = table().fds[fd];

    u64 base = 0;
    if (whence == 1) {
        base = d.offset;
    } else if (whence == 2) {
        vfs::Stat st{};
        if (!vfs::stat(d.path, st))
            return -1;
        base = st.size;
    }
    d.offset = base + static_cast<u64>(offset);
    return static_cast<i64>(d.offset);
}

i64 stat(const char* path, void* statbuf)
{
    char resolved[kPathMax];
    resolve(path, resolved);

    vfs::Stat st{};
    if (!vfs::stat(resolved, st))
        return -1;

    auto* out = static_cast<UserStat*>(statbuf);
    out->type = st.type == vfs::Type::Directory ? 1 : 0;
    out->mode = st.mode;
    out->size = st.size;
    out->uid  = st.uid;
    out->gid  = st.gid;
    return 0;
}

// Open a TCP connection and wrap it in a file descriptor, so read, write and
// close work on a socket exactly as they do on a file or a pipe.
i64 tcp_connect(u32 ip, u16 port)
{
    const int fd = alloc_fd();
    if (fd < 0)
        return -1;

    const int handle = tcp::connect(ip, port);
    if (handle < 0)
        return -1;

    Descriptor& d = table().fds[fd];
    d.kind   = Kind::Socket;
    d.offset = static_cast<u64>(handle);   // the connection, not a file offset
    d.flags  = kRead | kWrite;
    d.path[0] = '\0';
    return fd;
}

i64 chmod(const char* path, u16 mode)
{
    char resolved[kPathMax];
    resolve(path, resolved);

    vfs::Stat st{};
    if (!vfs::stat(resolved, st))
        return -1;
    // Only root or the file's owner may change its permissions.
    const u32 uid = scheduler::current_uid();
    if (uid != 0 && st.uid != uid)
        return -1;
    return vfs::chmod(resolved, mode) ? 0 : -1;
}

i64 chown(const char* path, u32 uid, u32 gid)
{
    char resolved[kPathMax];
    resolve(path, resolved);

    vfs::Stat st{};
    if (!vfs::stat(resolved, st))
        return -1;
    // Giving a file away is root's privilege: otherwise a user could dodge a
    // quota, or plant a file owned by someone else.
    if (scheduler::current_uid() != 0)
        return -1;
    return vfs::chown(resolved, uid, gid) ? 0 : -1;
}

i64 getdents(const char* path, void* buffer, usize max_entries)
{
    char resolved[kPathMax];
    resolve(path, resolved);

    // vfs::Entry is large (256-byte names); a page holds plenty for one dir.
    static vfs::Entry entries[64];
    const usize cap = max_entries < 64 ? max_entries : 64;

    usize count = 0;
    if (!vfs::list(resolved, entries, cap, count))
        return -1;

    auto* out = static_cast<Dirent*>(buffer);
    for (usize i = 0; i < count; ++i) {
        out[i].type = entries[i].type == vfs::Type::Directory ? 1 : 0;
        out[i].reserved = 0;
        out[i].size = entries[i].size;
        usize k = 0;
        for (; entries[i].name[k] != '\0' && k < kPathMax - 1; ++k)
            out[i].name[k] = entries[i].name[k];
        out[i].name[k] = '\0';
    }
    return static_cast<i64>(count);
}

i64 chdir(const char* path)
{
    char resolved[kPathMax];
    resolve(path, resolved);

    vfs::Stat st{};
    if (!vfs::stat(resolved, st) || st.type != vfs::Type::Directory)
        return -1;

    usize i = 0;
    for (; resolved[i] != '\0' && i < kPathMax - 1; ++i)
        table().cwd[i] = resolved[i];
    table().cwd[i] = '\0';
    return 0;
}

i64 getcwd(char* buffer, usize size)
{
    const char* cwd = table().cwd;
    usize i = 0;
    for (; cwd[i] != '\0' && i + 1 < size; ++i)
        buffer[i] = cwd[i];
    buffer[i] = '\0';
    return static_cast<i64>(i);
}

i64 mkdir(const char* path)
{
    char resolved[kPathMax];
    resolve(path, resolved);
    return vfs::create(resolved, vfs::Type::Directory) ? 0 : -1;
}

i64 unlink(const char* path)
{
    char resolved[kPathMax];
    resolve(path, resolved);
    return vfs::remove(resolved) ? 0 : -1;
}

i64 rename(const char* old_path, const char* new_path)
{
    char old_resolved[kPathMax];
    char new_resolved[kPathMax];
    resolve(old_path, old_resolved);
    resolve(new_path, new_resolved);
    return vfs::rename(old_resolved, new_resolved) ? 0 : -1;
}

i64 pipe(int* out_fds)
{
    const int read_fd = alloc_fd();
    if (read_fd < 0)
        return -1;
    table().fds[read_fd].kind = Kind::File;      // reserve the slot temporarily
    const int write_fd = alloc_fd();
    if (write_fd < 0) {
        table().fds[read_fd].kind = Kind::None;
        return -1;
    }

    auto* p = static_cast<Pipe*>(kmalloc(sizeof(Pipe)));
    if (p == nullptr) {
        table().fds[read_fd].kind = Kind::None;
        table().fds[write_fd].kind = Kind::None;
        return -1;
    }
    memset(p, 0, sizeof(Pipe));
    p->readers = 1;
    p->writers = 1;

    Descriptor& r = table().fds[read_fd];
    r.kind = Kind::Pipe;
    r.flags = kRead;
    r.pipe = p;
    Descriptor& w = table().fds[write_fd];
    w.kind = Kind::Pipe;
    w.flags = kWrite;
    w.pipe = p;

    out_fds[0] = read_fd;
    out_fds[1] = write_fd;
    return 0;
}

i64 dup2(int oldfd, int newfd)
{
    if (!valid_fd(oldfd) || !valid_fd(newfd) || table().fds[oldfd].kind == Kind::None)
        return -1;
    if (oldfd == newfd)
        return newfd;

    if (table().fds[newfd].kind != Kind::None)
        close(newfd);

    Descriptor& src = table().fds[oldfd];
    table().fds[newfd] = src;
    if (src.kind == Kind::Pipe) {
        auto* p = static_cast<Pipe*>(src.pipe);
        if ((src.flags & kRead) != 0)
            ++p->readers;
        else
            ++p->writers;
    }
    return newfd;
}

void inherit(Table& child)
{
    for (int fd = 0; fd < kMaxFds; ++fd) {
        Descriptor& d = child.fds[fd];
        if (d.kind != Kind::Pipe)
            continue;
        auto* p = static_cast<Pipe*>(d.pipe);
        if ((d.flags & kRead) != 0)
            ++p->readers;
        else
            ++p->writers;
    }
}

void close_all(Table& t)
{
    for (int fd = 0; fd < kMaxFds; ++fd) {
        if (t.fds[fd].kind == Kind::Pipe)
            release_pipe(t.fds[fd]);
        t.fds[fd].kind = Kind::None;
    }
}

} // namespace files
