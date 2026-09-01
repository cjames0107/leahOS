/* See <leah/region.hpp>. */

#include <leah/lock.hpp>
#include <leah/region.hpp>
#include <leah/string.hpp>

namespace region {
namespace {

struct Region {
    bool                used;
    vmm::AddressSpace   space;
    u64                 base;
    u64                 bytes;
    void*               image;
    u64                 offset;
    u64                 flags;
};

Region g_regions[kMaxRegions];

/* How many are live, read without the lock on purpose. Every fork asks whether
 * there is anything to copy and every teardown whether there is anything to
 * forget; on this system the answer is almost always no. Walking the table to
 * learn that, on every fork, while holding the scheduler's lock with
 * interrupts masked, is not free. */
volatile u32 g_live = 0;

/* Below the image cache it names and above nothing in particular: a fault
 * asks this what belongs at an address, and then asks the image for the page. */
sync::RankedLock g_lock(sync::Rank::Region, "region");

} // namespace

void init()
{
    for (usize i = 0; i < kMaxRegions; ++i)
        g_regions[i].used = false;
}

bool add(vmm::AddressSpace space, u64 base, u64 bytes, void* image, u64 offset,
         u64 flags)
{
    if (space == 0 || bytes == 0 || image == nullptr)
        return false;
    sync::Guard guard(g_lock);
    for (usize i = 0; i < kMaxRegions; ++i) {
        if (g_regions[i].used)
            continue;
        g_regions[i] = { true, space, base, bytes, image, offset, flags };
        __atomic_add_fetch(&g_live, 1u, __ATOMIC_SEQ_CST);
        return true;
    }
    return false;
}

bool find(vmm::AddressSpace space, u64 address, void** out_image,
          u64* out_offset, u64* out_flags)
{
    if (__atomic_load_n(&g_live, __ATOMIC_SEQ_CST) == 0)
        return false;
    sync::Guard guard(g_lock);
    for (usize i = 0; i < kMaxRegions; ++i) {
        const Region& r = g_regions[i];
        if (!r.used || r.space != space)
            continue;
        if (address < r.base || address >= r.base + r.bytes)
            continue;
        /* The page, not the byte: what comes back is what to map, and a
         * mapping is a page wide. */
        const u64 within = (address - r.base) & ~(vmm::kPageSize - 1);
        *out_image  = r.image;
        *out_offset = r.offset + within;
        *out_flags  = r.flags;
        return true;
    }
    return false;
}

bool inherit(vmm::AddressSpace from, vmm::AddressSpace to)
{
    if (__atomic_load_n(&g_live, __ATOMIC_SEQ_CST) == 0)
        return true;
    sync::Guard guard(g_lock);
    /* Counted first. Copying half the parent's mappings into a child and then
     * running out would leave it with a space that faults where the parent
     * does not, which is worse than the fork failing. */
    usize wanted = 0, free_slots = 0;
    for (usize i = 0; i < kMaxRegions; ++i) {
        if (g_regions[i].used && g_regions[i].space == from)
            ++wanted;
        else if (!g_regions[i].used)
            ++free_slots;
    }
    if (wanted > free_slots)
        return false;

    for (usize i = 0; i < kMaxRegions && wanted > 0; ++i) {
        if (!g_regions[i].used || g_regions[i].space != from)
            continue;
        --wanted;
        for (usize j = 0; j < kMaxRegions; ++j) {
            if (g_regions[j].used)
                continue;
            g_regions[j] = g_regions[i];
            g_regions[j].space = to;
            __atomic_add_fetch(&g_live, 1u, __ATOMIC_SEQ_CST);
            break;
        }
    }
    return true;
}

void forget(vmm::AddressSpace space)
{
    if (__atomic_load_n(&g_live, __ATOMIC_SEQ_CST) == 0)
        return;
    sync::Guard guard(g_lock);
    for (usize i = 0; i < kMaxRegions; ++i)
        if (g_regions[i].used && g_regions[i].space == space) {
            g_regions[i].used = false;
            __atomic_sub_fetch(&g_live, 1u, __ATOMIC_SEQ_CST);
        }
}

} // namespace region
