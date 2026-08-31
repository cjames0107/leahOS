#include <leah/console.hpp>
#include <leah/cpu.hpp>
#include <leah/file.hpp>
#include <leah/lock.hpp>
#include <leah/heap.hpp>
#include <leah/keyboard.hpp>
#include <leah/pty.hpp>
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
    /* How many calls are inside this pipe right now.
     *
     * The descriptor table cannot be locked across a read that blocks - the
     * task would sleep holding a spin lock, which the scheduler asserts
     * against. So the table lock is held only long enough to turn a descriptor
     * number into an object, and this is what stops the object being freed out
     * from under the call that is using it. A close while somebody is inside
     * marks it and the last one out does the freeing. */
    int   busy;
    bool  doomed;
};

/* The descriptor table, and nothing else.
 *
 * Deliberately the narrowest lock in this file. Everything it protects is a
 * lookup - a number to an object - and every one of those is short and cannot
 * block. What can block is what happens afterwards, and that is done with this
 * dropped and a reference held instead.
 *
 * An earlier attempt at this deadlocked, and the cause was not here: the big
 * kernel lock had no place in the order, so a path that took it while holding
 * this one was a hang rather than a complaint. It now says so. */
sync::RankedLock g_lock(sync::Rank::Files, "files");

/* What to wake once the lock is off - the same discipline the terminal keeps,
 * and for the same reason. scheduler::wake takes the big kernel lock, and
 * taking that while holding a finer one is the deadlock the ordering exists to
 * prevent. It has been safe only because every syscall already holds the big
 * lock on entry, which makes the acquire recursive; that is an accident, not a
 * guarantee. */
struct Wakes {
    u64 channel[3];
    u32 count;

    void add(u64 c)
    {
        for (u32 i = 0; i < count; ++i)
            if (channel[i] == c)
                return;
        if (count < 3)
            channel[count++] = c;
    }
};

void flush(const Wakes& w)
{
    for (u32 i = 0; i < w.count; ++i)
        scheduler::wake(w.channel[i]);
}
u64 read_channel(Pipe* p)  { return reinterpret_cast<u64>(p); }
u64 write_channel(Pipe* p) { return reinterpret_cast<u64>(p) + 1; }

/* --- named pipes -------------------------------------------------------------
 *
 * A FIFO is a pipe two unrelated processes can find. An ordinary pipe is found
 * by inheritance - fork hands both ends down - which is no use to two programs
 * started separately from a shell, and that is the whole reason FIFOs exist.
 *
 * They find each other by the inode number of the file on disk, which is the
 * one name for it that is already unique and already agreed. The file itself
 * holds nothing; it exists so the FIFO has a name, permissions and an owner,
 * which is exactly what the filesystem is for. Everything that moves goes
 * through the pipe here, and never touches the disk.
 *
 * The table is small on purpose: a FIFO with nobody at either end is not a
 * FIFO, so entries live only as long as somebody has one open.
 */
constexpr usize kMaxNamedPipes = 16;

struct NamedPipe {
    bool  used;
    u64   key;          // the inode number of the file that names it
    Pipe* pipe;
    /* Whether each end has *ever* been opened, not whether one is open now.
     *
     * An open for reading waits for a writer to arrive - and once one has,
     * the wait is over for good, even if that writer has since finished and
     * gone. Waiting on the live count instead means a writer that opens,
     * writes and closes while the reader is between checks is never seen at
     * all: the count goes up and back down, and the reader waits forever for
     * something that already happened. The data is sitting in the buffer the
     * whole time.
     */
    bool  seen_reader;
    bool  seen_writer;
};

NamedPipe g_named[kMaxNamedPipes];

