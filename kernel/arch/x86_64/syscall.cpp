#include <leah/console.hpp>
#include <leah/cpu.hpp>
#include <leah/file.hpp>
#include <leah/gdt.hpp>
#include <leah/memory.hpp>
#include <leah/net.hpp>
#include <leah/pmm.hpp>
#include <leah/process.hpp>
#include <leah/scheduler.hpp>
#include <leah/signal.hpp>
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

// Anonymous memory, page-granular. Only private anonymous mappings are
// supported: file-backed mmap needs a page cache to be worth having, and the
// programs that want memory here want it zeroed, not shared with a file.
// Returns the base address, or -1.
i64 sys_mmap(u64 addr, u64 length, u64 prot, u64 flags)
{
    if (length == 0)
        return -1;
    if ((flags & kMapAnonymous) == 0)
        return -1;                                      // file-backed: unsupported

    const u64 pages = (length + vmm::kPageSize - 1) / vmm::kPageSize;
    const u64 bytes = pages * vmm::kPageSize;

    u64 base;
    if ((flags & kMapFixed) != 0 && addr != 0) {
        base = addr & ~(vmm::kPageSize - 1);
        if (!user_range_ok(base, bytes))
            return -1;
    } else {
        base = scheduler::current_mmap_next();
        if (base + bytes > memory::kUserMmapEnd)
            return -1;                                  // mmap arena exhausted
        scheduler::set_current_mmap_next(base + bytes);
    }

    u64 page_flags = vmm::User;
    if ((prot & kProtWrite) != 0)
        page_flags |= vmm::Write;
    if ((prot & kProtExec) == 0)
        page_flags |= vmm::NoExecute;

    for (u64 offset = 0; offset < bytes; offset += vmm::kPageSize) {
        const u64 page = base + offset;
        if (vmm::translate(page) != 0)
            continue;                                   // already mapped
        const paddr_t frame = pmm::alloc();
        if (frame == 0)
            return -1;
        // Map writable first so the zeroing below can happen, then tighten to
        // the requested protection - a read-only mapping must still start zeroed.
        if (!vmm::map(page, frame, page_flags | vmm::Write)) {
            pmm::free(frame);
            return -1;
        }
        memset(reinterpret_cast<void*>(page), 0, vmm::kPageSize);
        if ((page_flags & vmm::Write) == 0) {
            vmm::unmap(page);
            vmm::map(page, frame, page_flags);
        }
    }
    return static_cast<i64>(base);
}

// Unmap a range and return its frames. Addresses outside the user half, or
// pages that were never mapped, are skipped rather than treated as an error -
// munmap of a partly-unmapped range is legal.
i64 sys_munmap(u64 addr, u64 length)
{
    if (length == 0)
        return -1;
    const u64 base = addr & ~(vmm::kPageSize - 1);
    const u64 pages = (length + vmm::kPageSize - 1) / vmm::kPageSize;
    const u64 bytes = pages * vmm::kPageSize;
    if (!user_range_ok(base, bytes))
        return -1;

    for (u64 offset = 0; offset < bytes; offset += vmm::kPageSize) {
        const u64 page = base + offset;
        const paddr_t frame = vmm::translate(page);
        if (frame == 0)
            continue;
        vmm::unmap(page);
        pmm::free(frame);
    }
    return 0;
}

// What a signal handler's frame leaves on the user stack so sigreturn can put
// everything back. Saving the whole syscall frame is both simplest and exact:
// the interrupted context is precisely what the syscall was going to restore.
struct [[gnu::packed]] SignalContext {
    Frame frame;
};

// Rewrite the outgoing user context so it enters `handler` instead of resuming
// where the syscall left off. The handler's frame is built on the user's own
// stack: the saved context, then a return address pointing at libc's restorer,
// which calls sigreturn to undo all of this.
void deliver_signal(Frame* frame, int signo, u64 handler)
{
    u64 sp = frame->user_rsp;
    sp -= 128;                          // skip the SysV red zone
    sp -= sizeof(SignalContext);
    sp &= ~0xFull;                      // the ABI's 16-byte alignment

    const u64 restorer = scheduler::signal_restorer();
    if (restorer == 0 || !user_range_ok(sp - 8, sizeof(SignalContext) + 8)) {
        // No way back from the handler: treat it as fatal rather than jumping
        // into a handler that can never return.
        scheduler::exit_current(128 + signo);
        return;
    }

    auto* context = reinterpret_cast<SignalContext*>(sp);
    context->frame = *frame;

    sp -= 8;
    *reinterpret_cast<u64*>(sp) = restorer;   // where the handler's RET goes

    frame->user_rsp = sp;
    frame->user_rip = handler;
    frame->rdi      = static_cast<u64>(signo);   // handler(int signo)
    // A handler runs with a clean flags word; the saved copy is what gets
    // restored, so nothing is lost.
    frame->user_flags = kUserFlags;
}

// Called on the way out of every syscall. Takes at most one pending signal per
// return, which is enough: if more are pending the next syscall (or the
// restorer's own sigreturn) picks up the next one.
void handle_pending_signals(Frame* frame)
{
    if (!scheduler::signal_pending())
        return;

    const int signo = scheduler::signal_take_pending();
    if (signo == 0)
        return;

    const u64 handler = scheduler::signal_handler(signo);
    if (handler == signals::kSigIgnore)
        return;
    if (handler == signals::kSigDefault) {
        if (signals::default_kills(signo))
            scheduler::exit_current(128 + signo);
        return;                          // the rest default to being ignored
    }
    // SIGKILL is never catchable, whatever the process asked for.
    if (signo == signals::kSigKill) {
        scheduler::exit_current(128 + signo);
        return;
    }
    deliver_signal(frame, signo, handler);
}

