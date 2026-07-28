#pragma once

#include <leah/cpu.hpp>
#include <leah/types.hpp>

// Mutual exclusion between processors.
//
// Everything the kernel did before SMP relied on "interrupts off" being enough:
// on one CPU, nothing else can run while they are masked. That stops being true
// the moment a second core executes kernel code, and a spinlock is the smallest
// thing that restores it.

// Forward-declared rather than included: a waiting CPU has to stay able to
// answer a TLB shootdown, and <leah/vmm.hpp> cannot be pulled into a header
// this low down without a cycle.
namespace vmm { void ack_shootdown(); }

namespace sync {

// Test-and-set rather than a ticket lock, deliberately. A ticket lock is fairer,
// but a waiter that gets descheduled still holds its ticket and stalls everyone
// queued behind it - and the kernel lock is taken inside syscalls that can be
// preempted. Test-and-set lets a descheduled waiter simply stop competing.
// Starvation is possible in theory; with a handful of CPUs and a coarse lock it
// has not been worth the trade.
class Spinlock {
public:
    bool try_acquire()
    {
        return __atomic_exchange_n(&m_locked, 1u, __ATOMIC_ACQUIRE) == 0;
    }

    void acquire()
    {
        while (!try_acquire()) {
            // Waiting is not an excuse to stop answering: the CPU that holds
            // this lock may be waiting on a shootdown from us, and we may be
            // spinning with interrupts masked and unable to take the IPI.
            vmm::ack_shootdown();
            asm volatile("pause");
        }
    }

    void release() { __atomic_store_n(&m_locked, 0u, __ATOMIC_RELEASE); }

    bool held() const { return __atomic_load_n(&m_locked, __ATOMIC_RELAXED) != 0; }

private:
    volatile u32 m_locked = 0;
};

// RAII: take on construction, drop on scope exit.
class ScopedLock {
public:
    explicit ScopedLock(Spinlock& lock) : m_lock(lock) { m_lock.acquire(); }
    ~ScopedLock() { m_lock.release(); }
    ScopedLock(const ScopedLock&) = delete;
    ScopedLock& operator=(const ScopedLock&) = delete;

private:
    Spinlock& m_lock;
};

// RAII, and masks interrupts for as long as the lock is held.
//
// A plain ScopedLock is not enough for a lock whose critical section is long or
// which is taken from interrupt context. If the holder is preempted part-way
// through, anyone spinning for that lock with interrupts already masked - a
// syscall, or a task on its way out through exit - can never be preempted back,
// and on a single processor the two wedge it permanently. Masking while held
// makes the section atomic with respect to this CPU, so the holder always runs
// to the release.
class IrqScopedLock {
public:
    explicit IrqScopedLock(Spinlock& lock) : m_lock(lock) { m_lock.acquire(); }
    ~IrqScopedLock() { m_lock.release(); }
    IrqScopedLock(const IrqScopedLock&) = delete;
    IrqScopedLock& operator=(const IrqScopedLock&) = delete;

private:
    // Declared first so it is constructed first (interrupts off before the
    // lock is taken) and destroyed last (restored after the release).
    cpu::InterruptGuard m_guard;
    Spinlock& m_lock;
};

// --- the big kernel lock ----------------------------------------------------
//
// One lock around every entry into the kernel: syscalls and interrupts. Coarse
// and slow, and deliberately so - it is the version that is obviously correct,
// and finer-grained locks can be split out from under it later without changing
// what callers may assume.
//
// It is recursive per CPU, because a syscall that enables interrupts (the
// network waits do, so the timer can wake a hlt) can take an interrupt while
// already holding it. A context switch only ever happens at depth 1: the only
// code that enables interrupts inside a syscall also disables preemption, so the
// nested handler cannot schedule.
namespace bkl {

void acquire();
void release();

// Depth on the current CPU. The scheduler saves and restores this across a
// context switch, since it belongs to the task rather than the processor.
u32  depth();
void set_depth(u32 value);

// Drop the lock entirely regardless of depth, returning what it was; used by
// the idle loop, which must not hold the kernel while it halts.
u32  release_all();
void reacquire(u32 previous_depth);

// Hand the lock to the task being switched to.
//
// Depth belongs to the task, not the processor. A task suspended inside a
// syscall was holding the lock and must get it back; a task being entered for
// the very first time never took it and must not inherit it, or it would run to
// user mode and never release, wedging every other CPU. This is called from the
// context switch, between saving the outgoing depth and switching stacks.
void handoff(u32 target_depth);

} // namespace bkl

} // namespace sync
