#include <leah/percpu.hpp>
#include <leah/spinlock.hpp>

namespace sync {
namespace bkl {
namespace {

Spinlock g_lock;

// Which CPU holds it, and how deep. kNoOwner keeps "unheld" distinct from
// "held by slot 0".
constexpr u32 kNoOwner = 0xFFFFFFFF;
volatile u32 g_owner = kNoOwner;
volatile u32 g_depth = 0;

} // namespace

void acquire()
{
    const u32 self = percpu::slot();
    if (__atomic_load_n(&g_owner, __ATOMIC_ACQUIRE) == self) {
        g_depth = g_depth + 1;      // already ours; this is a nested entry
        return;
    }
    // Spin with interrupts briefly open. Another CPU may be waiting on this one
    // to acknowledge a TLB shootdown before it can free a page, and it cannot
    // make progress - nor release this lock - until we let that interrupt in.
    // Nothing is held while waiting, so servicing one here is safe.
    u64 flags;
    asm volatile("pushfq; pop %0" : "=r"(flags));
    while (!g_lock.try_acquire()) {
        if ((flags & (1ull << 9)) != 0)         // only if the caller had them on
            asm volatile("sti; pause; cli");
        else
            asm volatile("pause");
    }
    if ((flags & (1ull << 9)) != 0)
        asm volatile("sti");

    __atomic_store_n(&g_owner, self, __ATOMIC_RELEASE);
    g_depth = 1;
    // Everything under the lock can now ask which CPU it is on without paying
    // for an APIC read.
    percpu::set_active(self);
}

void release()
{
    if (g_depth == 0)
        return;
    g_depth = g_depth - 1;
    if (g_depth == 0) {
        __atomic_store_n(&g_owner, kNoOwner, __ATOMIC_RELEASE);
        g_lock.release();
    }
}

u32 depth() { return g_depth; }

void set_depth(u32 value) { g_depth = value; }

u32 release_all()
{
    const u32 previous = g_depth;
    if (previous > 0) {
        g_depth = 0;
        __atomic_store_n(&g_owner, kNoOwner, __ATOMIC_RELEASE);
        g_lock.release();
    }
    return previous;
}

void handoff(u32 target_depth)
{
    const u32 self = percpu::slot();
    const bool holding = __atomic_load_n(&g_owner, __ATOMIC_ACQUIRE) == self;

    if (target_depth == 0) {
        if (holding) {
            g_depth = 0;
            __atomic_store_n(&g_owner, kNoOwner, __ATOMIC_RELEASE);
            g_lock.release();
        }
        return;
    }

    if (!holding) {
        g_lock.acquire();
        __atomic_store_n(&g_owner, self, __ATOMIC_RELEASE);
        percpu::set_active(self);
    }
    g_depth = target_depth;
}

void reacquire(u32 previous_depth)
{
    if (previous_depth == 0)
        return;
    const u32 self = percpu::slot();
    g_lock.acquire();
    __atomic_store_n(&g_owner, self, __ATOMIC_RELEASE);
    g_depth = previous_depth;
    percpu::set_active(self);
}

} // namespace bkl
} // namespace sync
