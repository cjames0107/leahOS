#include <leah/console.hpp>
#include <leah/cpu.hpp>
#include <leah/file.hpp>
#include <leah/heap.hpp>
#include <leah/keyboard.hpp>
#include <leah/scheduler.hpp>
#include <leah/string.hpp>

namespace files {
namespace {

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
        // Woken with a signal waiting - being killed, most likely. Going back
        // to sleep here is how a process ends up impossible to kill.
        if (scheduler::signal_pending())
            return -1;
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
            if (scheduler::signal_pending())
                return written > 0 ? static_cast<i64>(written) : -1;
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
bool g_echo = true;

char read_key()
{
    // Sleep the task (letting others run) until a key is buffered, then take it.
    // Interrupts are masked in the syscall, so the has_input check and the block
    // cannot race the keyboard IRQ that would wake us.
    // Both keyboards feed the same buffer and the same wake channel: the PS/2
    // one from its IRQ, the USB one from the timer tick, which is what polls it.
    while (!keyboard::has_input())
        scheduler::block_on(scheduler::kKeyboardChannel);
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
                if (g_echo)
                    console::write("\b \b");   // erase: back up, blank, back up
            }
            continue;
        }
        if (g_echo)
            console::put(c);        // echo
        out[n++] = c;
        if (c == '\n')
            break;
    }
    return static_cast<i64>(n);
}

} // namespace

// Echo is suppressed while a password is being typed. It lives here rather
// than in the console because it is a property of reading the terminal, not of
// writing to it.
void set_console_echo(bool on) { g_echo = on; }

void init_table(Table& t)
{
    memset(&t, 0, sizeof(t));
    t.fds[0].kind = Kind::ConsoleIn;
    t.fds[1].kind = Kind::ConsoleOut;
    t.fds[2].kind = Kind::ConsoleOut;
}

i64 close(int fd)
{
    if (!valid_fd(fd) || table().fds[fd].kind == Kind::None)
        return -1;
    Descriptor& d = table().fds[fd];
    if (d.kind == Kind::Pipe)
        release_pipe(d);
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


    case Kind::Pipe:
        if ((d.flags & kRead) == 0)
            return -1;
        return pipe_read(static_cast<Pipe*>(d.pipe), buffer, count);
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

    case Kind::Pipe:
        if ((d.flags & kWrite) == 0)
            return -1;
        return pipe_write(static_cast<Pipe*>(d.pipe), buffer, count);
    default:
        return -1;
    }
}

i64 pipe(int* out_fds)
{
    const int read_fd = alloc_fd();
    if (read_fd < 0)
        return -1;
    /* Reserve the slot so the second allocation cannot pick the same one.
     * Pipe rather than a placeholder kind, because Pipe is the only thing this
     * slot is ever going to be and there are no other kinds left to borrow. */
    table().fds[read_fd].kind = Kind::Pipe;
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