// Restore the context a signal handler interrupted. The user stack pointer is
// sitting at the saved context, because the handler's RET popped the restorer
// address that deliver_signal pushed below it.
i64 sys_sigreturn(Frame* frame)
{
    const u64 sp = frame->user_rsp;
    if (!user_range_ok(sp, sizeof(SignalContext)))
        return -1;
    const auto* context = reinterpret_cast<const SignalContext*>(sp);

    // Everything the interrupted code had, including the rax the original
    // syscall was about to return.
    *frame = context->frame;
    return static_cast<i64>(frame->rax);
}

// Start a thread in the calling process: same address space, same open files,
// its own stack and register state. The caller supplies the stack (its libc
// mmaps one), which keeps thread stacks out of the kernel's bookkeeping.
// Returns the new thread's tid, or -1.
i64 sys_clone(u64 entry, u64 arg, u64 stack_top)
{
    if (entry == 0 || stack_top == 0)
        return -1;
    if (!user_range_ok(entry, 1) || !user_range_ok(stack_top, 1))
        return -1;

    scheduler::TrapFrame tf{};
    tf.rip    = entry;
    tf.rdi    = arg;                    // the SysV first argument
    tf.rsp    = stack_top & ~0xFull;    // the ABI wants a 16-aligned stack
    tf.cs     = kUserCode;
    tf.ss     = kUserData;
    tf.rflags = kUserFlags;

    const u32 tid = scheduler::spawn_thread(tf);
    return tid == 0 ? -1 : static_cast<i64>(tid);
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
        // The process id, shared by every thread of the process; gettid gives
        // the caller's own thread id.
        frame->rax = scheduler::current_tgid();
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

    case Mmap:
        frame->rax = static_cast<u64>(
            sys_mmap(frame->rdi, frame->rsi, frame->rdx, frame->r10));
        break;

    case Munmap:
        frame->rax = static_cast<u64>(sys_munmap(frame->rdi, frame->rsi));
        break;

    case Clone:
        frame->rax = static_cast<u64>(
            sys_clone(frame->rdi, frame->rsi, frame->rdx));
        break;

    case Gettid:
        frame->rax = scheduler::current_tid();
        break;

    case Getuid:
        frame->rax = scheduler::current_uid();
        break;

    case Setuid:
        frame->rax = static_cast<u64>(
            scheduler::set_current_uid(static_cast<u32>(frame->rdi)) ? 0 : -1);
        break;

    case Getgid:
        frame->rax = scheduler::current_gid();
        break;

    case Setgid:
        frame->rax = static_cast<u64>(
            scheduler::set_current_gid(static_cast<u32>(frame->rdi)) ? 0 : -1);
        break;

    case Chmod:
        frame->rax = static_cast<u64>(
            files::chmod(reinterpret_cast<const char*>(frame->rdi),
                         static_cast<u16>(frame->rsi)));
        break;

    case Chown:
        frame->rax = static_cast<u64>(
            files::chown(reinterpret_cast<const char*>(frame->rdi),
                         static_cast<u32>(frame->rsi),
                         static_cast<u32>(frame->rdx)));
        break;

    case Kill:
        frame->rax = static_cast<u64>(
            scheduler::signal_send(static_cast<u32>(frame->rdi),
                                   static_cast<int>(frame->rsi)) ? 0 : -1);
        break;

    case Signal: {
        // signal(signo, handler, restorer): the restorer is libc's trampoline,
        // registered here so the kernel has a return path out of a handler
        // without putting code on the (non-executable) user stack.
        const int signo = static_cast<int>(frame->rdi);
        const u64 previous = scheduler::signal_handler(signo);
        scheduler::signal_set_handler(signo, frame->rsi);
        if (frame->rdx != 0)
            scheduler::signal_set_restorer(frame->rdx);
        frame->rax = previous;
        break;
    }

    case Sigreturn:
        // Restores the interrupted context wholesale, including rax - so unlike
        // every other call, this one must not overwrite frame->rax afterwards.
        sys_sigreturn(frame);
        return;

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

    case Resolve: {
        if (!net::available() || !user_range_ok(frame->rdi, 1) ||
            !user_range_ok(frame->rsi, sizeof(u32))) {
            frame->rax = static_cast<u64>(-1);
            break;
        }
        // Copy the hostname into the kernel, bounded, so a missing terminator
        // cannot walk off the end of the user mapping.
        char host[256];
        const char* src = reinterpret_cast<const char*>(frame->rdi);
        usize n = 0;
        while (n < sizeof(host) - 1 && src[n] != '\0')
            ++n;
        memcpy(host, src, n);
        host[n] = '\0';

        u32 ip = 0;
        const bool ok = net::resolve(host, &ip);
        if (ok)
            *reinterpret_cast<u32*>(frame->rsi) = ip;
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
        // A new image knows nothing of the old one's handlers, and their
        // addresses no longer mean anything, so dispositions go back to default.
        if (frame->rax != static_cast<u64>(-1))
            scheduler::signal_reset_all();
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

    // The last thing before returning to ring 3: this is where the kernel holds
    // the full user register state, so it is the only place a handler can be
    // spliced in front of the interrupted code.
    handle_pending_signals(frame);
}
