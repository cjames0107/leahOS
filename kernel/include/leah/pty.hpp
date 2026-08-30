#pragma once

#include <leah/types.hpp>

/* Pseudo-terminals: the tty driver this system did not have.
 *
 * A terminal here is an ordinary program - a window with a font in it - and
 * the thing it runs is at the far end of something. Until now that something
 * was a pipe, and the terminal did the line discipline itself: assembling
 * lines, echoing keys, applying backspace, turning Ctrl-C into a signal, and
 * keeping the settings and the foreground process group in a page of shared
 * memory because there was nowhere else to put them.
 *
 * That works exactly as long as the only program on the far end is one that
 * knows about the arrangement. `isatty` had to be told; job control had to be
 * approximated; anything expecting a terminal had to be taught. A pty makes
 * the far end an ordinary character device, so a program does not have to know
 * anything at all - which is the point, and the thing that lets a future sshd
 * or `script` run a shell without either of them being special.
 *
 * It lives in the kernel rather than in a server, and that is not a retreat
 * from putting drivers in ring 3. A tty is not a driver: it is an IPC object
 * with process-group semantics, and the four things it needs - a buffer, a
 * blocked reader, a process group and a signal - are all already here. Pipes
 * are in the kernel for the same reason.
 *
 * Two directions, and they are not symmetrical:
 *
 *   master -> slave   what was typed. The line discipline is on this path: in
 *                     canonical mode a line is assembled and only becomes
 *                     readable at the newline, echoing goes back the other way
 *                     as it is typed, and the interrupt keys become signals to
 *                     the foreground group.
 *   slave -> master   what the program printed. Nothing is done to it.
 */

namespace pty {

constexpr int kMaxPtys = 8;

// Open a fresh pair. Returns the master's descriptor, and writes which
// /dev/pts entry the slave is at. -1 when there are none left.
i64 open_pair(int* out_index);

// The slave end of a pair that already exists. -1 when it does not.
i64 open_slave(int index);

// Whether an index names a pty that is open, which is what a /dev/pts listing
// and a stat of one need to know.
bool exists(int index);

// What tty_control asks. `op` is one of the kControl values below.
constexpr int kGetPgrp    = 0;
constexpr int kSetPgrp    = 1;
constexpr int kGetFlags   = 2;   // the local modes: ISIG, ICANON, ECHO
constexpr int kSetFlags   = 3;
constexpr int kGetSize    = 4;   // rows in the high 16 bits, columns in the low
constexpr int kSetSize    = 5;
i64 control(void* object, int op, u64 argument);

// The read, write and close behind the file syscalls. `object` is what the
// descriptor holds; `master` says which end it is.
i64 read(void* object, bool master, void* buffer, usize count);
i64 write(void* object, bool master, const void* buffer, usize count);
void close(void* object, bool master);
void reopen(void* object, bool master);   // a fork inherited this end

// Whether either end has anything to read without blocking, for poll.
bool readable(void* object, bool master);

} // namespace pty
