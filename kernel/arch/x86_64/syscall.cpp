#include <leah/console.hpp>
#include <leah/cpu.hpp>
#include <leah/file.hpp>
#include <leah/gdt.hpp>
#include <leah/interrupts.hpp>
#include <leah/memory.hpp>
#include <leah/pmm.hpp>
#include <leah/process.hpp>
#include <leah/shm.hpp>
#include <leah/framebuffer.hpp>
#include <leah/mouse.hpp>
#include <leah/percpu.hpp>
#include <leah/keyboard.hpp>
#include <leah/ipc.hpp>
#include <leah/scheduler.hpp>
#include <leah/timer.hpp>
#include <leah/signal.hpp>
#include <leah/spinlock.hpp>
#include <leah/string.hpp>
#include <leah/syscall.hpp>
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
// A program the kernel will copy into its own memory before mapping it. The
// bound is not about correctness so much as about not letting a caller ask the
// kernel to kmalloc an arbitrary amount on its say-so.
constexpr u64 kMaxImageBytes = 32ull * 1024 * 1024;

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

    case CpuInfo: {
        const u32 max = static_cast<u32>(frame->rsi);
        const u64 bytes = static_cast<u64>(max) * sizeof(scheduler::CpuStat);
        if (max == 0 || max > 32 || !user_range_ok(frame->rdi, bytes)) {
            frame->rax = static_cast<u64>(-1);
            break;
        }
        frame->rax = scheduler::cpu_stats(
            reinterpret_cast<scheduler::CpuStat*>(frame->rdi), max);
        break;
    }

    /* --- message passing --------------------------------------------------
     *
     * The message is copied into the kernel while the sender is the current
     * address space and out of it while the receiver is. That is the whole
     * reason these are copies rather than a pointer handed across: the two
     * ends share no mapping, and the only moment either buffer is addressable
     * is while its own process is running.
     */
    case PortCreate:
        frame->rax = static_cast<u64>(ipc::port_create(static_cast<u32>(frame->rdi)));
        break;

    case PortOpen:
        frame->rax = static_cast<u64>(ipc::port_open(static_cast<u32>(frame->rdi)));
        break;

    case PortDestroy:
        frame->rax = static_cast<u64>(ipc::port_destroy(static_cast<i32>(frame->rdi)));
        break;

    case IpcCall: {
        if (!user_range_ok(frame->rsi, sizeof(ipc::Message)) ||
            !user_range_ok(frame->rdx, sizeof(ipc::Message))) {
            frame->rax = static_cast<u64>(-1);
            break;
        }
        frame->rax = static_cast<u64>(
            ipc::call(static_cast<i32>(frame->rdi),
                      reinterpret_cast<const ipc::Message*>(frame->rsi),
                      reinterpret_cast<ipc::Message*>(frame->rdx)));
        break;
    }

    case IpcRecv: {
        if (!user_range_ok(frame->rsi, sizeof(ipc::Message)) ||
            (frame->rdx != 0 && !user_range_ok(frame->rdx, sizeof(u32)))) {
            frame->rax = static_cast<u64>(-1);
            break;
        }
        frame->rax = static_cast<u64>(
            ipc::recv(static_cast<i32>(frame->rdi),
                      reinterpret_cast<ipc::Message*>(frame->rsi),
                      reinterpret_cast<u32*>(frame->rdx)));
        break;
    }

    case IpcReply: {
        if (!user_range_ok(frame->rsi, sizeof(ipc::Message))) {
            frame->rax = static_cast<u64>(-1);
            break;
        }
        frame->rax = static_cast<u64>(
            ipc::reply(static_cast<i32>(frame->rdi),
                       reinterpret_cast<const ipc::Message*>(frame->rsi)));
        break;
    }

    /* --- the driver ABI ---------------------------------------------------
     *
     * All four are root-only, which is a placeholder for a real capability:
     * what should grant these is whatever launches a driver knowing what it
     * is, not the uid it happens to run as. The narrowness is real, though -
     * each grant names exactly what it covers.
     */
    case IoPermit:
        frame->rax = static_cast<u64>(
            scheduler::grant_io_ports(static_cast<u16>(frame->rdi),
                                      static_cast<u32>(frame->rsi)));
        break;

    case MapPhysical: {
        // A device's registers, mapped where the driver can reach them.
        // Uncached and no-execute: these are not memory, and a stale cache
        // line where a status register should be is a bug that looks like
        // faulty hardware.
        if (scheduler::current_uid() != 0) {
            frame->rax = static_cast<u64>(-1);
            break;
        }
        const u64 phys  = frame->rdi & ~(vmm::kPageSize - 1);
        const u64 slack = frame->rdi - phys;
        const u64 bytes = (frame->rsi + slack + vmm::kPageSize - 1) &
                          ~(vmm::kPageSize - 1);
        const u64 base  = scheduler::current_mmap_next();
        if (bytes == 0 || bytes > (64u << 20) ||
            base + bytes > memory::kUserMmapEnd) {
            frame->rax = static_cast<u64>(-1);
            break;
        }
        bool ok = true;
        for (u64 off = 0; off < bytes && ok; off += vmm::kPageSize)
            ok = vmm::map(base + off, phys + off,
                          vmm::Write | vmm::User | vmm::NoExecute |
                          vmm::NoCache | vmm::WriteThrough | vmm::Shared);
        if (!ok) {
            frame->rax = static_cast<u64>(-1);
            break;
        }
        scheduler::set_current_mmap_next(base + bytes);
        frame->rax = base + slack;
        break;
    }

    case DmaAlloc: {
        // Contiguous physical pages, mapped and reported. A driver has to know
        // the physical address because that is what it writes into a
        // descriptor; there is no IOMMU here to translate on its behalf.
        if (scheduler::current_uid() != 0 || !user_range_ok(frame->rsi, sizeof(u64))) {
            frame->rax = static_cast<u64>(-1);
            break;
        }
        const u64 bytes = (frame->rdi + vmm::kPageSize - 1) &
                          ~(vmm::kPageSize - 1);
        const usize pages = static_cast<usize>(bytes / vmm::kPageSize);
        const u64 base = scheduler::current_mmap_next();
        if (pages == 0 || pages > 4096 || base + bytes > memory::kUserMmapEnd) {
            frame->rax = static_cast<u64>(-1);
            break;
        }
        const paddr_t phys = pmm::alloc_contiguous(pages);
        if (phys == 0) {
            frame->rax = static_cast<u64>(-1);
            break;
        }
        memset(reinterpret_cast<void*>(memory::phys_to_direct(phys)), 0, bytes);
        bool ok = true;
        for (u64 off = 0; off < bytes && ok; off += vmm::kPageSize)
            ok = vmm::map(base + off, phys + off,
                          vmm::Write | vmm::User | vmm::NoExecute | vmm::Shared);
        if (!ok) {
            pmm::free_contiguous(phys, pages);
            frame->rax = static_cast<u64>(-1);
            break;
        }
        *reinterpret_cast<u64*>(frame->rsi) = phys;
        scheduler::set_current_mmap_next(base + bytes);
        frame->rax = base;
        break;
    }

    case IrqListen:
        frame->rax = static_cast<u64>(
            interrupts::listen(static_cast<u8>(frame->rdi)));
        break;

    case IrqWait:
        frame->rax = static_cast<u64>(
            interrupts::wait_for(static_cast<u8>(frame->rdi)));
        break;

    case SetCreds:
        // The kernel's whole remaining share of logging in. Who is allowed to
        // ask, and on what evidence, is authd's business; making it true of a
        // running process is this.
        frame->rax = static_cast<u64>(
            scheduler::set_credentials_of(static_cast<u32>(frame->rdi),
                                          static_cast<u32>(frame->rsi),
                                          static_cast<u32>(frame->rdx))
                ? 0 : -1);
        break;

    case InputPost:
        // A keyboard driver in ring 3 handing over what it decoded. Root only,
        // and for the obvious reason: being able to put characters into the
        // input queue is being able to type into someone else's session.
        //
        // The kernel keeps the queue rather than the driver, because the queue
        // is what a blocked reader is asleep on and waking it is scheduling.
        if (scheduler::current_uid() != 0) {
            frame->rax = static_cast<u64>(-1);
            break;
        }
        if (frame->rdi == 0)
            keyboard::inject_char(static_cast<char>(frame->rsi));
        else
            keyboard::set_usb_modifiers(static_cast<u32>(frame->rsi));
        frame->rax = 0;
        break;

    case CredsOf:
        // A server cannot take the caller's word for who the caller is - a uid
        // inside a message is a uid the sender chose. This is the kernel
        // saying it instead, about the pid the kernel itself reported. The
        // gid comes back in the high half; a caller that only wants the uid
        // can truncate and be right.
        frame->rax = scheduler::credentials_of(static_cast<u32>(frame->rdi));
        break;

    case IpcTryRecv: {
        if (!user_range_ok(frame->rsi, sizeof(ipc::Message)) ||
            (frame->rdx != 0 && !user_range_ok(frame->rdx, sizeof(u32)))) {
            frame->rax = static_cast<u64>(-1);
            break;
        }
        frame->rax = static_cast<u64>(
            ipc::try_recv(static_cast<i32>(frame->rdi),
                          reinterpret_cast<ipc::Message*>(frame->rsi),
                          reinterpret_cast<u32*>(frame->rdx)));
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

    case Close:
        frame->rax = static_cast<u64>(files::close(static_cast<int>(frame->rdi)));
        break;

    case Pipe:
        frame->rax = static_cast<u64>(
            files::pipe(reinterpret_cast<int*>(frame->rdi)));
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

    case Fork:
        frame->rax = scheduler::fork_current(to_trap_frame(*frame));
        break;

    case Execve:
        // execve(image, bytes, argv). The caller read the program; the kernel
        // only maps it. On success this rewrites frame to enter the new image
        // and never "returns" to the caller; on failure it leaves rax = -1.
        if (frame->rsi == 0 || frame->rsi > kMaxImageBytes ||
            !user_range_ok(frame->rdi, frame->rsi)) {
            frame->rax = static_cast<u64>(-1);
            break;
        }
        process::exec(*frame, reinterpret_cast<const u8*>(frame->rdi),
                      static_cast<usize>(frame->rsi),
                      reinterpret_cast<char**>(frame->rdx));
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
