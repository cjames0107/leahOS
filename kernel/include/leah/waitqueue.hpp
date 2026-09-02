#pragma once

#include <leah/lock.hpp>
#include <leah/types.hpp>

// A place to wait, and the thing that makes waiting safe.
//
// There are four blocking primitives in this kernel and fourteen places that
// use them, and every one of those places is a check-then-block loop written
// by hand. That was free while a single lock covered the whole kernel: nothing
// could run between the check and the block, so the wake could not be missed.
// Taking that lock off the system call path removed the guarantee from all
// fourteen without changing a line of any of them, and only five have been
// converted since. The other nine are a lost wakeup waiting for the timing to
// go the wrong way - one of them hangs a pipe loop today, and one carries a
// one-tick nap whose comment says outright that it is there because a wake can
// go missing.
//
// So the discipline stops being a discipline. To wait on this you must hold
// the lock that protects the condition you are waiting on - the signature says
// so, and there is no overload that does not. The lock is dropped while asleep
// and taken again before returning, so a waker holding it either arrives
// before the check, and the condition is already true, or after the task is
// queued, and the wake is seen. There is no third case for a wakeup to fall
// into.
//
// The queue's own address is its channel. That is not a trick: a condition
// worth waiting on is a thing with an address - a pipe, a port, a terminal -
// and giving the queue an identity is what stops two unrelated conditions
// sharing an integer, which is how the poll channel came to wake every poller
// in the machine on every event.

namespace sched {

class WaitQueue {
public:
    constexpr WaitQueue() = default;

    // Sleep until woken. `held` must be held on entry and is held on return.
    //
    // Spurious wakeups are permitted and callers must expect them: this is
    // always used in a loop that re-checks the condition, because a queue with
    // two waiters wakes both and only one of them can win.
    void wait(sync::RankedLock& held);

    // The same, giving up after `ticks` whether or not anything woke it. For a
    // condition that may never come true - a server waiting for a request that
    // is not going to arrive.
    void wait_until(sync::RankedLock& held, u64 ticks);

    // Wake one waiter, or all of them. One is right when any single waiter can
    // consume what has arrived - a byte in a pipe, a request on a port - and
    // waking the rest only to have them find nothing is work for nothing.
    void wake_one();
    void wake_all();

private:
    // Nothing yet. The queue is identified by its address and the waiting is
    // done by the scheduler's channel mechanism, which walks the task table.
    // That is a linear scan per wake and wants replacing with a real list -
    // but it is an optimisation, and this class exists for the contract above,
    // which is a correctness property. One at a time.
    //
    // The member keeps the address distinct: two empty objects at the same
    // address would be the same queue, and an empty class can share one.
    u32 m_waiters = 0;
};

} // namespace sched
