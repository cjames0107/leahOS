#include <leah/console.hpp>
#include <leah/cpu.hpp>
#include <leah/file.hpp>
#include <leah/gdt.hpp>
#include <leah/memory.hpp>
#include <leah/net.hpp>
#include <leah/pmm.hpp>
#include <leah/process.hpp>
#include <leah/scheduler.hpp>
#include <leah/string.hpp>
#include <leah/syscall.hpp>
#include <leah/vfs.hpp>
#include <leah/vmm.hpp>

extern "C" void syscall_entry();

namespace syscall {
namespace {

constexpr u32 kIa32Efer  = 0xC0000080;
constexpr u32 kIa32Star  = 0xC0000081;
constexpr u32 kIa32Lstar = 0xC0000082;
constexpr u32 kIa32Fmask = 0xC0000084;

constexpr u64 kEferSyscallEnable = 1ull << 0;

// A user pointer must not be trusted to point where the program claimed. The
// full check needs the process's address space; for now, reject anything that
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

// Grow (or query) the process's heap. Returns the previous break, or -1. The
// new pages are mapped into the process's own space, which is active during the
// syscall, so zeroing them is a plain write.
i64 sys_sbrk(i64 increment)
{
    const u64 old_brk = scheduler::current_brk();
    if (increment == 0)
        return static_cast<i64>(old_brk);
    if (increment < 0) {
        scheduler::set_current_brk(old_brk - static_cast<u64>(-increment));
        return static_cast<i64>(old_brk);
    }

    const u64 new_brk = old_brk + static_cast<u64>(increment);
    for (u64 page = old_brk & ~(vmm::kPageSize - 1); page < new_brk;
         page += vmm::kPageSize) {
        if (vmm::translate(page) != 0)
            continue;                                   // already mapped
        const paddr_t frame = pmm::alloc();
        if (frame == 0)
            return -1;
        if (!vmm::map(page, frame, vmm::Write | vmm::User | vmm::NoExecute)) {
            pmm::free(frame);
            return -1;
        }
        memset(reinterpret_cast<void*>(page), 0, vmm::kPageSize);
    }

    scheduler::set_current_brk(new_brk);
    return static_cast<i64>(old_brk);
}

// Turn the saved syscall registers into the full ring-3 frame a forked child
// resumes on. The child re-enters user mode through IRETQ (see user_entry.asm)
// rather than SYSRET, so it needs CS/SS/RFLAGS as well as the general set.
scheduler::TrapFrame to_trap_frame(const Frame& f)
{
    scheduler::TrapFrame tf{};
    tf.r15 = f.r15; tf.r14 = f.r14; tf.r13 = f.r13; tf.r12 = f.r12;
    tf.r11 = f.r11; tf.r10 = f.r10; tf.r9 = f.r9; tf.r8 = f.r8;
    tf.rbp = f.rbp; tf.rdi = f.rdi; tf.rsi = f.rsi; tf.rdx = f.rdx;
    tf.rcx = 0;     tf.rbx = f.rbx; tf.rax = f.rax;
    tf.rip = f.user_rip;
    tf.cs  = kUserCode;
    tf.rflags = f.user_flags;
    tf.rsp = f.user_rsp;
    tf.ss  = kUserData;
    return tf;
}

} // namespace

void init()
{
    cpu::write_msr(kIa32Efer, cpu::read_msr(kIa32Efer) | kEferSyscallEnable);

    // STAR selectors. On SYSRET the base is eight below user data, so +8 lands
    // on user data and +16 on user code; on SYSCALL the base is kernel code.
    const u64 star = static_cast<u64>(gdt::kKernelCode) << 32
                   | static_cast<u64>(gdt::kUserData - 8) << 48;
    cpu::write_msr(kIa32Star, star);
    cpu::write_msr(kIa32Lstar, reinterpret_cast<u64>(&syscall_entry));

    // Clear IF, DF and TF on entry: the handler runs with interrupts off and
    // inherits no string direction or single-step from the program.
    cpu::write_msr(kIa32Fmask, (1ull << 9) | (1ull << 10) | (1ull << 8));
}

} // namespace syscall

