#pragma once

#include <leah/types.hpp>

// Signal numbers and dispositions. The numbers match Linux's so the names read
// the same everywhere. Mirrored in user/libc/include/signal.h.

namespace signals {

constexpr u32 kMaxSignals = 32;

constexpr int kSigHup  = 1;
constexpr int kSigInt  = 2;
constexpr int kSigQuit = 3;
constexpr int kSigKill = 9;     // never catchable
constexpr int kSigUsr1 = 10;
constexpr int kSigSegv = 11;
constexpr int kSigUsr2 = 12;
constexpr int kSigPipe = 13;    // wrote to a pipe nobody is reading
constexpr int kSigTerm = 15;
constexpr int kSigChld = 17;
// Job control. A shell that can suspend a program needs all of these: two to
// stop it, one to start it again, and two more for a background program
// reaching for a terminal that is not currently its.
constexpr int kSigCont = 18;
constexpr int kSigStop = 19;    // never catchable, like kill
constexpr int kSigTstp = 20;    // what the keyboard sends, and catchable
constexpr int kSigTtin = 21;
constexpr int kSigTtou = 22;

// Handler slots. A real function pointer is any other value.
constexpr u64 kSigDefault = 0;
constexpr u64 kSigIgnore  = 1;

// True when the default disposition is to kill the process. Everything else
// defaults to being ignored - notably SIGCHLD, and SIGCONT, whose whole effect
// happens before a disposition is ever consulted.
constexpr bool default_kills(int signo)
{
    return signo == kSigHup || signo == kSigInt || signo == kSigQuit ||
           signo == kSigKill || signo == kSigSegv || signo == kSigTerm ||
           signo == kSigPipe || signo == kSigUsr1 || signo == kSigUsr2;
}

// True when the default disposition is to stop the process rather than kill it.
// SIGSTOP is here and cannot be caught; the other three can be, which is the
// difference between a shell suspending a program and a program deciding it
// would rather not be suspended just now.
constexpr bool default_stops(int signo)
{
    return signo == kSigStop || signo == kSigTstp ||
           signo == kSigTtin || signo == kSigTtou;
}

// The two that cannot be caught or ignored. That is the whole reason they
// exist: whatever a program has decided about signals, it can still be stopped
// and it can still be killed.
constexpr bool uncatchable(int signo)
{
    return signo == kSigKill || signo == kSigStop;
}

// --- what came back out of wait ---------------------------------------------
//
// One word has to carry three different endings, and "the exit code" is not
// enough for a system that can suspend a program: stopped is not exited, and a
// shell has to be able to tell them apart to know whether the job is still
// there.
//
//   0x000 | code    ran to the end, or called exit
//   0x100 | signo   killed by a signal
//   0x200 | signo   stopped by a signal, and still there
//   0x300           started again by SIGCONT
//
// The layout is this system's own rather than the bit-packing Linux uses, for
// the reason the rest of this kernel keeps giving: nothing here has to
// interoperate, and a number somebody can read at a glance is worth more than
// one that happens to match another kernel's macros.
//
// An ordinary exit is *just its code*, which is what every caller in this
// system was already comparing against - which is why almost nothing had to
// change when this arrived. Mirrored in user/libc/include/sys/wait.h.
constexpr i32 kExited     = 0x000;
constexpr i32 kSignalled  = 0x100;
constexpr i32 kStopped    = 0x200;
constexpr i32 kContinued  = 0x300;
constexpr i32 kStatusKind = 0x300;      // the bits that say which of the four
constexpr i32 kStatusData = 0x0FF;      // the code, or the signal number

} // namespace signals
