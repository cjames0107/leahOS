#pragma once

#include <leah/types.hpp>

// Per-processor state, reached through GS.
//
// Once more than one CPU runs kernel code, anything that describes "the current
// CPU" cannot be a global. x86-64 solves this with a segment base: GS points at
// a per-CPU block, so the same instruction reads a different structure on each
// core. The kernel keeps that block in GS the whole time it is executing, and
// SWAPGS exchanges it with the user's GS on every entry and exit.

namespace percpu {

// Field offsets are an ABI with syscall_entry.asm; do not reorder without
// editing it in lockstep.
struct Cpu {
    Cpu* self;              // +0   so gs:0 yields the block's own address
    u64  syscall_stack;     // +8   kernel stack SYSCALL switches to
    u64  user_rsp;          // +16  scratch, holds the user stack across entry
    u32  slot;              // +24  dense index, 0 for the bootstrap processor
    u32  apic_id;           // +28
    u32  current_task;      // +32  index into the scheduler's task table
    u32  lock_depth;        // +36  this CPU's big-kernel-lock recursion depth
    u32  shootdown_seen;    // +40  last TLB shootdown generation acknowledged
    u32  previous_task;     // +44  task switched away from, not yet finished
};

// Install this CPU's block in GS. Must run *after* gdt::init_cpu, because
// loading a data selector into GS resets its base.
void init(u32 slot, u32 apic_id);

Cpu& current();
u32  slot();

// The slot of whichever CPU is executing this instruction.
//
// This was once a global written when the kernel lock was taken, on the
// reasoning that only one CPU is inside the kernel at a time. That is not true:
// a context switch hands the lock off, and a CPU switching to a task that never
// held it carries on running kernel code with the lock released - so another
// CPU can own the global while this one is still using it. The scheduler then
// reads *that* CPU's current task, and two processors run the same task.
//
// Reading it out of GS instead is one instruction and cannot be wrong, because
// the segment base is genuinely per-processor.
u32  active();

// Record the kernel stack SYSCALL switches to on this CPU. Kept per CPU here,
// and also written to the global the entry stub still reads - see the comment
// on the definition for why that global has to go before an application
// processor can run user code.
void set_syscall_stack_for_this_cpu(u64 rsp);

} // namespace percpu
