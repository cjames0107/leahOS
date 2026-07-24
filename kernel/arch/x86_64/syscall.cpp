#include <leah/console.hpp>
#include <leah/cpu.hpp>
#include <leah/gdt.hpp>
#include <leah/syscall.hpp>

// Implemented in syscall_entry.asm.
extern "C" void syscall_entry();
extern "C" [[noreturn]] void exit_to_kernel(u64 code);
extern "C" u64 enter_user_mode(vaddr_t entry, vaddr_t user_stack);
extern "C" void set_syscall_stack(u64 rsp);

namespace syscall {
namespace {

constexpr u32 kIa32Efer  = 0xC0000080;
constexpr u32 kIa32Star  = 0xC0000081;
constexpr u32 kIa32Lstar = 0xC0000082;
constexpr u32 kIa32Fmask = 0xC0000084;

constexpr u64 kEferSyscallEnable = 1ull << 0;   // EFER.SCE

// One kernel stack, used both for the SYSCALL trampoline and for interrupts
// taken while in ring 3 (via the TSS). They never overlap in time: SYSCALL
// masks IF through FMASK, and the ISR gates keep IF clear, so no interrupt
// runs while a syscall is on this stack and vice versa.
alignas(16) u8 g_kernel_stack[32 * 1024];

u64 kernel_stack_top()
{
    return reinterpret_cast<u64>(g_kernel_stack) + sizeof(g_kernel_stack);
}

// A user pointer must not be trusted to point where the program claimed. The
// real check needs the process's address space; for now, reject anything that
// reaches into the kernel's higher half, which is the mapping that matters.
bool user_range_ok(vaddr_t base, u64 length)
{
    constexpr vaddr_t kUserCeiling = 0x0000800000000000ull;   // canonical low half
    if (base >= kUserCeiling)
        return false;
    if (length > kUserCeiling - base)
        return false;
    return true;
}

// --- individual calls ------------------------------------------------------

u64 sys_write(u64 fd, vaddr_t buffer, u64 length)
{
    if (fd != 1 && fd != 2)                       // stdout, stderr
        return static_cast<u64>(-1);
    if (length == 0)
        return 0;
    if (!user_range_ok(buffer, length))
        return static_cast<u64>(-1);

    const char* text = reinterpret_cast<const char*>(buffer);
    for (u64 i = 0; i < length; ++i)
        console::put(text[i]);
    return length;
}

} // namespace

void init()
{
    set_syscall_stack(kernel_stack_top());
    gdt::set_kernel_stack(kernel_stack_top());

    // Turn the SYSCALL/SYSRET pair on.
    cpu::write_msr(kIa32Efer, cpu::read_msr(kIa32Efer) | kEferSyscallEnable);

    // STAR selectors, and the one piece of this whole path most likely to
    // triple-fault if it is off by a slot.
    //
    // SYSCALL:  CS = bits[47:32],       SS = bits[47:32] + 8
    // SYSRET :  CS = (bits[63:48]+16)|3, SS = (bits[63:48]+8)|3
    //
    // So the SYSRET base is not the user selector itself - it is eight below
    // user data, chosen so +8 lands on user data and +16 on user code. That
    // only works because the GDT is ordered kernel-code, kernel-data,
    // user-data, user-code with no gaps.
    const u16 syscall_base = gdt::kKernelCode;        // -> CS 0x08, SS 0x10
    const u16 sysret_base  = gdt::kUserData - 8;      // +8 -> 0x18, +16 -> 0x20
    const u64 star = static_cast<u64>(syscall_base) << 32
                   | static_cast<u64>(sysret_base) << 48;
    cpu::write_msr(kIa32Star, star);

    // Where SYSCALL jumps.
    cpu::write_msr(kIa32Lstar, reinterpret_cast<u64>(&syscall_entry));

    // Bits set here are cleared in RFLAGS on entry. Clearing IF means the
    // handler runs with interrupts off; clearing DF and TF avoids inheriting a
    // string direction or single-step from the user program.
    cpu::write_msr(kIa32Fmask, (1ull << 9) | (1ull << 10) | (1ull << 8));
}

u64 run(vaddr_t entry, vaddr_t user_stack_top)
{
    // Interrupts taken in ring 3 land on this stack too.
    gdt::set_kernel_stack(kernel_stack_top());
    return enter_user_mode(entry, user_stack_top);
}

} // namespace syscall

// ----------------------------------------------------------------------------
// Called from syscall_entry with the saved register frame.
// ----------------------------------------------------------------------------
extern "C" void syscall_dispatch(syscall::Frame* frame)
{
    using namespace syscall;

    switch (frame->rax) {
    case Exit:
        // Never returns: unwinds back out of syscall::run().
        exit_to_kernel(frame->rdi);

    case Write:
        frame->rax = sys_write(frame->rdi, frame->rsi, frame->rdx);
        break;

    case GetPid:
        frame->rax = 1;                           // only one "process" so far
        break;

    default:
        frame->rax = static_cast<u64>(-1);
        break;
    }
}
