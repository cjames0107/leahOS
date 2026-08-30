/* See <leah/pty.hpp>. */

#include <leah/file.hpp>
#include <leah/pty.hpp>
#include <leah/scheduler.hpp>
#include <leah/string.hpp>

namespace pty {
namespace {

/* The local modes, by the names <termios.h> gives them. Kept here rather than
 * included from userland: the two have to agree, and the header says so. */
constexpr u32 kIsig   = 0x0001;
constexpr u32 kIcanon = 0x0002;
constexpr u32 kEcho   = 0x0008;

/* The control characters, at the indices <termios.h> uses. */
constexpr int kVIntr  = 0;
constexpr int kVQuit  = 1;
constexpr int kVErase = 2;
constexpr int kVSusp  = 3;
constexpr int kNCC    = 8;

constexpr int kSigInt  = 2;
constexpr int kSigQuit = 3;
constexpr int kSigTstp = 20;

struct Ring {
    static constexpr usize kSize = 4096;
    u8    data[kSize];
    usize head, tail, count;

    void put(u8 c)
    {
        if (count == kSize)
            return;                     /* full: the byte is dropped, as a
                                         * terminal's input buffer does */
        data[tail] = c;
        tail = (tail + 1) % kSize;
        ++count;
    }
    u8 get()
    {
        const u8 c = data[head];
        head = (head + 1) % kSize;
        --count;
        return c;
    }
};

struct Pty {
    bool  used;
    Ring  to_slave;                     /* typed, and ready to be read */
    Ring  to_master;                    /* printed, and echoed */

    /* The line being assembled in canonical mode. Not readable until the
     * newline, which is what makes backspace work: nothing has been handed
     * over yet, so there is still something to take back. */
    static constexpr usize kLine = 1024;
    u8    line[kLine];
    usize line_len;

