#pragma once

#include <leah/types.hpp>

// Per-process open files and working directory, and the operations the file
// syscalls are built from. The VFS underneath is path-based, so an open file
// descriptor is just a remembered path plus a cursor - no VFS handle to leak.

namespace files {

/* Sixteen was enough while a process had three descriptors and a file. A
 * shell running a long pipeline, or a server holding a client each, wants
 * more, and the table is per-task so the cost is paid by every task. */
constexpr int   kMaxFds   = 32;
constexpr usize kPathMax  = 128;

// A read or a write that a signal cut short, as a negated errno - the same
// convention libc already uses for what comes back from vfsd, and for the same
// reason: one register has to carry both an answer and a reason.
//
// It matters that this is not a plain -1. A shell blocked reading its terminal
// cannot tell "failed" from "the far end closed", so it treats both as the end
// of its input - and closes. That is what pressing Ctrl-C at a prompt used to
// do. The number is Linux's EINTR, as everything else here is.
constexpr i64 kInterrupted = -4;

enum class Kind : u8 {
    None,
    ConsoleIn,      // fd 0: the keyboard
    ConsoleOut,     // fd 1, 2: the console
    Pipe,
};

struct Descriptor {
    Kind  kind;
    u32   flags;
    void* pipe;     // the shared pipe object, for Kind::Pipe
};

// A process's file table and its current directory, carried in its task.
struct Table {
    Descriptor fds[kMaxFds];
};

// open() flags. Small subset, values chosen to be our own.
constexpr u32 kRead   = 1 << 0;
constexpr u32 kWrite  = 1 << 1;
constexpr u32 kCreate = 1 << 2;
constexpr u32 kTrunc  = 1 << 3;
constexpr u32 kAppend = 1 << 4;

// Set up a brand-new table: stdin/stdout/stderr wired to the console, cwd "/".
void init_table(Table& table);

// The dirent a getdents call fills for user space. Kept flat and fixed-size so
// it copies straight out to a user array.
struct [[gnu::packed]] Dirent {
    u32  type;      // 0 = file, 1 = directory
    u32  reserved;
    u64  size;
    char name[kPathMax];
};

// These operate on the calling process's table (fetched from the scheduler) and
// on user pointers in the active address space. Return values follow the usual
// convention: a non-negative result, or -1 on error.
i64 close(int fd);
i64 read(int fd, void* buffer, usize count);

// --- poll --------------------------------------------------------------------
//
// Which of these can be done right now without waiting. Only the kernel's own
// descriptors are asked about - a file on a disk is always ready, and libc
// answers for those without a syscall.
constexpr u32 kPollIn   = 1u << 0;      // a read would not block
constexpr u32 kPollOut  = 1u << 2;      // a write would not block
constexpr u32 kPollErr  = 1u << 3;
constexpr u32 kPollHup  = 1u << 4;      // the other end has gone
constexpr u32 kPollBad  = 1u << 5;      // not an open descriptor

// What `fd` could do this instant, as a mask of the above.
u32 readiness(int fd);

// Open one end of the FIFO named by `key`, which is the inode number of the
// file that names it. Blocks until the other end is opened, unless
// `nonblocking` - an open for reading with no writer is not an empty pipe, it
// is a conversation that has not started.
i64 open_fifo(u64 key, bool for_writing, bool nonblocking);
i64 write(int fd, const void* buffer, usize count);
void set_console_echo(bool on);

// Create a pipe. Writes the read fd to out_fds[0] and the write fd to
// out_fds[1]. Returns 0, or -1.
i64 pipe(int* out_fds);

// Point newfd at whatever oldfd refers to, closing newfd first if it is open.

// Bump the pipe reference counts for a table just copied by fork - both the
// parent and child now hold each inherited pipe end.
void inherit(Table& child);

// Close every descriptor in a table (called when a task exits), so pipe ends
// are released and readers see EOF.
void close_all(Table& table);

} // namespace files
