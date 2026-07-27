#pragma once

#include <leah/types.hpp>

namespace gdt {

// Selector values are an ABI, not an arbitrary numbering. SYSCALL/SYSRET read
// a single base out of the STAR MSR and derive four selectors from it by fixed
// offsets, which forces this exact order:
//
//   SYSCALL:  CS = STAR[47:32]        SS = STAR[47:32] + 8
//   SYSRET :  CS = STAR[63:48] + 16   SS = STAR[63:48] + 8
//
// With kernel code at 0x08 and user data at 0x18, both work out. Reordering
// these to something more natural-looking would silently break ring 3 later.
constexpr u16 kKernelCode = 0x08;
constexpr u16 kKernelData = 0x10;
constexpr u16 kUserData   = 0x18;
constexpr u16 kUserCode   = 0x20;
constexpr u16 kTss        = 0x28;

// Interrupt Stack Table slot used for the faults that cannot be allowed to
// push onto whatever stack was current. See gdt.cpp.
constexpr u8 kIstDoubleFault = 1;

void init();

// Build and load this processor's own GDT and TSS. Each CPU needs its own,
// because the TSS carries the ring-0 stack the CPU switches to on a trap from
// ring 3 - sharing one would land two cores on the same stack.
void init_cpu(u32 slot);

// Stack the given CPU switches to on a ring 3 -> ring 0 transition.
void set_kernel_stack(u32 slot, u64 rsp0);

} // namespace gdt
