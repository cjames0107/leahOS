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
    /* Kept for the report below: "taking X while holding X" says what happened
     * and not where, and where is the only thing worth knowing. */
    void* const from  = __builtin_return_address(0);
    void* const from2 = __builtin_return_address(1);

    /* The check happens before the lock is taken, deliberately. A violation
     * detected afterwards has already had the chance to deadlock. */
    if (held.count > 0 && static_cast<u32>(m_rank) <=
                              static_cast<u32>(held.rank[held.count - 1])) {
        console::printf("\n  lock order: taking %s (rank %u) while holding "
                        "%s (rank %u)\n    from %p, called from %p\n",
                        m_name, static_cast<u32>(m_rank),
                        held.name[held.count - 1],
                        static_cast<u32>(held.rank[held.count - 1]),
                        from, from2);
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

void note_acquire(Rank rank, const char* name)
{
    Held& held = mine();
    if (held.count > 0 && static_cast<u32>(rank) <=
                              static_cast<u32>(held.rank[held.count - 1])) {
        console::printf("\n  lock order: taking %s (rank %u) while holding "
                        "%s (rank %u)\n",
                        name, static_cast<u32>(rank),
                        held.name[held.count - 1],
                        static_cast<u32>(held.rank[held.count - 1]));
        panic("locks taken out of order");
    }
    if (held.count >= kMaxDepth) {
        console::printf("\n  lock depth: %s is one too many\n", name);
        panic("too many locks held at once");
    }
    held.rank[held.count] = rank;
    held.name[held.count] = name;
    ++held.count;
}

void note_release(Rank rank)
{
    Held& held = mine();
    /* Quietly ignored when it is not the top. The big lock is handed between
     * tasks at a context switch, so a processor can find itself releasing one
     * it did not record acquiring - which is a fact about the handoff and not
     * a bug to panic over. */
    if (held.count > 0 && held.rank[held.count - 1] == rank)
        --held.count;
}

u32 held_count() { return mine().count; }

void assert_none_held(const char* where)
{
    const Held& held = mine();
    if (held.count == 0)
        return;
    /* The big lock is the exception, and the only one. It is handed to the
     * task being switched to rather than released - that is its whole design,
     * and it is why the depth belongs to the task rather than the processor.
     * Every other lock here is a spin lock that no sleeping task may hold. */
    if (held.count == 1 && held.rank[0] == Rank::Kernel)
        return;
    console::printf("\n  %s while holding %s (rank %u)\n", where,
                    held.name[held.count - 1],
                    static_cast<u32>(held.rank[held.count - 1]));
    panic("a ranked lock was held across a context switch");
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
