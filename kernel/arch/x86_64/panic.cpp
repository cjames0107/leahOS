#include <leah/apic.hpp>
#include <leah/smp.hpp>
#include <leah/console.hpp>
#include <leah/vmm.hpp>
#include <leah/cpu.hpp>
#include <leah/panic.hpp>
#include <leah/scheduler.hpp>

namespace {

// A page fault carries its detail in CR2 and the error code rather than in the
// register dump, so decode it into something readable before the raw state.
void describe_page_fault(const interrupts::Frame& frame)
{
    const u64 code = frame.error_code;

    console::printf("  faulting address : %p\n",
                    reinterpret_cast<void*>(cpu::read_cr2()));
    console::printf("  cause            : %s, %s, in %s mode\n",
                    code & 0x1 ? "protection violation" : "page not present",
                    code & 0x2 ? "write" : "read",
                    code & 0x4 ? "user" : "kernel");
    if (code & 0x10)
        console::write("  during an instruction fetch\n");

    // What the mapping actually says. "Protection violation" alone does not
    // distinguish a page that is read-only on purpose from one that is
    // copy-on-write and should have been resolved, and that is usually the
    // question being asked.
    u64 entry = 0;
    if (!vmm::recorded_fault_mapping(cpu::read_cr2(), entry))
        entry = vmm::entry_for(cpu::read_cr2());
    if (entry == 0) {
        console::write("  mapping          : none\n");
    } else {
        console::printf("  mapping          : %016llx  %s%s%s%s\n", entry,
                        (entry & vmm::Present) ? "present " : "absent ",
                        (entry & vmm::Write) ? "write " : "read-only ",
                        (entry & vmm::User) ? "user " : "kernel ",
                        (entry & vmm::CopyOnWrite) ? "copy-on-write" : "");
    }
    console::write("\n");
}

void print_header(const char* message)
{
    console::set_color(console::Color::White, console::Color::Red);
    console::write("\n  KERNEL PANIC  ");
    console::set_color(console::Color::LightRed);
    console::printf(" %s\n\n", message);
    console::set_color(console::Color::LightGray);
}

void print_registers(const interrupts::Frame& frame)
{
    console::printf("  rax %016llx  rbx %016llx  rcx %016llx\n",
                    frame.rax, frame.rbx, frame.rcx);
    console::printf("  rdx %016llx  rsi %016llx  rdi %016llx\n",
                    frame.rdx, frame.rsi, frame.rdi);
    console::printf("  rbp %016llx  rsp %016llx  rip %016llx\n",
                    frame.rbp, frame.rsp, frame.rip);
    console::printf("  r8  %016llx  r9  %016llx  r10 %016llx\n",
                    frame.r8, frame.r9, frame.r10);
    console::printf("  r11 %016llx  r12 %016llx  r13 %016llx\n",
                    frame.r11, frame.r12, frame.r13);
    console::printf("  r14 %016llx  r15 %016llx\n\n",
                    frame.r14, frame.r15);

    console::printf("  cs %04llx  ss %04llx  rflags %016llx\n",
                    frame.cs, frame.ss, frame.rflags);
    console::printf("  vector %llu  error code 0x%llx\n",
                    frame.vector, frame.error_code);
    console::printf("  cr0 %016llx  cr2 %016llx\n",
                    cpu::read_cr0(), cpu::read_cr2());
    console::printf("  cr3 %016llx  cr4 %016llx\n",
                    cpu::read_cr3(), cpu::read_cr4());
}

} // namespace

// Bring the machine to a stop before saying anything. Other processors still
// running would interleave their console output with the report - which is what
// turns a register dump into nonsense - and would carry on executing on a
// machine that has already declared itself broken.
void stop_other_cpus()
{
    if (smp::multiprocessor())
        apic::send_ipi_all_but_self(interrupts::kHaltVector);
}

[[noreturn]] void panic(const char* message)
{
    cpu::cli();
    stop_other_cpus();
    print_header(message);
    console::set_color(console::Color::DarkGray);
    console::write("\n  halted.\n");
    cpu::halt_forever();
}

[[noreturn]] void panic(const char* message, const interrupts::Frame& frame)
{
    cpu::cli();
    stop_other_cpus();
    print_header(message);
    if (frame.vector == 14)
        describe_page_fault(frame);
    print_registers(frame);
    console::set_color(console::Color::DarkGray);

    /* Whatever on the stack looks like a kernel return address.
     *
     * When the fault is a jump to nonsense, the register dump says where it
     * went and nothing about where it came from - and the code that made the
     * jump has already returned. The stack still has the frames above it, so
     * anything in the kernel's text range is a caller, and printing them names
     * the path. Not a real unwind: no frame pointers, so this is a sieve, and
     * a stale value that happens to look like an address will be in the list
     * too. It still beats guessing.
     */
    /* Whether the stack pointer is even inside the running task's own stack.
     * When it is not, everything downstream - the frames, the return addresses
     * - is somebody else's memory being read as a stack, and saying so is more
     * use than printing it. */
    {
        u64 base = 0, top = 0;
        scheduler::current_stack_bounds(&base, &top);
        console::printf("\n  running: %s (pid %u), kernel stack %016llx..%016llx\n",
                        scheduler::current_name(), scheduler::current_pid(),
                        base, top);
        if (base != 0 && (frame.rsp < base || frame.rsp >= top)) {
            u32 owner_pid = 0;
            const char* owner = scheduler::stack_owner(frame.rsp, &owner_pid);
            console::printf("  rsp is NOT in that stack; it belongs to %s\n",
                            owner != nullptr ? owner : "no live task");
            if (owner != nullptr)
                console::printf("  that is pid %u\n", owner_pid);
        }
    }

    {
        const u64* raw = reinterpret_cast<const u64*>(frame.rsp & ~7ull);
        console::write("\n  stack, from rsp:\n");
        for (u32 i = 0; i < 10; ++i)
            console::printf("    +%03u  %016llx\n", i * 8, raw[i]);
    }

    {
        const u64* sp = reinterpret_cast<const u64*>(frame.rsp & ~7ull);
        console::write("\n  kernel addresses on the stack:\n");
        u32 shown = 0;
        for (u32 i = 0; i < 64 && shown < 8; ++i) {
            const u64 value = sp[i];
            if (value >= 0xFFFFFFFF80000000ull && value < 0xFFFFFFFF80800000ull) {
                console::printf("    +%03u  %016llx\n", i * 8, value);
                ++shown;
            }
        }
    }

    console::write("\n  halted.\n");
    cpu::halt_forever();
}
