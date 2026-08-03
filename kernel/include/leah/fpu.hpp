#pragma once

#include <leah/types.hpp>

// The floating-point and SIMD unit, and whose registers are in it.
//
// x86-64 starts with SSE disabled: CR0.EM set makes any SSE instruction fault
// with #UD, and CR4.OSFXSR clear tells the CPU the operating system is not
// prepared to save the registers. Both are true of a kernel that has not
// thought about it, and this system had not - everything was built
// -mno-sse -mno-80387, so there was no floating point anywhere in it.
//
// Turning it on is two decisions. The first is telling the CPU: clear EM, set
// MP, and set OSFXSR so FXSAVE is available. The second is the real one -
// those registers are now per-task state, and something has to carry them
// across a context switch, a fork, and a signal.
//
// The kernel itself stays integer-only. It is still compiled -mno-sse, so
// kernel code never touches these registers, which is why entering the kernel
// costs nothing: there is no state to save until one *user* task gives way to
// another. That is the usual arrangement and it is worth keeping.
//
// FXSAVE, not XSAVE. FXSAVE covers x87, MMX and SSE in a fixed 512 bytes;
// XSAVE covers AVX and beyond with a variable layout negotiated through
// XCR0. Nothing here emits AVX, so the fixed form is the whole job.

namespace fpu {

// The area FXSAVE writes. Sixteen-byte alignment is architectural: FXSAVE and
// FXRSTOR fault on a misaligned operand rather than fixing it up.
struct alignas(16) State {
    u8 bytes[512];
};

// Enable the unit on the calling processor. Must run on every CPU - the
// control registers are per-processor, and an application processor that skips
// this faults on the first SSE instruction a task runs there. Returns false if
// the CPU lacks FXSAVE or SSE, in which case nothing is enabled and the system
// is exactly as it was.
bool init_this_cpu();

// Whether the unit was successfully enabled. False means every save and
// restore below is a no-op, and user code compiled for SSE will fault - which
// is a fair description of a machine too old to run this.
bool available();

// A freshly initialised unit: x87 control word 0x037F, and MXCSR 0x1F80 with
// every SIMD exception masked. Not a zeroed buffer - a zero MXCSR unmasks all
// of them, and the first denormal in a task's arithmetic would trap.
void init_state(State& state);

void save(State& state);

/* For state this kernel saved itself, which is every context switch. */
void restore(const State& state);

/* For state that came from user memory - the signal frame. FXRSTOR faults on
 * an MXCSR bit outside what the CPU implements, so a process could otherwise
 * hand sigreturn a value that makes the kernel #GP. Masks it and restores. */
void restore_untrusted(const State& state);

}  // namespace fpu
