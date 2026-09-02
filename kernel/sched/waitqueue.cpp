/* See <leah/waitqueue.hpp>. */

#include <leah/scheduler.hpp>
#include <leah/waitqueue.hpp>

namespace sched {
namespace {

/* The channel a queue waits on: its own address.
 *
 * Distinct by construction, which is the property the ad-hoc channels did not
 * have - a pipe used its own address plus one for the far direction, and the
 * poll channel is a constant every poller in the machine shares. */
u64 channel_of(const WaitQueue* q) { return reinterpret_cast<u64>(q); }

} // namespace

void WaitQueue::wait(sync::RankedLock& held)
{
    ++m_waiters;
    scheduler::block_on_releasing(channel_of(this), held);
    if (m_waiters > 0)
        --m_waiters;
}

void WaitQueue::wait_until(sync::RankedLock& held, u64 ticks)
{
    ++m_waiters;
    scheduler::block_on_until_releasing(channel_of(this), ticks, held);
    if (m_waiters > 0)
        --m_waiters;
}

void WaitQueue::wake_one()
{
    /* Skipped when nobody is waiting, which is the common case for a pipe that
     * is being read as fast as it is written. Waking costs a walk of the task
     * table; not waking costs a load. */
    if (m_waiters == 0)
        return;
    scheduler::wake_n(channel_of(this), 1);
}

void WaitQueue::wake_all()
{
    if (m_waiters == 0)
        return;
    scheduler::wake(channel_of(this));
}

} // namespace sched
