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

// Inter-processor vectors, above the device range. The TLB shootdown is
// handled without taking the kernel lock - the CPU that sent it is waiting for
// an acknowledgement while holding that very lock.
constexpr u8 kTlbShootdownVector = 0xFD;

// Sent by a panicking CPU to every other one. A machine that has given up must
// actually stop: leaving the other processors running means they carry on
// writing to the console over the register dump - which is how a panic report
// ends up full of another CPU's stack - and means whatever went wrong keeps
// going on hardware nobody is watching.
constexpr u8 kHaltVector = 0xFC;

void init();

// Point this CPU at the IDT init() built; application processors share it.
void load_on_this_cpu();

// Register a handler for a hardware IRQ line (0-15, not a raw vector). The
// dispatcher acknowledges the PIC afterwards, so handlers must not.
void register_irq(u8 irq, Handler handler);

// --- interrupts for drivers in ring 3 ---------------------------------------
//
// A driver outside the kernel cannot have an interrupt delivered to it: an
// interrupt arrives in ring 0, on whatever stack happened to be current, with
// no address space of the driver's in sight. So the kernel keeps the handler
// and turns the event into something a process can wait for.
//
// The kernel's own handler still runs. That is deliberate for now: the drivers
// have not moved out yet, and a line has to be able to have both while they do.
// When a driver owns a device outright its kernel handler goes and only the
// count remains.

// Claim a line. Returns 0, or -1 if it is out of range.
i64 listen(u8 irq);

// Block until the line fires again. Returns how many times it has fired since
// the last call, so a driver that was slow learns it missed some rather than
// silently handling one event for several.
i64 wait_for(u8 irq);

} // namespace interrupts
