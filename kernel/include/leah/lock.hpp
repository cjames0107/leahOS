#pragma once

#include <leah/spinlock.hpp>
#include <leah/types.hpp>

// Locks that know what order they are meant to be taken in.
//
// The big kernel lock is being taken apart, and the failure mode of that work
// is not a crash. It is two processors each holding one lock and waiting for
// the other, on a machine with eight of them, once every few hundred boots -
// which is indistinguishable from the intermittent stalls this system already
// has, and would be blamed on them.
//
// So the instrument comes before the surgery. Every lock carries a rank, and a
// processor may only take one whose rank is strictly greater than every rank
// it already holds. A deadlock between two ranked locks is then impossible:
// there is no cycle in a strict ordering. Getting the order wrong stops being
// a rare hang and becomes a panic, on the first CPU that does it, naming both
// locks.
//
// The ranks below are the acquisition order, lowest first. They read as a
// dependency graph of the kernel: the scheduler is entered from everywhere and
// so is taken first; the console is called from inside everything and so is
// taken last; memory sits between, because a page table walk allocates frames
// and never the other way round.
//
// Handles come before images because that is the direction things are looked
// up in: a process presents a handle, the handle names an image, and never the
// reverse. Ranks are spaced so that something can be inserted between two of
// them without renumbering the rest.
//
// The scheduler was first in an earlier version of this list, on the reasoning
// that it is entered from everywhere. That is true and it is the wrong way
// round. What decides the order is who calls whom *while holding*: a pipe with
// nothing in it holds its own lock, checks, and then asks the scheduler to
// block - so the blocking subsystems come before the scheduler. And the
// scheduler, tearing a process down, calls into handles and shared memory
// while holding its own - so those come after it. The rank order is the call
// graph, not an intuition about importance.
//
// Note that a rank is only compared against what is held *now*. Taking a lock,
// releasing it, and then taking a lower-ranked one is fine and common: ipc's
// reply resolves capabilities (handles, 30) and then wakes the caller
// (scheduler, 20), and the two never overlap.

namespace sync {

enum class Rank : u32 {
    None      = 0,
    Files     = 10,     // pipes, the console device, pseudo-terminals
    Ipc       = 15,     // ports and requests in flight
    Scheduler = 20,     // the task table and the run queue
    Handles   = 30,     // per-process capability tables
    Image     = 34,     // held program images
    Shm       = 38,     // shared memory segments
    Vmm       = 50,     // address spaces and page tables
    Pmm       = 60,     // the physical frame bitmap
    Console   = 70,     // printing, which anything may do while holding others
};

class RankedLock {
public:
    explicit constexpr RankedLock(Rank rank, const char* name)
        : m_rank(rank), m_name(name) {}

    void acquire();
    void release();

    Rank        rank() const { return m_rank; }
    const char* name() const { return m_name; }

private:
    Spinlock    m_lock;
    Rank        m_rank;
    const char* m_name;
};

// RAII, with interrupts masked for the duration.
//
// Masked because a holder that is preempted part way through leaves every
// other processor spinning for a lock whose owner is not running - and one of
// those spinners may be the CPU that would have scheduled it back. See the
// note on IrqScopedLock; the same reasoning applies here and matters more,
// because these sections are the ones being made short enough to matter.
class Guard {
public:
    explicit Guard(RankedLock& lock) : m_lock(lock) { m_lock.acquire(); }
    ~Guard() { m_lock.release(); }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;

private:
    cpu::InterruptGuard m_guard;    // first, so interrupts go off before the lock
    RankedLock& m_lock;
};

// Whether this processor holds a lock of exactly this rank. For assertions in
// subsystems that mean to require their own lock is already held.
bool holding(Rank rank);

} // namespace sync