NamedPipe* find_named(u64 key)
{
    for (usize i = 0; i < kMaxNamedPipes; ++i)
        if (g_named[i].used && g_named[i].key == key)
            return &g_named[i];
    return nullptr;
}

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
            scheduler::wake(scheduler::kPollChannel);
            return static_cast<i64>(n);
        }
        if (p->writers == 0)
            return 0;                               // end of file
        scheduler::block_on(read_channel(p));
        // Woken with a signal waiting - being killed, most likely. Going back
        // to sleep here is how a process ends up impossible to kill.
        //
        // Interrupted, though, and said so: this used to be a plain -1, which
        // libc turned into "read failed" and a shell reading its own terminal
        // took for end of input. Pressing Ctrl-C at a prompt closed the
        // terminal, because the shell's read came back looking exactly like
        // the pipe having been closed.
        if (scheduler::signal_pending())
            return kInterrupted;
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
            // What did get through still counts; only a write that managed
            // nothing at all reports the interruption.
            if (scheduler::signal_pending())
                return written > 0 ? static_cast<i64>(written) : kInterrupted;
            continue;
        }
        while (written < count && p->count < Pipe::kSize) {
            p->buffer[p->tail] = in[written++];
            p->tail = (p->tail + 1) % Pipe::kSize;
            ++p->count;
        }
        scheduler::wake(read_channel(p));           // data available
        scheduler::wake(scheduler::kPollChannel);
    }
    return static_cast<i64>(written);
}

// Drop this descriptor's hold on its pipe end; free the pipe when both ends are
// fully closed, and wake the other side so it notices.
void release_pipe(Descriptor& d, Wakes& wake)
{
    auto* p = static_cast<Pipe*>(d.pipe);
    if (p == nullptr)
        return;
    if ((d.flags & kRead) != 0) {
        if (--p->readers == 0)
            wake.add(write_channel(p));
    } else {
        if (--p->writers == 0)
            wake.add(read_channel(p));
    }
    /* An end closing changes what the other end can do without waiting - a
     * reader with no writers left is readable, at end of file. A poller that
     * was not told would sit through the one event it was waiting for. */
    wake.add(scheduler::kPollChannel);
    if (p->readers == 0 && p->writers == 0) {
        /* And its name, if it had one. A FIFO with nobody at either end is
         * not a FIFO; the next open makes a fresh one, which is right - what
         * was in it belonged to the conversation that just ended. */
        for (usize i = 0; i < kMaxNamedPipes; ++i)
            if (g_named[i].used && g_named[i].pipe == p) {
                g_named[i].used = false;
                g_named[i].pipe = nullptr;
            }
        /* Not while a call is inside it. The last one out frees it instead -
         * see drop_pipe. */
        if (p->busy == 0)
            kfree(p);
        else
            p->doomed = true;
    }
}

/* Take and give back a reference for the duration of one call.
 *
 * Both are called with the table lock held, which is what makes "still open"
 * and "reference taken" one step rather than two. */
void hold_pipe(Pipe* p) { if (p != nullptr) ++p->busy; }

void drop_pipe(Pipe* p)
{
    if (p == nullptr)
        return;
    if (--p->busy == 0 && p->doomed)
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

u32 readiness(int fd)
{
    if (fd < 0 || fd >= kMaxFds)
        return kPollBad;
    const Descriptor& d = table().fds[fd];
    switch (d.kind) {
    case Kind::None:
        return kPollBad;
    case Kind::ConsoleIn:
        return keyboard::has_input() ? kPollIn : 0;
    case Kind::ConsoleOut:
        return kPollOut;            // the console never makes anyone wait
    case Kind::PtyMaster:
    case Kind::PtySlave:
        /* Always writable: a terminal's output buffer drops rather than
         * blocks, so nothing ever waits to write to one. */
        return kPollOut |
               (pty::readable(d.pipe, d.kind == Kind::PtyMaster) ? kPollIn : 0);
    case Kind::Pipe: {
        const Pipe* p = static_cast<const Pipe*>(d.pipe);
        if (p == nullptr)
            return kPollErr;
        u32 ready = 0;
        /* Data to take, or nobody left to send any - both mean a read returns
         * immediately, which is the only thing kPollIn claims. A reader that
         * asks and then gets zero bytes has been told the truth. */
        if (p->count > 0)
            ready |= kPollIn;
        if (p->writers == 0)
            ready |= kPollIn | kPollHup;
        if (p->count < Pipe::kSize)
            ready |= kPollOut;
        if (p->readers == 0)
            ready |= kPollErr;      // writing would raise SIGPIPE
        return ready;
    }
    }
    return kPollBad;
}


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
    Wakes wake{};
    void* pty_end = nullptr;
    bool  pty_master = false;
    {
        sync::Guard guard(g_lock);
        if (!valid_fd(fd) || table().fds[fd].kind == Kind::None)
            return -1;
        Descriptor& d = table().fds[fd];
        if (d.kind == Kind::Pipe) {
            release_pipe(d, wake);
        } else if (d.kind == Kind::PtyMaster || d.kind == Kind::PtySlave) {
            /* Closed after this lock is off: the terminal takes its own, and
             * it wakes people, which reaches the scheduler. */
            pty_end = d.pipe;
            pty_master = d.kind == Kind::PtyMaster;
        }
        d.kind = Kind::None;
        d.pipe = nullptr;
    }
    if (pty_end != nullptr)
        pty::close(pty_end, pty_master);
    flush(wake);
    return 0;
}

