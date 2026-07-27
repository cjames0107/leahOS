#pragma once

#include <leah/types.hpp>

// Bringing the application processors out of reset.
//
// A multiprocessor x86 machine boots exactly one core; the rest sit halted
// until the bootstrap processor pokes them with an INIT followed by a startup
// IPI. Each then begins in 16-bit real mode at a page the SIPI names, walks
// itself up to long mode through boot/ap_trampoline.asm, and lands in C here.
//
// This brings them up and parks them. Letting them *run tasks* is a separate
// step: every structure the scheduler touches would need locking first, and a
// half-locked scheduler on two cores is far worse than one core that works.

namespace smp {

// Start every processor the MADT listed. Returns how many are online,
// including the bootstrap processor (so 1 means no APs started).
usize init();

usize cpu_count();

// True once more than one processor is running.
bool multiprocessor();

// True once the application processors are actually scheduling, as opposed to
// merely being awake. TLB shootdown depends on this and not on the CPU count: a
// parked processor is halted with interrupts off and can never acknowledge one,
// so enabling shootdowns for it would make every unmap wait out its full
// timeout.
bool scheduling();

} // namespace smp
