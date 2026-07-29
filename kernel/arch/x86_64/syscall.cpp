#include <leah/accounts.hpp>
#include <leah/console.hpp>
#include <leah/cpu.hpp>
#include <leah/file.hpp>
#include <leah/gdt.hpp>
#include <leah/interrupts.hpp>
#include <leah/memory.hpp>
#include <leah/net.hpp>
#include <leah/pmm.hpp>
#include <leah/process.hpp>
#include <leah/shm.hpp>
#include <leah/framebuffer.hpp>
#include <leah/mouse.hpp>
#include <leah/percpu.hpp>
#include <leah/keyboard.hpp>
#include <leah/scheduler.hpp>
#include <leah/timer.hpp>
#include <leah/signal.hpp>
#include <leah/spinlock.hpp>
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

// Copy a NUL-terminated string out of user memory, bounded. Returns false when
// the pointer is not in the user half; a missing terminator truncates rather
// than running off the end of the mapping.
bool copy_user_string(u64 user_pointer, char* out, usize out_size)
{
    if (user_pointer == 0 || !user_range_ok(user_pointer, 1))
        return false;
    const char* in = reinterpret_cast<const char*>(user_pointer);
    usize n = 0;
    while (n + 1 < out_size && in[n] != '\0') {
        out[n] = in[n];
        ++n;
    }
    out[n] = '\0';
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
        // Release, not free. A frame under this mapping may be shared - with a
        // forked sibling, or through a shared-memory segment - and handing it
        // straight back to the allocator while someone else still maps it is
        // exactly how one process corrupts another.
        pmm::release(frame);
    }
    return 0;
}

// The one kernel primitive userland locks need: sleep until someone says the
// word. Everything else - mutexes, condition variables, semaphores - is built
// on top of it in libc, which is the point: an uncontended lock never enters
// the kernel at all, and only a thread that actually has to wait pays for a
// syscall.
//
// The address itself is the wait channel. Threads share an address space, so a
// virtual address identifies the same word for everyone who can contend on it.
// (Cross-process futexes over shared memory would need the physical address;
// there is no shared memory yet, so this is exact rather than merely close.)
i64 sys_futex(u64 uaddr, u64 op, u64 val)
{
    if ((uaddr & 3) != 0 || !user_range_ok(uaddr, sizeof(u32)))
        return -1;                          // must be an aligned user word

    switch (op) {
    case kFutexWait: {
        // Re-check under the syscall's masked interrupts: if the value has
        // already changed, the wakeup we would have waited for has happened,
        // and blocking now would miss it forever.
        if (*reinterpret_cast<volatile u32*>(uaddr) != static_cast<u32>(val))
            return -1;
        scheduler::block_on(uaddr);
        return 0;
    }
    case kFutexWake:
        return static_cast<i64>(
            scheduler::wake_n(uaddr, val == 0 ? 1u : static_cast<u32>(val)));
    default:
        return -1;
    }
}

// The context a handler's frame leaves on the user stack. A signal can arrive
// at a syscall or at a hardware IRQ, so the saved form is the neutral full
// register set rather than either entry path's own frame - and restoring it
// goes out through IRETQ, which can put back RCX and R11 that SYSRET would
// destroy.
using SavedContext = scheduler::TrapFrame;

extern "C" [[noreturn]] void sigreturn_to_user(const SavedContext* frame);

// Build a handler frame on the user's own stack: the saved context, then a
// return address pointing at libc's restorer, which calls sigreturn to undo all
// of this. Reports where the handler should start and on what stack.
bool push_signal_frame(const SavedContext& saved, u64& new_rsp)
{
    u64 sp = saved.rsp;
    sp -= 128;                          // skip the SysV red zone
    sp -= sizeof(SavedContext);
    sp &= ~0xFull;                      // the ABI's 16-byte alignment

    const u64 restorer = scheduler::signal_restorer();
    if (restorer == 0 || !user_range_ok(sp - 8, sizeof(SavedContext) + 8))
        return false;

    *reinterpret_cast<SavedContext*>(sp) = saved;
    sp -= 8;
    *reinterpret_cast<u64*>(sp) = restorer;   // where the handler's RET goes

    new_rsp = sp;
    return true;
}

