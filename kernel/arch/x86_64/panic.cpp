#include <leah/console.hpp>
#include <leah/cpu.hpp>
#include <leah/panic.hpp>

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

[[noreturn]] void panic(const char* message)
{
    cpu::cli();
    print_header(message);
    console::set_color(console::Color::DarkGray);
    console::write("\n  halted.\n");
    cpu::halt_forever();
}

[[noreturn]] void panic(const char* message, const interrupts::Frame& frame)
{
    cpu::cli();
    print_header(message);
    if (frame.vector == 14)
        describe_page_fault(frame);
    print_registers(frame);
    console::set_color(console::Color::DarkGray);
    console::write("\n  halted.\n");
    cpu::halt_forever();
}
