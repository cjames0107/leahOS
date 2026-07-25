#pragma once

#include <leah/types.hpp>

// Running a program as a process: its own address space, loaded from an ELF on
// disk, executed in ring 3. This is the piece execve() will become - for now it
// runs one program to completion and tears the address space down after.

namespace process {

struct Result {
    bool loaded;        // did the ELF load and run at all
    u64  exit_code;     // meaningful only when loaded
};

// Where a program's ring-3 stack lives. Its own slot, well clear of the
// program image, both in the user (low) half of the address space.
constexpr vaddr_t kUserStackTop   = 0x0000700000000000ull;
constexpr usize   kUserStackPages = 16;

Result run(const char* path);

} // namespace process