    u32   lflag;
    u8    cc[kNCC];
    i32   fg_pgid;
    u32   size;                         /* rows << 16 | columns */
    int   masters;
    int   slaves;
};

Pty g_ptys[kMaxPtys];

/* One channel per direction per pty, as pipes have. The addresses are distinct
 * because they are inside distinct objects. */
u64 slave_channel(Pty* p)  { return reinterpret_cast<u64>(&p->to_slave); }
u64 master_channel(Pty* p) { return reinterpret_cast<u64>(&p->to_master); }

void echo(Pty* p, u8 c)
{
    if ((p->lflag & kEcho) == 0)
        return;
    p->to_master.put(c);
}

void echo_text(Pty* p, const char* s)
{
    while (*s != '\0')
        echo(p, static_cast<u8>(*s++));
}

/* A finished line becomes readable, all at once. */
void commit_line(Pty* p)
{
    for (usize i = 0; i < p->line_len; ++i)
        p->to_slave.put(p->line[i]);
    p->line_len = 0;
}

/* One key, through the line discipline. This is the whole of what a terminal
 * program used to do for itself. */
void key(Pty* p, u8 c)
{
    if ((p->lflag & kIsig) != 0) {
        int signo = 0;
        if (c == p->cc[kVIntr])      signo = kSigInt;
        else if (c == p->cc[kVQuit]) signo = kSigQuit;
        else if (c == p->cc[kVSusp]) signo = kSigTstp;
        if (signo != 0) {
            /* What was half typed is abandoned: the line the interrupt
             * cancelled must not turn up as the next command. */
            p->line_len = 0;
            echo_text(p, "^C\r\n");
            if (p->fg_pgid > 0)
                scheduler::signal_send_group(static_cast<u32>(p->fg_pgid), signo);
            scheduler::wake(slave_channel(p));
            scheduler::wake(master_channel(p));
            return;
        }
    }

    if ((p->lflag & kIcanon) == 0) {
        /* Raw: every key is readable the moment it arrives, which is what an
         * editor or a pager asks for when it turns canonical mode off. */
        p->to_slave.put(c);
        echo(p, c);
        return;
    }

    if (c == p->cc[kVErase] || c == 0x7F) {
        if (p->line_len > 0) {
            --p->line_len;
            /* Back over it, blank it, back again - which is how a character is
             * removed from a screen that can only be written forwards. */
            echo_text(p, "\b \b");
        }
        return;
    }
    if (c == '\r' || c == '\n') {
        if (p->line_len < Pty::kLine)
            p->line[p->line_len++] = '\n';
        echo_text(p, "\r\n");
        commit_line(p);
        return;
    }
    if (p->line_len < Pty::kLine) {
        p->line[p->line_len++] = c;
        echo(p, c);
    }
}

} // namespace

i64 open_pair(int* out_index)
{
    for (int i = 0; i < kMaxPtys; ++i) {
        if (g_ptys[i].used)
            continue;
        Pty& p = g_ptys[i];
        memset(&p, 0, sizeof(p));
        p.used = true;
        /* What a terminal is before anybody changes it: lines, echoed, with
         * the interrupt keys doing what they say. */
        p.lflag = kIsig | kIcanon | kEcho;
        p.cc[kVIntr]  = 0x03;           /* Ctrl-C */
        p.cc[kVQuit]  = 0x1C;           /* Ctrl-\ */
        p.cc[kVErase] = 0x08;           /* Backspace */
        p.cc[kVSusp]  = 0x1A;           /* Ctrl-Z */
        p.size = (24u << 16) | 80u;
        p.masters = 1;

        const i64 fd = files::adopt_pty(&p, true);
        if (fd < 0) {
            p.used = false;
            return -1;
        }
        if (out_index != nullptr)
            *out_index = i;
        return fd;
    }
    return -1;
}

i64 open_slave(int index)
{
    if (index < 0 || index >= kMaxPtys || !g_ptys[index].used)
        return -1;
    Pty& p = g_ptys[index];
    const i64 fd = files::adopt_pty(&p, false);
    if (fd < 0)
        return -1;
    ++p.slaves;
    return fd;
}

bool exists(int index)
{
    return index >= 0 && index < kMaxPtys && g_ptys[index].used;
}

i64 control(void* object, int op, u64 argument)
{
    auto* p = static_cast<Pty*>(object);
    if (p == nullptr || !p->used)
        return -1;
    switch (op) {
    case kGetPgrp:  return p->fg_pgid;
    case kSetPgrp:  p->fg_pgid = static_cast<i32>(argument); return 0;
    case kGetFlags: return p->lflag;
    case kSetFlags:
        p->lflag = static_cast<u32>(argument);
        /* Leaving canonical mode hands over whatever was half typed rather
         * than stranding it: a program that switches to raw mode wants the
         * keys, not a line that will never be finished. */
        if ((p->lflag & kIcanon) == 0 && p->line_len > 0) {
            commit_line(p);
            scheduler::wake(slave_channel(p));
        }
        return 0;
    case kGetSize:  return p->size;
    case kSetSize:  p->size = static_cast<u32>(argument); return 0;
    default:        return -1;
    }
}

i64 read(void* object, bool master, void* buffer, usize count)
{
    auto* p = static_cast<Pty*>(object);
    auto* out = static_cast<u8*>(buffer);
    if (p == nullptr || !p->used || count == 0)
        return -1;

    Ring& ring = master ? p->to_master : p->to_slave;
    const u64 channel = master ? master_channel(p) : slave_channel(p);

    for (;;) {
        if (ring.count > 0) {
            usize n = 0;
            while (n < count && ring.count > 0) {
                const u8 c = ring.get();
                out[n++] = c;
                /* A canonical read stops at the end of a line, so that a shell
                 * asking for a line gets one and not whatever else has been
                 * typed ahead of it. */
                if (!master && (p->lflag & kIcanon) != 0 && c == '\n')
                    break;
            }
            scheduler::wake(scheduler::kPollChannel);
            return static_cast<i64>(n);
        }
        /* The other end has gone: end of file, which is what closes a shell
         * whose terminal window was shut. */
        if (master ? (p->slaves == 0) : (p->masters == 0))
            return 0;
        scheduler::block_on(channel);
        if (scheduler::signal_pending())
            return files::kInterrupted;
    }
}

i64 write(void* object, bool master, const void* buffer, usize count)
{
    auto* p = static_cast<Pty*>(object);
    const auto* in = static_cast<const u8*>(buffer);
    if (p == nullptr || !p->used)
        return -1;

    if (master) {
        /* Keys. Through the line discipline, one at a time, because what each
         * one does depends on the ones before it. */
        for (usize i = 0; i < count; ++i)
            key(p, in[i]);
        scheduler::wake(slave_channel(p));
        scheduler::wake(master_channel(p));     /* the echo */
        scheduler::wake(scheduler::kPollChannel);
        return static_cast<i64>(count);
    }

    /* Output. Nothing is done to it - a terminal shows what a program printed.
     * A write with nobody at the master end is thrown away rather than
     * blocking: the alternative is a program wedged forever because its window
     * was closed. */
    if (p->masters == 0)
        return static_cast<i64>(count);
    for (usize i = 0; i < count; ++i)
        p->to_master.put(in[i]);
    scheduler::wake(master_channel(p));
    scheduler::wake(scheduler::kPollChannel);
    return static_cast<i64>(count);
}

void reopen(void* object, bool master)
{
    auto* p = static_cast<Pty*>(object);
    if (p == nullptr || !p->used)
        return;
    if (master) ++p->masters;
    else        ++p->slaves;
}

void close(void* object, bool master)
{
    auto* p = static_cast<Pty*>(object);
    if (p == nullptr || !p->used)
        return;
    if (master) {
        if (--p->masters <= 0) {
            p->masters = 0;
            /* Everything on the far end is reading a terminal that has gone.
             * Waking them is how they find out. */
            scheduler::wake(slave_channel(p));
        }
    } else {
        if (--p->slaves <= 0) {
            p->slaves = 0;
            scheduler::wake(master_channel(p));
        }
    }
    if (p->masters == 0 && p->slaves == 0)
        p->used = false;
    scheduler::wake(scheduler::kPollChannel);
}

bool readable(void* object, bool master)
{
    auto* p = static_cast<Pty*>(object);
    if (p == nullptr || !p->used)
        return true;                    /* gone counts as ready: it reads 0 */
    const Ring& ring = master ? p->to_master : p->to_slave;
    if (ring.count > 0)
        return true;
    return master ? (p->slaves == 0) : (p->masters == 0);
}

} // namespace pty
