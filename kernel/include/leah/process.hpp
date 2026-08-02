#pragma once

#include <leah/syscall.hpp>
#include <leah/types.hpp>

// Turning an ELF into a running process. create_embedded() makes a brand-new
// process from bytes the kernel carries; exec() replaces the calling process's
// image with bytes the caller read. Both build the
// same thing - a fresh address space with the program's segments and a ring-3
// stack - they differ only in whether a new task is created or the current one
// is reused.

namespace process {

// Where a program's ring-3 stack lives, in its own slot of the user half.
constexpr vaddr_t kUserStackTop   = 0x0000700000000000ull;
constexpr usize   kUserStackPages = 16;

// Create a new process from `path`, as a child of `parent_pid`. Returns its
// pid, or 0 on failure. The process is runnable but not run until scheduled.

// The same, from an image already in memory rather than a path. Used for the
// two servers the kernel carries, which have to run before there is a
// filesystem for anything to be loaded from.
u32 create_embedded(const char* name, const u8* image, usize size, u32 parent_pid);

// Replace the calling process's image with `path`, passing `argv`. On success it
// rewrites `frame` so the syscall returns into the new program on a fresh argv
// stack and frees the old address space; on failure it sets frame.rax to -1 and
// leaves the caller running.
// Replace the calling process's image with one the caller has already read.
// Bytes, not a path: the kernel has no filesystem client left, and reaching
// into one from the middle of building an address space was the last place it
// blocked as somebody else's client.
void exec(syscall::Frame& frame, const u8* image, usize size, char** argv);

} // namespace process
