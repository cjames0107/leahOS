#include <leah/console.hpp>
#include <leah/cpu.hpp>
#include <leah/file.hpp>
#include <leah/keyboard.hpp>
#include <leah/scheduler.hpp>
#include <leah/string.hpp>
#include <leah/vfs.hpp>

namespace files {
namespace {

// The layout stat() writes to the user statbuf; mirrored by struct stat in the
// libc's sys/stat.h.
struct [[gnu::packed]] UserStat {
    u32 type;       // 0 = file, 1 = directory
    u32 reserved;
    u64 size;
};

Table& table() { return scheduler::current_files(); }

bool valid_fd(int fd) { return fd >= 0 && fd < kMaxFds; }

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
i64 read_console(void* buffer, usize count)
{
    auto* out = static_cast<char*>(buffer);
    usize n = 0;
    while (n < count) {
        const char c = keyboard::read_blocking();
        if (c == '\b') {
            if (n > 0) {
                --n;
                console::write("\b \b");
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
    table().fds[fd].kind = Kind::None;
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
    out->reserved = 0;
    out->size = st.size;
    return 0;
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

} // namespace files