extern "C" void syscall_dispatch(syscall::Frame* frame)
{
    using namespace syscall;

    switch (frame->rax) {
    case Exit:
        scheduler::exit_current(static_cast<i32>(frame->rdi));   // never returns

    case Write:
        frame->rax = static_cast<u64>(
            files::write(static_cast<int>(frame->rdi),
                         reinterpret_cast<const void*>(frame->rsi), frame->rdx));
        break;

    case Read:
        frame->rax = static_cast<u64>(
            files::read(static_cast<int>(frame->rdi),
                        reinterpret_cast<void*>(frame->rsi), frame->rdx));
        break;

    case GetPid:
        frame->rax = scheduler::current_pid();
        break;

    case Open:
        frame->rax = static_cast<u64>(
            files::open(reinterpret_cast<const char*>(frame->rdi),
                        static_cast<u32>(frame->rsi)));
        break;

    case Close:
        frame->rax = static_cast<u64>(files::close(static_cast<int>(frame->rdi)));
        break;

    case Lseek:
        frame->rax = static_cast<u64>(
            files::lseek(static_cast<int>(frame->rdi),
                         static_cast<i64>(frame->rsi), static_cast<int>(frame->rdx)));
        break;

    case Stat:
        frame->rax = static_cast<u64>(
            files::stat(reinterpret_cast<const char*>(frame->rdi),
                        reinterpret_cast<void*>(frame->rsi)));
        break;

    case Getdents:
        frame->rax = static_cast<u64>(
            files::getdents(reinterpret_cast<const char*>(frame->rdi),
                            reinterpret_cast<void*>(frame->rsi), frame->rdx));
        break;

    case Chdir:
        frame->rax = static_cast<u64>(
            files::chdir(reinterpret_cast<const char*>(frame->rdi)));
        break;

    case Getcwd:
        frame->rax = static_cast<u64>(
            files::getcwd(reinterpret_cast<char*>(frame->rdi), frame->rsi));
        break;

    case Mkdir:
        frame->rax = static_cast<u64>(
            files::mkdir(reinterpret_cast<const char*>(frame->rdi)));
        break;

    case Unlink:
        frame->rax = static_cast<u64>(
            files::unlink(reinterpret_cast<const char*>(frame->rdi)));
        break;

    case Pipe:
        frame->rax = static_cast<u64>(
            files::pipe(reinterpret_cast<int*>(frame->rdi)));
        break;

    case Dup2:
        frame->rax = static_cast<u64>(
            files::dup2(static_cast<int>(frame->rdi), static_cast<int>(frame->rsi)));
        break;

    case Sbrk:
        frame->rax = static_cast<u64>(sys_sbrk(static_cast<i64>(frame->rdi)));
        break;

    case Rename:
        frame->rax = static_cast<u64>(
            files::rename(reinterpret_cast<const char*>(frame->rdi),
                          reinterpret_cast<const char*>(frame->rsi)));
        break;

    case Netinfo: {
        if (!net::available() || !user_range_ok(frame->rdi, sizeof(net::Info))) {
            frame->rax = static_cast<u64>(-1);
            break;
        }
        net::info(*reinterpret_cast<net::Info*>(frame->rdi));
        frame->rax = 0;
        break;
    }

    case Ping: {
        if (!net::available()) {
            frame->rax = static_cast<u64>(-1);
            break;
        }
        u8 ttl = 0;
        const bool ok = net::ping(static_cast<u32>(frame->rdi),
                                  static_cast<u16>(frame->rsi), &ttl);
        if (ok && frame->rdx != 0 && user_range_ok(frame->rdx, sizeof(u8)))
            *reinterpret_cast<u8*>(frame->rdx) = ttl;
        frame->rax = ok ? 1 : 0;
        break;
    }

    case Arp: {
        if (!net::available() || !user_range_ok(frame->rsi, net::kMacLength)) {
            frame->rax = static_cast<u64>(-1);
            break;
        }
        const bool ok = net::arp_resolve(static_cast<u32>(frame->rdi),
                                         reinterpret_cast<u8*>(frame->rsi));
        frame->rax = ok ? 0 : static_cast<u64>(-1);
        break;
    }

    case Fork:
        frame->rax = scheduler::fork_current(to_trap_frame(*frame));
        break;

    case Execve:
        // On success this rewrites frame to enter the new image and never
        // "returns" to the caller; on failure it leaves frame->rax = -1.
        process::exec(*frame, reinterpret_cast<const char*>(frame->rdi),
                      reinterpret_cast<char**>(frame->rsi));
        break;

    case Wait: {
        i32 status = 0;
        const i64 pid = scheduler::wait_child(&status);
        if (pid >= 0 && frame->rsi != 0 && user_range_ok(frame->rsi, sizeof(i32)))
            *reinterpret_cast<i32*>(frame->rsi) = status;
        frame->rax = static_cast<u64>(pid);
        break;
    }

    case Yield:
        scheduler::yield();
        frame->rax = 0;
        break;

    default:
        frame->rax = static_cast<u64>(-1);
        break;
    }
}
