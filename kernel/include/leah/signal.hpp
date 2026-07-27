#pragma once

#include <leah/types.hpp>

// Signal numbers and dispositions. The set is deliberately small - the ones a
// shell and a job-control-less system actually use - and the numbers match
// Linux's so the names read the same everywhere. Mirrored in
// user/libc/include/signal.h.

namespace signals {

constexpr u32 kMaxSignals = 32;

constexpr int kSigHup  = 1;
constexpr int kSigInt  = 2;
constexpr int kSigQuit = 3;
constexpr int kSigKill = 9;     // never catchable
constexpr int kSigUsr1 = 10;
constexpr int kSigSegv = 11;
constexpr int kSigUsr2 = 12;
constexpr int kSigTerm = 15;
constexpr int kSigChld = 17;

// Handler slots. A real function pointer is any other value.
constexpr u64 kSigDefault = 0;
constexpr u64 kSigIgnore  = 1;

// True when the default disposition is to kill the process. Everything else
// (notably SIGCHLD) defaults to being ignored.
constexpr bool default_kills(int signo)
{
    return signo == kSigHup || signo == kSigInt || signo == kSigQuit ||
           signo == kSigKill || signo == kSigSegv || signo == kSigTerm ||
           signo == kSigUsr1 || signo == kSigUsr2;
}

} // namespace signals