// Called on the way out of every syscall. Takes at most one pending signal per
// return, which is enough: if more are pending the next return picks up the
// next one.
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
        return;
    }
    if (signo == signals::kSigKill)
        scheduler::exit_current(128 + signo);

    // RCX and R11 are call-clobbered across a syscall by the ABI, so the
    // interrupted code cannot depend on them; everything else is exact.
    SavedContext saved{};
    saved.r15 = frame->r15; saved.r14 = frame->r14; saved.r13 = frame->r13;
    saved.r12 = frame->r12; saved.r11 = frame->user_flags;
    saved.r10 = frame->r10; saved.r9  = frame->r9;  saved.r8  = frame->r8;
    saved.rbp = frame->rbp; saved.rdi = frame->rdi; saved.rsi = frame->rsi;
    saved.rdx = frame->rdx; saved.rcx = frame->user_rip;
    saved.rbx = frame->rbx; saved.rax = frame->rax;
    saved.rip = frame->user_rip;
    saved.cs  = kUserCode;
    saved.rflags = frame->user_flags;
    saved.rsp = frame->user_rsp;
    saved.ss  = kUserData;

    u64 new_rsp = 0;
    if (!push_signal_frame(saved, new_rsp)) {
        // No way back from the handler: fatal rather than jumping into a
        // handler that can never return.
        scheduler::exit_current(128 + signo);
        return;
    }

    frame->user_rsp   = new_rsp;
    frame->user_rip   = handler;
    frame->rdi        = static_cast<u64>(signo);   // handler(int signo)
    frame->user_flags = kUserFlags;
}

// Restore the context a signal handler interrupted, and go straight back to
// ring 3 through IRETQ rather than the syscall's SYSRET - the saved context may
// have come from a hardware interrupt, where RCX and R11 hold live values.
//
// The saved frame sits in the user's own memory, so every field it controls has
// to be sanitised: the segment selectors are forced back to ring 3 (or a
// process could ask to be resumed in ring 0), and RFLAGS is masked down to the
// arithmetic bits plus a set IF, dropping IOPL and NT.
[[noreturn]] void sys_sigreturn(Frame* frame)
{
    const u64 sp = frame->user_rsp;
    if (!user_range_ok(sp, sizeof(SavedContext)))
        scheduler::exit_current(128 + signals::kSigSegv);

    SavedContext restored = *reinterpret_cast<const SavedContext*>(sp);

    if (!user_range_ok(restored.rip, 1) || !user_range_ok(restored.rsp, 1))
        scheduler::exit_current(128 + signals::kSigSegv);

    restored.cs = kUserCode;
    restored.ss = kUserData;
    constexpr u64 kSafeFlags = 0x8D5;    // CF PF AF ZF SF DF OF
    restored.rflags = (restored.rflags & kSafeFlags) | kUserFlags;

    // This path leaves through IRETQ rather than returning, so the dispatcher's
    // scope guard will never fire: drop the kernel lock by hand first.
    sync::bkl::release();
    sigreturn_to_user(&restored);
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

// Deliver a pending signal to a task that a hardware interrupt caught in ring 3.
// This is what lets a signal reach a process that is not making syscalls at all
// - a compute-bound loop is interrupted by the timer, and the handler is spliced
// in on the way back out. The interrupted context is saved in full, because
// unlike a syscall an IRQ leaves every register live.
void deliver_on_interrupt(interrupts::Frame& frame)
{
    if ((frame.cs & 3) != 3)
        return;                          // interrupted the kernel, not userland
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
        return;
    }
    if (signo == signals::kSigKill)
        scheduler::exit_current(128 + signo);

    SavedContext saved{};
    saved.r15 = frame.r15; saved.r14 = frame.r14; saved.r13 = frame.r13;
    saved.r12 = frame.r12; saved.r11 = frame.r11; saved.r10 = frame.r10;
    saved.r9  = frame.r9;  saved.r8  = frame.r8;  saved.rbp = frame.rbp;
    saved.rdi = frame.rdi; saved.rsi = frame.rsi; saved.rdx = frame.rdx;
    saved.rcx = frame.rcx; saved.rbx = frame.rbx; saved.rax = frame.rax;
    saved.rip = frame.rip;
    saved.cs  = kUserCode;
    saved.rflags = frame.rflags;
    saved.rsp = frame.rsp;
    saved.ss  = kUserData;

    u64 new_rsp = 0;
    if (!push_signal_frame(saved, new_rsp)) {
        scheduler::exit_current(128 + signo);
        return;
    }

    // The IRETQ at the end of the ISR now lands in the handler instead.
    frame.rsp    = new_rsp;
    frame.rip    = handler;
    frame.rdi    = static_cast<u64>(signo);
    frame.rflags = kUserFlags;
}

