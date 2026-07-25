#pragma once

#include <leah/types.hpp>

// The system call interface, and the machinery to run a program in ring 3.
//
// Calls come in through the SYSCALL instruction: number in RAX, arguments in
// RDI, RSI, RDX, R10, R8, R9 - the SysV order with R10 standing in for RCX,
// which SYSCALL destroys. The return value comes back in RAX.

namespace syscall {

// leahOS's own numbers, not Linux's; there is no compatibility to preserve yet.
enum Number : u64 {
    Exit    = 0,
    Write   = 1,
    Read    = 2,
    GetPid  = 3,
    Fork    = 4,
    Execve  = 5,
    Wait    = 6,
    Yield   = 7,
    Open    = 8,
    Close   = 9,
    Lseek   = 10,
    Stat    = 11,
    Getdents = 12,
    Chdir   = 13,
    Getcwd  = 14,
    Mkdir   = 15,
    Unlink  = 16,
    Pipe    = 17,
    Dup2    = 18,
};

// The register state a syscall handler sees. Field order is a contract with
// syscall_entry in syscall_entry.asm - do not reorder without editing both.
struct [[gnu::packed]] Frame {
    u64 r15, r14, r13, r12, rbp, rbx;    // callee-saved
    u64 r11, r10, r9, r8;
    u64 rdx, rsi, rdi;
    u64 rax;                             // number on entry, return value on exit
    u64 user_rip;                        // RCX at entry: where SYSCALL came from
    u64 user_flags;                      // R11 at entry
    u64 user_rsp;
};

// Selectors a user process runs with, RPL 3. Used to build the register frame
// a new or forked process resumes on.
constexpr u64 kUserCode = 0x20 | 3;
constexpr u64 kUserData = 0x18 | 3;
constexpr u64 kUserFlags = 0x202;       // IF set

void init();

} // namespace syscall