bool pty_of(int fd, void** out_object, bool* out_master)
{
    sync::Guard guard(g_lock);
    if (!valid_fd(fd))
        return false;
    const Descriptor& d = table().fds[fd];
    if (d.kind != Kind::PtyMaster && d.kind != Kind::PtySlave)
        return false;
    if (out_object != nullptr) *out_object = d.pipe;
    if (out_master != nullptr) *out_master = (d.kind == Kind::PtyMaster);
    return true;
}

i64 adopt_pty(void* object, bool master)
{
    sync::Guard guard(g_lock);
    const int fd = alloc_fd();
    if (fd < 0)
        return -1;
    Descriptor& d = table().fds[fd];
    d.kind  = master ? Kind::PtyMaster : Kind::PtySlave;
    d.flags = kRead | kWrite;
    d.pipe  = object;
    return fd;
}

/* Resolve, then let go, then do the work.
 *
 * The table lock cannot be held across the call below: every one of these can
 * block, and a task that sleeps holding a spin lock leaves other processors
 * waiting on something that is not running - which the scheduler now asserts
 * against outright. So the lock covers only the lookup, and a reference taken
 * in the same breath is what keeps the object alive afterwards.
 *
 * That reference is the whole design. Without it, another thread closing this
 * descriptor while the read is asleep would free the pipe under it. The pty
 * needs no separate one: its ends are already counted, and it is those counts
 * that decide when it goes.
 */
i64 read(int fd, void* buffer, usize count)
{
    Kind  kind   = Kind::None;
    void* object = nullptr;
    {
        sync::Guard guard(g_lock);
        if (!valid_fd(fd))
            return -1;
        Descriptor& d = table().fds[fd];
        kind = d.kind;
        object = d.pipe;
        if (kind == Kind::Pipe) {
            if ((d.flags & kRead) == 0)
                return -1;
            hold_pipe(static_cast<Pipe*>(object));
        }
    }

    i64 result;
    switch (kind) {
    case Kind::ConsoleIn:
        result = read_console(buffer, count);
        break;
    case Kind::Pipe:
        result = pipe_read(static_cast<Pipe*>(object), buffer, count);
        break;
    case Kind::PtyMaster:
    case Kind::PtySlave:
        result = pty::read(object, kind == Kind::PtyMaster, buffer, count);
        break;
    default:
        result = -1;
        break;
    }

    if (kind == Kind::Pipe) {
        sync::Guard guard(g_lock);
        drop_pipe(static_cast<Pipe*>(object));
    }
    return result;
}

i64 write(int fd, const void* buffer, usize count)
{
    Kind  kind   = Kind::None;
    void* object = nullptr;
    {
        sync::Guard guard(g_lock);
        if (!valid_fd(fd))
            return -1;
        Descriptor& d = table().fds[fd];
        kind = d.kind;
        object = d.pipe;
        if (kind == Kind::Pipe) {
            if ((d.flags & kWrite) == 0)
                return -1;
            hold_pipe(static_cast<Pipe*>(object));
        }
    }

    i64 result;
    switch (kind) {
    case Kind::ConsoleOut: {
        const char* text = static_cast<const char*>(buffer);
        for (usize i = 0; i < count; ++i)
            console::put(text[i]);
        result = static_cast<i64>(count);
        break;
    }

    case Kind::Pipe:
        result = pipe_write(static_cast<Pipe*>(object), buffer, count);
        break;

    case Kind::PtyMaster:
    case Kind::PtySlave:
        result = pty::write(object, kind == Kind::PtyMaster, buffer, count);
        break;

    default:
        result = -1;
        break;
    }

    if (kind == Kind::Pipe) {
        sync::Guard guard(g_lock);
        drop_pipe(static_cast<Pipe*>(object));
    }
    return result;
}

