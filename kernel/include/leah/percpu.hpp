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
    u32  reserved;
};

// Install this CPU's block in GS. Must run *after* gdt::init_cpu, because
// loading a data selector into GS resets its base.
void init(u32 slot, u32 apic_id);

Cpu& current();
u32  slot();

} // namespace percpu
