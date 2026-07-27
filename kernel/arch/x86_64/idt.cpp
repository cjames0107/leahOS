#include <leah/gdt.hpp>
#include <leah/interrupts.hpp>
#include <leah/pic.hpp>
#include <leah/panic.hpp>
#include <leah/console.hpp>
#include <leah/cpu.hpp>
#include <leah/scheduler.hpp>
#include <leah/syscall.hpp>
#include <leah/string.hpp>

// Provided by isr.asm: 256 entry points, one per vector.
extern "C" void* isr_stub_table[];

namespace interrupts {
namespace {

struct [[gnu::packed]] Entry {
    u16 offset_low;
    u16 selector;
    u8  ist;            // 0 = use the current stack, 1-7 = TSS IST slot
    u8 type_attributes;
    u16 offset_mid;
    u32 offset_high;
    u32 reserved;
};

struct [[gnu::packed]] Pointer {
    u16 limit;
    u64 base;
};

// P=1, DPL=0, type 0xE = 64-bit interrupt gate. An interrupt gate clears IF on
// entry; a trap gate (0xF) would not. We want handlers to start with
// interrupts masked and opt in, rather than opt out.
constexpr u8 kInterruptGate = 0x8E;

alignas(16) Entry g_idt[256]{};

const char* const kExceptionNames[32] = {
    "divide error",                 // 0  #DE
    "debug",                        // 1  #DB
    "non-maskable interrupt",       // 2  NMI
    "breakpoint",                   // 3  #BP
    "overflow",                     // 4  #OF
    "bound range exceeded",         // 5  #BR
    "invalid opcode",               // 6  #UD
    "device not available",         // 7  #NM
    "double fault",                 // 8  #DF
    "coprocessor segment overrun",  // 9
    "invalid TSS",                  // 10 #TS
    "segment not present",          // 11 #NP
    "stack-segment fault",          // 12 #SS
    "general protection fault",     // 13 #GP
    "page fault",                   // 14 #PF
    "reserved",                     // 15
    "x87 floating-point exception", // 16 #MF
    "alignment check",              // 17 #AC
    "machine check",                // 18 #MC
    "SIMD floating-point exception",// 19 #XM
    "virtualization exception",     // 20 #VE
    "control protection exception", // 21 #CP
    "reserved", "reserved", "reserved", "reserved",
    "reserved", "reserved",
    "hypervisor injection",         // 28 #HV
    "VMM communication",            // 29 #VC
    "security exception",           // 30 #SX
    "reserved",                     // 31
};

Handler g_irq_handlers[16]{};

void set_entry(u8 vector, void* handler, u8 ist = 0)
{
    const u64 address = reinterpret_cast<u64>(handler);
    g_idt[vector] = Entry{
        .offset_low      = static_cast<u16>(address & 0xFFFF),
        .selector        = gdt::kKernelCode,
        .ist             = ist,
        .type_attributes = kInterruptGate,
        .offset_mid      = static_cast<u16>(address >> 16 & 0xFFFF),
        .offset_high     = static_cast<u32>(address >> 32),
        .reserved        = 0,
    };
}

} // namespace

const char* exception_name(u64 vector)
{
    return vector < 32 ? kExceptionNames[vector] : "unknown exception";
}

void register_irq(u8 irq, Handler handler)
{
    if (irq < 16)
        g_irq_handlers[irq] = handler;
}

void init()
{
    for (u32 vector = 0; vector < 256; ++vector)
        set_entry(static_cast<u8>(vector), isr_stub_table[vector]);

    // #DF is the one vector that cannot trust the stack it arrived on - see
    // the IST commentary in gdt.cpp.
    set_entry(8, isr_stub_table[8], gdt::kIstDoubleFault);

    const Pointer pointer{
        .limit = sizeof(g_idt) - 1,
        .base  = reinterpret_cast<u64>(&g_idt),
    };
    asm volatile("lidt %0" : : "m"(pointer));
}

} // namespace interrupts

// ----------------------------------------------------------------------------
// Called from isr_common with the saved register state.
// ----------------------------------------------------------------------------
extern "C" void interrupt_dispatch(interrupts::Frame* frame)
{
    using namespace interrupts;

    const u64 vector = frame->vector;

    if (vector < 32)
        panic(exception_name(vector), *frame);

    if (vector >= kIrqBase && vector < kIrqBase + 16) {
        const u8 irq = static_cast<u8>(vector - kIrqBase);
        // A spurious IRQ is the PIC withdrawing a line before we serviced it.
        // It must not be acknowledged or the PIC's state machine desynchronises.
        if (pic::is_spurious(irq)) {
            pic::handle_spurious(irq);
            return;
        }

        if (g_irq_handlers[irq] != nullptr)
            g_irq_handlers[irq](*frame);

        pic::end_of_interrupt(irq);

        // Only now, with the interrupt acknowledged, is it safe to let the
        // scheduler switch threads: switching before the EOI would leave the
        // PIC holding this line and starve every other thread of timer ticks.
        scheduler::on_irq_return();

        // Last thing before the IRETQ back to ring 3: a signal sent to a task
        // that never makes syscalls still gets delivered, because the timer
        // interrupt reaches it here.
        syscall::deliver_on_interrupt(*frame);
        return;
    }

    // Anything else is a software interrupt we never asked for.
}
