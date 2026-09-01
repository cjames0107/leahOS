#include <leah/gdt.hpp>
#include <leah/interrupts.hpp>
#include <leah/apic.hpp>
#include <leah/pic.hpp>
#include <leah/panic.hpp>
#include <leah/console.hpp>
#include <leah/cpu.hpp>
#include <leah/scheduler.hpp>
#include <leah/spinlock.hpp>
#include <leah/syscall.hpp>
#include <leah/vmm.hpp>
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

    load_on_this_cpu();
}

void load_on_this_cpu()
{
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
namespace interrupts {
namespace {

// One counter and one channel per line. A count rather than a flag because a
// driver that is slow should be told it missed something: two frames arriving
// while it was still handling the first is a fact it needs, not one to hide.
constexpr usize kIrqLines = 16;
volatile u64 g_irq_count[kIrqLines];
volatile u64 g_irq_seen[kIrqLines];      // what the listener has collected
volatile bool g_irq_claimed[kIrqLines];

u64 irq_channel(u8 irq)
{
    return reinterpret_cast<u64>(&g_irq_count[irq]);
}

} // namespace

i64 listen(u8 irq)
{
    if (irq >= kIrqLines)
        return -1;
    g_irq_claimed[irq] = true;
    g_irq_seen[irq] = g_irq_count[irq];
    return 0;
}

i64 wait_for(u8 irq)
{
    if (irq >= kIrqLines || !g_irq_claimed[irq])
        return -1;
    /* Check then block, with interrupts already masked by the syscall entry -
     * which is what stops the line firing in the gap between deciding to sleep
     * and being asleep. */
    while (g_irq_count[irq] == g_irq_seen[irq])
        scheduler::block_on(irq_channel(irq));
    const u64 missed = g_irq_count[irq] - g_irq_seen[irq];
    g_irq_seen[irq] = g_irq_count[irq];
    return static_cast<i64>(missed);
}

// Called from the dispatcher for every hardware line.
void note_irq(u8 irq)
{
    if (irq >= kIrqLines || !g_irq_claimed[irq])
        return;
    g_irq_count[irq] = g_irq_count[irq] + 1;
    scheduler::wake(irq_channel(irq));
}

} // namespace interrupts

extern "C" void interrupt_dispatch(interrupts::Frame* frame)
{
    using namespace interrupts;

    /* Is this frame even shaped like a frame?
     *
     * Checked on the way *in*, because by the time a malformed one reaches the
     * IRETQ on the way out it has already been turned into a wild jump and the
     * register dump is the wreckage rather than the cause. Three things are
     * true of every real frame the CPU pushes: CS is one of exactly two
     * selectors, bit 1 of RFLAGS is hardwired to 1, and a frame from ring 3
     * carries a user RSP. Catching a violation here says the frame was already
     * wrong when the handler was entered - which puts the fault before this
     * point, not after it. */
    {
        const u64 cs = frame->cs;
        const bool ring3 = (cs & 3) == 3;
        const bool cs_ok = cs == 0x08 || cs == (0x20 | 3) || cs == (0x18 | 3) ||
                           cs == (0x28 | 3) || cs == 0x10;
        if (!cs_ok || (frame->rflags & 2) == 0 ||
            (ring3 && frame->rsp >= 0xFFFF800000000000ull)) {
            console::printf("\n  interrupt_dispatch: malformed frame at %p\n"
                            "    vector %llu err %llx rip %016llx cs %04llx\n"
                            "    rflags %016llx rsp %016llx ss %04llx\n",
                            frame, frame->vector, frame->error_code,
                            frame->rip, cs, frame->rflags, frame->rsp,
                            frame->ss);
            {
                u64 base = 0, top = 0;
                scheduler::current_stack_bounds(&base, &top);
                console::printf("    task %s stack %016llx..%016llx\n"
                                "    frame is %llu bytes below the top\n",
                                scheduler::current_name(), base, top,
                                top - reinterpret_cast<u64>(frame));
                const u64* w = reinterpret_cast<const u64*>(frame);
                for (u32 i = 0; i < 26; ++i)
                    console::printf("    +%03u %016llx%s\n", i * 8, w[i],
                                    i == 15 ? "  <- vector slot" :
                                    i == 17 ? "  <- rip slot" : "");
            }
            panic("interrupt_dispatch: the frame was already malformed on entry");
        }
    }

    // A TLB shootdown is serviced before anything else and without the kernel
    // lock: the CPU that sent it is holding that lock and waiting for this
    // acknowledgement, so taking it here would deadlock the pair.
    if (frame->vector == kTlbShootdownVector) {
        vmm::on_shootdown_ipi();
        apic::eoi();
        return;
    }

    // Another CPU has panicked. Stop, without taking any lock on the way - the
    // panicking CPU may well be holding it.
    if (frame->vector == kHaltVector) {
        apic::eoi();
        for (;;)
            asm volatile("cli; hlt");
    }

    // Capture what the page tables said *at the moment of the fault*. Waiting
    // for the kernel lock below can block, and blocking can switch tasks - so by
    // the time a report is printed CR3 may describe an entirely different
    // address space, and a walk done then would quote the wrong mapping.
    if (frame->vector == 14) {
        u64 address;
        asm volatile("mov %%cr2, %0" : "=r"(address));
        vmm::note_fault_mapping(address, vmm::entry_for(address));
    }

    /* The scheduler's lock, and it stays.
     *
     * This is not the syscall path and the difference is preemption: a timer
     * interrupt is where a task is taken off a processor and another put on,
     * which is the scheduler's own business. Taking it off here let two
     * processors into the same task, which the scheduler noticed and said so.
     *
     * So the big lock ends up being what it always should have been - the lock
     * over the task table and the run queue, held where scheduling happens.
     * What it is no longer is a lock around every system call. */
    sync::bkl::acquire();
    struct Unlock { ~Unlock() { sync::bkl::release(); } } unlock;

    const u64 vector = frame->vector;

    if (vector == 14) {
        // A page fault is not automatically a bug: a write to a shared
        // copy-on-write page is the normal way a forked process takes private
        // ownership of a page. Error-code bit 0 says the page was present and
        // bit 1 that the access was a write - exactly a CoW fault.
        u64 fault_address;
        asm volatile("mov %%cr2, %0" : "=r"(fault_address));
        constexpr u64 kPresent = 1, kWrite = 2;
        if ((frame->error_code & kPresent) != 0 && (frame->error_code & kWrite) != 0 &&
            vmm::handle_cow_fault(fault_address))
            return;                     // resolved: retry the faulting write

        // Not present, but reserved: a page that was asked for and is only now
        // being touched. Reads count as well as writes - a program is entitled
        // to read the zeroes it was promised.
        if ((frame->error_code & kPresent) == 0 &&
            vmm::handle_lazy_fault(fault_address))
            return;

        // A ring-3 fault that cannot be resolved is the program's bug, not the
        // kernel's, and killing the kernel over it is a category error: a
        // process that dereferences a bad pointer should die on its own. The
        // report says which one and where, which is the thing actually worth
        // knowing.
        constexpr u64 kUserMode = 4;
        if ((frame->error_code & kUserMode) != 0) {
            /* The stack pointer as well, because "where was it" is the first
             * question about any fault and the report could not answer it. A
             * fault at address 0 from an instruction that only ever touches
             * the stack means RSP is the thing that is wrong, and without this
             * that has to be worked out from a disassembly. */
            /* And the bytes at the instruction, because an address alone has
             * repeatedly not been enough: matching a fault against a
             * disassembly means trusting that the binary on the host is the
             * one that ran, and twice now it was not. Eight bytes is one
             * instruction and enough of the next to recognise it. Read only if
             * the page is really there - a fault on the instruction fetch
             * itself must not become a second fault in the handler. */
            char bytes[32];
            usize at = 0;
            const auto* code = reinterpret_cast<const u8*>(frame->rip);
            for (int i = 0; i < 8 && at + 3 < sizeof(bytes); ++i) {
                if (vmm::translate(reinterpret_cast<vaddr_t>(code + i) &
                                   ~(vmm::kPageSize - 1)) == 0)
                    break;
                static const char kHex[] = "0123456789abcdef";
                bytes[at++] = kHex[(code[i] >> 4) & 0xF];
                bytes[at++] = kHex[code[i] & 0xF];
                bytes[at++] = ' ';
            }
            bytes[at] = '\0';

            /* And the page-table entry behind the instruction. "The bytes are
             * zero" has two very different causes - the page holds zeros, or
             * the page is not the one that was mapped there - and only the
             * entry tells them apart. */
            const u64 code_pte = vmm::entry_for(frame->rip);

            console::printf("\n  %s[%u] faulted: %s at %p, %s %p%s (rsp %p) "
                            "[%s] pte %016llx\n",
                            scheduler::current_name(), scheduler::current_pid(),
                            (frame->error_code & 1) ? "protection violation"
                                                    : "unmapped address",
                            reinterpret_cast<void*>(fault_address),
                            (frame->error_code & 2) ? "writing from" : "reading from",
                            reinterpret_cast<void*>(frame->rip),
                            (frame->error_code & 0x10) ? " (instruction fetch)" : "",
                            reinterpret_cast<void*>(frame->rsp), bytes,
                            code_pte);
            scheduler::exit_current(139);      // 128 + SIGSEGV
        }
    }

    /* Any fault from ring 3 is the program's problem, not the machine's.
     *
     * The page fault path has said this for a while; every other exception
     * still brought the whole system down. A privileged instruction, a bad
     * segment, and - the one that made this urgent - an I/O port a task has
     * not been granted all arrive as a general protection fault, and a driver
     * outside the kernel touching a port it does not own must die by itself.
     * A microkernel where a driver's mistake stops the machine has given up
     * the only thing it was for.
     *
     * The low two bits of the saved CS are the privilege the fault came from,
     * which is the general form of the check the page fault handler was
     * doing with its error code. */
    if (vector < 32 && (frame->cs & 3) == 3) {
        console::printf("\n  %s[%u] faulted: %s at %p\n",
                        scheduler::current_name(), scheduler::current_pid(),
                        exception_name(vector),
                        reinterpret_cast<void*>(frame->rip));
        scheduler::exit_current(128 + 4);       // SIGILL-ish; it was illegal
    }

    if (vector < 32)
        panic(exception_name(vector), *frame);

    // The local APIC's spurious vector. It is delivered when an interrupt is
    // withdrawn mid-delivery and must not be acknowledged.
    if (vector == 0xFF)
        return;

    if (vector >= kIrqBase && vector < kIrqBase + 16) {
        const u8 irq = static_cast<u8>(vector - kIrqBase);
        // A spurious IRQ is the PIC withdrawing a line before we serviced it.
        // It must not be acknowledged or the PIC's state machine desynchronises.
        // Only meaningful while the PIC is the live controller.
        if (!apic::available() && pic::is_spurious(irq)) {
            pic::handle_spurious(irq);
            return;
        }

        if (g_irq_handlers[irq] != nullptr)
            g_irq_handlers[irq](*frame);

        /* And anyone in ring 3 waiting on this line. The kernel's own handler
         * still ran: while the drivers are being moved out, a line has to be
         * able to have both. */
        interrupts::note_irq(irq);

        // Whichever controller delivered it is the one that must be told the
        // interrupt is done. The local APIC needs no line number - it knows.
        if (apic::available())
            apic::eoi();
        else
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