// SYSCALL is configured entirely through MSRs, and an MSR belongs to one
// processor. Miss this on an application processor and the first `syscall` a
// task executes there raises #UD - EFER.SCE is what makes the opcode legal at
// all - which looks like a corrupt user program rather than a missing bit.
void init_this_cpu()
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

void init() { init_this_cpu(); }

} // namespace syscall

extern "C" void syscall_dispatch(syscall::Frame* frame)
{
    // One lock around the whole kernel. Released on every path out, including
    // the one sigreturn takes, which does not return here.
    sync::bkl::acquire();
    struct Unlock { ~Unlock() { sync::bkl::release(); } } unlock;

    using namespace syscall;

    switch (frame->rax) {
    case Exit:
        // The whole process: a program returning from main ends its threads too.
        scheduler::exit_group(static_cast<i32>(frame->rdi));     // never returns

    case ProcList: {
        // The count is capped by the caller's buffer, which is validated
        // against the whole array rather than per entry.
        const u32 max = static_cast<u32>(frame->rsi);
        const u64 bytes = static_cast<u64>(max) * sizeof(scheduler::TaskInfo);
        if (max == 0 || max > 128 || !user_range_ok(frame->rdi, bytes)) {
            frame->rax = static_cast<u64>(-1);
            break;
        }
        frame->rax = scheduler::snapshot(
            reinterpret_cast<scheduler::TaskInfo*>(frame->rdi), max);
        break;
    }

    case MemInfo: {
        if (!user_range_ok(frame->rdi, sizeof(u64) * 3)) {
            frame->rax = static_cast<u64>(-1);
            break;
        }
        auto* out = reinterpret_cast<u64*>(frame->rdi);
        out[0] = pmm::usable_bytes();
        out[1] = pmm::used_bytes();
        out[2] = pmm::free_bytes();
        frame->rax = 0;
        break;
    }

    case ThreadExit:
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

    case Futex:
        frame->rax = static_cast<u64>(
            sys_futex(frame->rdi, frame->rsi, frame->rdx));
        break;

    case Connect:
        frame->rax = static_cast<u64>(
            files::tcp_connect(static_cast<u32>(frame->rdi),
                               static_cast<u16>(frame->rsi)));
        break;

    case Login: {
        // The password is copied in and the account file is read here, so the
        // hash never enters a user process - which is what lets su work without
        // being setuid and without a world-readable shadow file.
        if (!user_range_ok(frame->rdi, 1)) {
            frame->rax = static_cast<u64>(-1);
            break;
        }
        char user[accounts::kMaxName] = {};
        char password[128] = {};
        char home[accounts::kMaxHome] = {};

        const char* user_in = reinterpret_cast<const char*>(frame->rdi);
        usize n = 0;
        while (n < sizeof(user) - 1 && user_in[n] != '\0') { user[n] = user_in[n]; ++n; }

        if (frame->rsi != 0 && user_range_ok(frame->rsi, 1)) {
            const char* pw_in = reinterpret_cast<const char*>(frame->rsi);
            usize m = 0;
            while (m < sizeof(password) - 1 && pw_in[m] != '\0') {
                password[m] = pw_in[m];
                ++m;
            }
        }

        const bool ok = accounts::login(user, frame->rsi != 0 ? password : nullptr,
                                        home, sizeof(home));
        if (ok && frame->rdx != 0 && user_range_ok(frame->rdx, sizeof(home)))
            memcpy(reinterpret_cast<void*>(frame->rdx), home, sizeof(home));
        // Wipe the copy rather than leave a password lying in kernel memory.
        memset(password, 0, sizeof(password));
        frame->rax = static_cast<u64>(ok ? 0 : -1);
        break;
    }

    case UserAdd: {
        // Strings are copied in before use, as everywhere: a user pointer that
        // changes under the kernel is a bug waiting to happen.
        char name[accounts::kMaxName] = {};
        char password[128] = {};
        char home[accounts::kMaxHome] = {};
        if (!copy_user_string(frame->rdi, name, sizeof(name)) ||
            !copy_user_string(frame->rsi, password, sizeof(password)) ||
            !copy_user_string(frame->r8, home, sizeof(home))) {
            frame->rax = static_cast<u64>(-1);
            break;
        }
        const bool ok = accounts::create(name, password,
                                         static_cast<u32>(frame->rdx),
                                         static_cast<u32>(frame->r10), home);
        memset(password, 0, sizeof(password));
        frame->rax = static_cast<u64>(ok ? 0 : -1);
        break;
    }

    case SetPasswd: {
        char name[accounts::kMaxName] = {};
        char old_password[128] = {};
        char new_password[128] = {};
        if (!copy_user_string(frame->rdi, name, sizeof(name)) ||
            !copy_user_string(frame->rdx, new_password, sizeof(new_password))) {
            frame->rax = static_cast<u64>(-1);
            break;
        }
        const bool have_old = frame->rsi != 0 &&
                              copy_user_string(frame->rsi, old_password,
                                               sizeof(old_password));
        const bool ok = accounts::set_password(name,
                                               have_old ? old_password : nullptr,
                                               new_password);
        memset(old_password, 0, sizeof(old_password));
        memset(new_password, 0, sizeof(new_password));
        frame->rax = static_cast<u64>(ok ? 0 : -1);
        break;
    }

    case ShmOpen:
        // Create or open a segment by key. The key is the rendezvous: two
        // processes that agree on a number find the same memory, which is all
        // a window server and its clients actually need from a namespace.
        frame->rax = static_cast<u64>(
            shm::open(static_cast<u32>(frame->rdi), frame->rsi,
                      scheduler::current_uid(), static_cast<u32>(frame->rdx)));
        break;

    case ShmSize:
        frame->rax = shm::size_of(static_cast<i32>(frame->rdi));
        break;

    case ShmDestroy:
        // Drops the segment's own reference. Anyone who still has it mapped
        // keeps it alive through theirs, so this is safe to call while the
        // other side is still using it - the frames go when the last mapping
        // does. Without it a key could never be reused, and reusing one would
        // hand back a segment of the wrong size.
        frame->rax = static_cast<u64>(
            shm::destroy(static_cast<i32>(frame->rdi),
                         scheduler::current_uid()) ? 0 : -1);
        break;

    case ShmMap: {
        const i32 id = static_cast<i32>(frame->rdi);
        if (!shm::exists(id)) {
            frame->rax = static_cast<u64>(-1);
            break;
        }
        if (!shm::accessible(id, scheduler::current_uid())) {
            frame->rax = static_cast<u64>(-1);
            break;
        }

        const usize pages = shm::page_count(id);
        const u64 bytes = static_cast<u64>(pages) * vmm::kPageSize;
        const u64 base = scheduler::current_mmap_next();
        if (pages == 0 || base + bytes > memory::kUserMmapEnd) {
            frame->rax = static_cast<u64>(-1);
            break;
        }

        // One reference per frame for this mapping. Address-space teardown
        // drops exactly one per mapped page, so the segment survives a client
        // exiting - and the frames survive until the last mapping and the
        // segment itself have both let go.
        if (!shm::share_frames(id)) {
            frame->rax = static_cast<u64>(-1);
            break;
        }

        // Shared, not merely writable: a fork must hand these pages to the
        // child as they are. Without the mark they would be made
        // copy-on-write like anything else, and a client that forked after
        // opening a window would carry on drawing into a private copy while
        // the server composited the pages it had stopped writing to.
        bool ok = true;
        for (usize i = 0; i < pages && ok; ++i) {
            ok = vmm::map(base + i * vmm::kPageSize, shm::frame_of(id, i),
                          vmm::Write | vmm::User | vmm::NoExecute | vmm::Shared);
        }
        if (!ok) {
            frame->rax = static_cast<u64>(-1);
            break;
        }
        scheduler::set_current_mmap_next(base + bytes);
        frame->rax = base;
        break;
    }

    case FbInfo: {
        // width, height, pitch in bytes, bits per pixel.
        if (!user_range_ok(frame->rdi, sizeof(u32) * 4) ||
            !framebuffer::available()) {
            frame->rax = static_cast<u64>(-1);
            break;
        }
        auto* out = reinterpret_cast<u32*>(frame->rdi);
        out[0] = framebuffer::width();
        out[1] = framebuffer::height();
        out[2] = framebuffer::pitch();
        out[3] = framebuffer::bits_per_pixel();
        frame->rax = 0;
        break;
    }

    case FbMap: {
        // Handing the screen to a process is not a small thing, so it is root's
        // to ask for. Whoever holds this mapping can draw anywhere, including
        // over whatever another user is looking at.
        if (scheduler::current_uid() != 0 || !framebuffer::available()) {
            frame->rax = static_cast<u64>(-1);
            break;
        }
        const u64 bytes = static_cast<u64>(framebuffer::pitch()) *
                          framebuffer::height();
        const paddr_t phys = framebuffer::physical_base();
        const u64 base = scheduler::current_mmap_next();
        if (phys == 0 || base + bytes > memory::kUserMmapEnd) {
            frame->rax = static_cast<u64>(-1);
            break;
        }
        // The framebuffer is device memory, shared by definition: copying it
        // on write would give a forked child a private screen.
        bool ok = true;
        for (u64 offset = 0; offset < bytes && ok; offset += vmm::kPageSize) {
            ok = vmm::map(base + offset, phys + offset,
                          vmm::Write | vmm::User | vmm::NoExecute | vmm::Shared);
        }
        if (ok) {
            scheduler::set_current_mmap_next(base + bytes);
            // The console stops painting: two things drawing to one screen just
            // corrupt each other. It comes back when this process exits.
            console::grant_display_to(scheduler::current_tgid());
        }
        frame->rax = ok ? base : static_cast<u64>(-1);
        break;
    }

    case Sleep: {
        // Milliseconds in, rounded up to whole ticks - sleeping for less than a
        // tick would round to zero and busy-wait, which is the thing this
        // exists to avoid.
        const u64 ms = frame->rdi;
        const u64 per_ms = timer::kFrequencyHz / 1000;
        u64 ticks = per_ms != 0 ? ms * per_ms : (ms * timer::kFrequencyHz) / 1000;
        if (ticks == 0 && ms != 0)
            ticks = 1;
        scheduler::sleep_ticks(ticks);
        frame->rax = 0;
        break;
    }

    case FbFont: {
        // The 8x16 font stage 2 lifted out of the video BIOS, as 256 glyphs of
        // 16 rows. A server drawing its own text would otherwise have to carry
        // a second copy of a font the system already has.
        constexpr u64 kFontBytes = 256 * 16;
        if (!user_range_ok(frame->rdi, kFontBytes)) {
            frame->rax = static_cast<u64>(-1);
            break;
        }
        auto* out = reinterpret_cast<u8*>(frame->rdi);
        for (u32 glyph = 0; glyph < 256; ++glyph) {
            for (u32 row = 0; row < 16; ++row) {
                out[glyph * 16 + row] =
                    framebuffer::glyph_row(static_cast<char>(glyph), row);
            }
        }
        frame->rax = 0;
        break;
    }

    case InputPoll: {
        // Raw input, before the console cooks it: {mouse x, mouse y, buttons,
        // key}. The key is 0 when nothing is waiting, so a server can poll this
        // in its own loop without blocking on either device.
        if (scheduler::current_uid() != 0 ||
            !user_range_ok(frame->rdi, sizeof(i32) * 5)) {
            frame->rax = static_cast<u64>(-1);
            break;
        }
        const mouse::State state = mouse::state();
        auto* out = reinterpret_cast<i32*>(frame->rdi);
        out[0] = state.x;
        out[1] = state.y;
        out[2] = (state.left ? 1 : 0) | (state.right ? 2 : 0) |
                 (state.middle ? 4 : 0);
        out[3] = static_cast<i32>(static_cast<u8>(keyboard::read()));
        /* What is held *now*, which is what a click needs: a keystroke already
         * carries its modifiers in the character it produced. */
        out[4] = static_cast<i32>(keyboard::modifiers());
        frame->rax = 0;
        break;
    }

    case SetEcho:
        files::set_console_echo(frame->rdi != 0);
        frame->rax = 0;
        break;

    case UserName: {
        if (!user_range_ok(frame->rsi, 1)) {
            frame->rax = static_cast<u64>(-1);
            break;
        }
        char name[accounts::kMaxName] = {};
        const bool ok = accounts::lookup_uid(static_cast<u32>(frame->rdi), name,
                                             sizeof(name));
        if (ok)
            memcpy(reinterpret_cast<void*>(frame->rsi), name, sizeof(name));
        frame->rax = static_cast<u64>(ok ? 0 : -1);
        break;
    }

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
