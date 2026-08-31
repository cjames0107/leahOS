/* See <leah/lock.hpp>. */

#include <leah/console.hpp>
#include <leah/lock.hpp>
#include <leah/panic.hpp>
#include <leah/percpu.hpp>

namespace sync {
namespace {

/* What each processor is holding, in the order it took them.
 *
 * Kept here rather than in the per-CPU block because that block's layout is
 * known to assembly by byte offset, and this is a debugging structure that
 * should not be able to break the entry path by growing.
 */
constexpr usize kMaxCpus  = 32;
constexpr usize kMaxDepth = 8;

struct Held {
    Rank        rank[kMaxDepth];
    const char* name[kMaxDepth];
    u32         count;
};

Held g_held[kMaxCpus];

Held& mine()
{
    const u32 slot = percpu::active();
    return g_held[slot < kMaxCpus ? slot : 0];
}

} // namespace

void RankedLock::acquire()
{
    Held& held = mine();

    /* The check happens before the lock is taken, deliberately. A violation
     * detected afterwards has already had the chance to deadlock. */
    if (held.count > 0 && static_cast<u32>(m_rank) <=
                              static_cast<u32>(held.rank[held.count - 1])) {
        console::printf("\n  lock order: taking %s (rank %u) while holding "
                        "%s (rank %u)\n",
                        m_name, static_cast<u32>(m_rank),
                        held.name[held.count - 1],
                        static_cast<u32>(held.rank[held.count - 1]));
        panic("locks taken out of order");
    }
    if (held.count >= kMaxDepth) {
        console::printf("\n  lock depth: %s is one too many\n", m_name);
        panic("too many locks held at once");
    }

    m_lock.acquire();

    held.rank[held.count] = m_rank;
    held.name[held.count] = m_name;
    ++held.count;
}

void RankedLock::release()
{
    Held& held = mine();
    /* Strictly increasing acquisition makes release last-in-first-out, and a
     * release out of order is as much a bug as an acquire out of order. */
    if (held.count == 0 || held.rank[held.count - 1] != m_rank) {
        console::printf("\n  lock release: %s was not the last taken\n", m_name);
        panic("locks released out of order");
    }
    --held.count;
    m_lock.release();
}

bool holding(Rank rank)
{
    const Held& held = mine();
    for (u32 i = 0; i < held.count; ++i)
        if (held.rank[i] == rank)
            return true;
    return false;
}

} // namespace sync