/* Open one end of the FIFO named by `key`.
 *
 * The blocking is the part that surprises people, and it is deliberate: an
 * open for reading waits for a writer and an open for writing waits for a
 * reader. Without it `echo hi > fifo` would finish before anything was there
 * to receive it, and the data would go into a pipe with no reader - which is
 * to say, nowhere. Waiting is what makes the two ends a rendezvous.
 */
i64 open_fifo(u64 key, bool for_writing, bool nonblocking)
{
    const int fd = alloc_fd();
    if (fd < 0)
        return -1;

    NamedPipe* named = find_named(key);
    if (named == nullptr) {
        for (usize i = 0; i < kMaxNamedPipes && named == nullptr; ++i)
            if (!g_named[i].used)
                named = &g_named[i];
        if (named == nullptr)
            return -1;                  // too many FIFOs open at once
        auto* p = static_cast<Pipe*>(kmalloc(sizeof(Pipe)));
        if (p == nullptr)
            return -1;
        memset(p, 0, sizeof(Pipe));
        named->used = true;
        named->key = key;
        named->pipe = p;
    }

    Pipe* p = named->pipe;
    Descriptor& d = table().fds[fd];
    d.kind = Kind::Pipe;
    d.flags = for_writing ? kWrite : kRead;
    d.pipe = p;
    if (for_writing) {
        ++p->writers;
        named->seen_writer = true;
    } else {
        ++p->readers;
        named->seen_reader = true;
    }

    /* Now that this end is counted, wake anybody waiting for it and wait for
     * the other - in that order, or two processes arriving together each miss
     * the other's arrival and both wait forever. */
    scheduler::wake(read_channel(p));
    scheduler::wake(write_channel(p));
    scheduler::wake(scheduler::kPollChannel);

    if (!nonblocking) {
        /* Waited for in short naps rather than indefinitely.
         *
         * The other end may arrive between the check above and the block
         * below - two processes opening a FIFO at the same moment is the
         * normal case, not a rare one - and a wake delivered in that window
         * goes to a task that is not yet asleep. Blocking outright on a
         * condition that has already been signalled is a machine that stops,
         * and it stopped intermittently in exactly this way.
         *
         * The channel still does the work: an arrival wakes this immediately.
         * The deadline only bounds what a missed one costs, which is one tick
         * rather than the rest of the session. */
        while (for_writing ? !named->seen_reader : !named->seen_writer) {
            scheduler::block_on_until(for_writing ? write_channel(p)
                                                  : read_channel(p), 1);
            if (scheduler::signal_pending()) {
                { Wakes w{}; release_pipe(table().fds[fd], w); flush(w); }
                table().fds[fd].kind = Kind::None;
                return kInterrupted;
            }
        }
    }
    return fd;
}

i64 pipe(int* out_fds)
{
    sync::Guard guard(g_lock);
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
        if (d.kind == Kind::PtyMaster || d.kind == Kind::PtySlave) {
            /* Both ends are counted, for the same reason a pipe's are: a fork
             * leaves two processes holding this end, and the far end must not
             * see it close until both have let go. */
            pty::reopen(d.pipe, d.kind == Kind::PtyMaster);
            continue;
        }
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
            { Wakes w{}; release_pipe(t.fds[fd], w); flush(w); }
        else if (t.fds[fd].kind == Kind::PtyMaster ||
                 t.fds[fd].kind == Kind::PtySlave)
            pty::close(t.fds[fd].pipe, t.fds[fd].kind == Kind::PtyMaster);
        t.fds[fd].kind = Kind::None;
    }
}

} // namespace files
