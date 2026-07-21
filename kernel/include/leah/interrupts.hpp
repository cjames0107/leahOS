#pragma once

#include <leah/types.hpp>

namespace interrupts {

// The register state an ISR sees, laid out to match exactly what isr.asm
// pushes. Field order here is a hard contract with that file: the general
// purpose registers appear in reverse push order, then the vector and error
// code the stub supplied, then the frame the CPU itself pushed.
//
// Do not reorder anything below without editing isr_common in lockstep.
struct [[gnu::packed]] Frame {
    // pushed by isr_common, last push first
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;

    // pushed by the per-vector stub
    u64 vector;
    u64 error_code;

    // pushed by the CPU on entry
    u64 rip;
    u64 cs;
    u64 rflags;
    u64 rsp;
    u64 ss;
};

static_assert(sizeof(Frame) == 22 * 8, "Frame must match isr_common's pushes");

using Handler = void (*)(Frame&);

// Hardware IRQ lines, after the PIC has been remapped away from the CPU's
// exception vectors.
constexpr u8 kIrqBase     = 32;
constexpr u8 kIrqTimer    = 0;
constexpr u8 kIrqKeyboard = 1;

void init();

// Register a handler for a hardware IRQ line (0-15, not a raw vector). The
// dispatcher acknowledges the PIC afterwards, so handlers must not.
void register_irq(u8 irq, Handler handler);

} // namespace interrupts
