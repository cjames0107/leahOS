#include <leah/percpu.hpp>
#include <leah/vmm.hpp>
#include <leah/spinlock.hpp>

namespace sync {
namespace bkl {
namespace {

Spinlock g_lock;

// Which CPU holds it, and how deep. kNoOwner keeps "unheld" distinct from
// "held by slot 0".
constexpr u32 kNoOwner = 0xFFFFFFFF;
volatile u32 g_owner = kNoOwner;
// Recursion depth is per processor, not per machine: with the lock handed off
// at a context switch, a CPU can be running kernel code at depth 0 while
// another owns the lock at depth 1. A single global had the two decrementing
// each other's count.
u32& depth_of_this_cpu() { return percpu::current().lock_depth; }

} // namespace

void acquire()
{
    const u32 self = percpu::active();
    if (__atomic_load_n(&g_owner, __ATOMIC_ACQUIRE) == self) {
        depth_of_this_cpu() = depth_of_this_cpu() + 1;   // already ours: a nested entry
        return;
    }
    u64 flags;
    asm volatile("pushfq; pop %0" : "=r"(flags));
    while (!g_lock.try_acquire()) {
        // The CPU holding this lock may be waiting on us to acknowledge a TLB
        // shootdown before it can finish and release. Polling for it here is
        // what stops the two deadlocking; the IPI alone would not do, since a
        // syscall waits with interrupts masked.
        vmm::ack_shootdown();

        // And wait with interrupts *on*, whatever the caller arrived with.
        //
        // Nothing is held yet, so masking them here protects nothing - and
        // every device interrupt in the machine is routed to one processor. A
        // syscall enters with IF clear (SYSCALL masks it through FMASK), so
        // waiting the way the caller arrived means that if the bootstrap
        // processor is the one waiting, the keyboard and the mouse stop dead
        // for the duration. They did: the desktop came up and then ignored
        // every key and every mouse packet.
        //
        // This is safe precisely because the lock is not held yet. Every
        // check-then-block sequence in the kernel runs from inside a syscall,
        // which means the lock is already held by this CPU and the acquire it
        // does is a nested one that never reaches this loop - so no wake can
        // slip in between such a check and its block.
        asm volatile("sti; pause; cli");
    }
    // Leave interrupts as the caller had them.
    if ((flags & (1ull << 9)) != 0)
        asm volatile("sti");

    __atomic_store_n(&g_owner, self, __ATOMIC_RELEASE);
    depth_of_this_cpu() = 1;
}

void release()
{
    if (depth_of_this_cpu() == 0)
        return;
    depth_of_this_cpu() = depth_of_this_cpu() - 1;
    if (depth_of_this_cpu() == 0) {
        __atomic_store_n(&g_owner, kNoOwner, __ATOMIC_RELEASE);
        g_lock.release();
    }
}

u32 depth() { return depth_of_this_cpu(); }

void set_depth(u32 value) { depth_of_this_cpu() = value; }

u32 release_all()
{
    const u32 previous = depth_of_this_cpu();
    if (previous > 0) {
        depth_of_this_cpu() = 0;
        __atomic_store_n(&g_owner, kNoOwner, __ATOMIC_RELEASE);
        g_lock.release();
    }
    return previous;
}

void handoff(u32 target_depth)
{
    const u32 self = percpu::active();
    const bool holding = __atomic_load_n(&g_owner, __ATOMIC_ACQUIRE) == self;

    if (target_depth == 0) {
        if (holding) {
            depth_of_this_cpu() = 0;
            __atomic_store_n(&g_owner, kNoOwner, __ATOMIC_RELEASE);
            g_lock.release();
        }
        return;
    }

    if (!holding) {
        g_lock.acquire();
        __atomic_store_n(&g_owner, self, __ATOMIC_RELEASE);
    }
    depth_of_this_cpu() = target_depth;
}

void reacquire(u32 previous_depth)
{
    if (previous_depth == 0)
        return;
    const u32 self = percpu::active();
    g_lock.acquire();
    __atomic_store_n(&g_owner, self, __ATOMIC_RELEASE);
    depth_of_this_cpu() = previous_depth;
}

} // namespace bkl
} // namespace sync
