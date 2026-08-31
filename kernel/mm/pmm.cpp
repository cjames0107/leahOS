#include <leah/console.hpp>
#include <leah/lock.hpp>
#include <leah/memory.hpp>
#include <leah/panic.hpp>
#include <leah/heap.hpp>
#include <leah/pmm.hpp>
#include <leah/string.hpp>

// Laid down by the linker script; these bracket the loaded kernel image.
extern "C" u8 __kernel_start[];
extern "C" u8 __kernel_end[];

namespace pmm {
namespace {

// One bit per frame, 1 = in use. Starting from "everything is used" and
// clearing only what E820 called usable means holes in the map stay reserved
// by default, which is the safe direction to be wrong in.
u64* g_bitmap       = nullptr;
u64  g_bitmap_phys  = 0;        // physical base, so the pointer can be rebased
u64  g_bitmap_words = 0;
u64  g_frame_count  = 0;

u64 g_total_bytes    = 0;   // highest address E820 described, holes included
u64 g_usable_bytes   = 0;   // sum of usable regions
u64 g_highest_usable = 0;   // end of the topmost usable region
u64 g_used_frames  = 0;
u64 g_free_frames  = 0;

// Most allocations follow the previous one, so resuming the scan where it left
// off turns the common case from O(heap) into O(1).
u64 g_search_hint = 0;

/* The first thing carved out from under the big kernel lock.
 *
 * The frame allocator is a leaf: it calls nothing that takes a lock of its
 * own, so it can be made safe on its own terms without waiting for anything
 * else to be. It is also the most contended thing in the kernel - every page
 * fault, every exec, every fork goes through it - so it is the one worth doing
 * first.
 *
 * Still taken underneath the big lock for now, which changes no behaviour. The
 * point of this step is that the discipline exists and the ordering is being
 * exercised on every boot, so that when the big lock goes the ranks have
 * already been proved right rather than reasoned about. */
sync::RankedLock g_lock(sync::Rank::Pmm, "pmm");

bool test(u64 frame)
{
    /* Outside the bitmap is "not free", which is the safe answer and the true
     * one: a frame the allocator does not own is not a frame it can hand out.
     * Without the bound this read ran off the end of the bitmap - a mapped
     * framebuffer is at 0xfd000000 on this machine, three megabytes of frames
     * the allocator has never heard of, and every process that unmaps one asks
     * about them. */
    if (frame >= g_frame_count)
        return true;
    return (g_bitmap[frame / 64] >> (frame % 64) & 1) != 0;
}

void mark_used(u64 frame)
{
    if (frame >= g_frame_count)
        return;
    if (!test(frame)) {
        g_bitmap[frame / 64] |= 1ull << (frame % 64);
        ++g_used_frames;
        --g_free_frames;
    }
}

void mark_free(u64 frame)
{
    if (frame >= g_frame_count)
        return;
    if (test(frame)) {
        g_bitmap[frame / 64] &= ~(1ull << (frame % 64));
        --g_used_frames;
        ++g_free_frames;
    }
}

void reserve(u64 base, u64 length)
{
    const u64 first = page_align_down(base) >> kPageShift;
    const u64 last  = page_align_up(base + length) >> kPageShift;
    for (u64 frame = first; frame < last; ++frame)
        mark_used(frame);
}

void release(u64 base, u64 length)
{
    // Only whole frames wholly inside the region are usable; rounding the
    // other way would hand out memory the firmware never offered.
    const u64 first = page_align_up(base) >> kPageShift;
    const u64 last  = page_align_down(base + length) >> kPageShift;
    for (u64 frame = first; frame < last; ++frame)
        mark_free(frame);
}

/* Freed frames are filled with 0xCC, and checked on the way back out.
 *
 * A frame handed to a second owner while the first still maps it is the one
 * failure this allocator cannot report on its own: both sides are doing
 * something legitimate and the damage appears somewhere else entirely, as a
 * page of a program's code reading back as zeros long afterwards.
 *
 * 0xCC is int3, so the first owner executing out of a page it has lost traps
 * immediately and says where. Reading one gets an unmistakable pattern rather
 * than plausible data. And a frame that comes back out of the allocator only
 * partly poisoned was written to after it was freed, which is the other half
 * of the same bug seen from the other side.
 *
 * Costs a page-sized store per free. Worth it while there is a bug like that
 * to find; turn it off when there is not.
 */
constexpr bool kPoisonFreedFrames = true;
constexpr u64  kPoisonWord = 0xCCCCCCCCCCCCCCCCull;

/* Reaching a frame by its physical address needs the direct map, and the
 * direct map is built out of frames - so there is a window early in the boot
 * where freeing one cannot touch it. */
bool g_can_reach_frames = false;

void poison(paddr_t frame)
{
    if (!kPoisonFreedFrames || !g_can_reach_frames)
        return;
    auto* words = reinterpret_cast<u64*>(memory::phys_to_direct(frame));
    for (usize i = 0; i < kPageSize / sizeof(u64); ++i)
        words[i] = kPoisonWord;
}

/* Whether this frame was poisoned and then written to. Frames that have been
 * free since boot hold whatever the firmware left and are not poisoned, so the
 * first word is what says whether there is anything to check. */
void check_poison(paddr_t frame)
{
    if (!kPoisonFreedFrames || !g_can_reach_frames)
        return;
    const auto* words =
        reinterpret_cast<const u64*>(memory::phys_to_direct(frame));
    if (words[0] != kPoisonWord)
        return;                         // never poisoned: nothing to say
    for (usize i = 1; i < kPageSize / sizeof(u64); ++i) {
        if (words[i] == kPoisonWord)
            continue;
        console::printf("  pmm: frame %p was written to after it was freed "
                        "(word %u is %016llx)\n",
                        reinterpret_cast<void*>(frame),
                        static_cast<unsigned>(i), words[i]);
        return;
    }
}

/* Allocating, with the lock already held. Same reason as free_locked below:
 * asking for one contiguous frame is asking for a frame, and the delegation
 * that expressed it took the lock a second time. */
paddr_t alloc_locked()
{
    for (u64 pass = 0; pass < 2; ++pass) {
        const u64 start = pass == 0 ? g_search_hint : 0;
        const u64 stop  = pass == 0 ? g_bitmap_words : g_search_hint;

        for (u64 word = start; word < stop; ++word) {
            if (g_bitmap[word] == ~0ull)
                continue;                       // fully allocated, skip 64 frames

            for (u64 bit = 0; bit < 64; ++bit) {
                const u64 frame = word * 64 + bit;
                if (frame >= g_frame_count)
                    break;
                if (test(frame))
                    continue;
                mark_used(frame);
                g_search_hint = word;
                check_poison(frame << kPageShift);
                return frame << kPageShift;
            }
        }
    }
    return 0;
}

/* Freeing, with the lock already held.
 *
 * There are three public ways to give a frame back and two of them are written
 * in terms of the third, which is exactly the shape that turns a lock into a
 * deadlock the first time it is taken twice. The ranked lock catches that as a
 * panic rather than a hang - which is how this was found - but the fix is to
 * have one unlocked body and let the entry points wrap it. */
void free_locked(paddr_t frame)
{
    if (frame == 0)
        return;
    const u64 index = frame >> kPageShift;
    if (index < g_frame_count && test(index))
        poison(frame);
    mark_free(index);
    if (index / 64 < g_search_hint)
        g_search_hint = index / 64;
}

} // namespace

void init(const boot::Info& info)
{
    const boot::MemoryRegion* regions = boot::memory_map();
    const u32 count = info.e820_count;

    // Pass 1: how much address space do we have to describe?
    for (u32 i = 0; i < count; ++i) {
        const boot::MemoryRegion& r = regions[i];
        const u64 end = r.base + r.length;
        if (end > g_total_bytes)
            g_total_bytes = end;
        if (r.type == boot::RegionType::Usable)
            g_usable_bytes += r.length;
    }

    // Tracking every frame up to the top of a machine with sparse high MMIO
    // would waste a lot of bitmap on holes, so cap at the highest usable byte.
    for (u32 i = 0; i < count; ++i) {
        const boot::MemoryRegion& r = regions[i];
        if (r.type == boot::RegionType::Usable && r.base + r.length > g_highest_usable)
            g_highest_usable = r.base + r.length;
    }

    g_frame_count  = page_align_up(g_highest_usable) >> kPageShift;
    g_bitmap_words = (g_frame_count + 63) / 64;

    // Pass 2: park the bitmap immediately after the kernel image. That is
    // inside the big usable region below 4 GiB on every machine we care about,
    // and it needs no allocator to place - which is the point, since this is
    // the allocator.
    // The linker symbols are higher-half addresses now, so everything the
    // allocator reasons about has to be converted back to physical first.
    const u64 kernel_phys_start =
        memory::kernel_virt_to_phys(reinterpret_cast<u64>(__kernel_start));
    const u64 kernel_phys_end =
        memory::kernel_virt_to_phys(reinterpret_cast<u64>(__kernel_end));

    const u64 bitmap_bytes = g_bitmap_words * sizeof(u64);
    const u64 bitmap_base  = page_align_up(kernel_phys_end);

    bool bitmap_fits = false;
    for (u32 i = 0; i < count; ++i) {
        const boot::MemoryRegion& r = regions[i];
        if (r.type != boot::RegionType::Usable)
            continue;
        if (bitmap_base >= r.base && bitmap_base + bitmap_bytes <= r.base + r.length) {
            bitmap_fits = true;
            break;
        }
    }
    if (!bitmap_fits)
        panic("pmm: no usable region can hold the frame bitmap");

    g_bitmap_phys = bitmap_base;
    g_bitmap = reinterpret_cast<u64*>(bitmap_base);   // identity, valid pre-VMM

    memset(g_bitmap, 0xFF, bitmap_bytes);
    g_used_frames = g_frame_count;
    g_free_frames = 0;

    for (u32 i = 0; i < count; ++i) {
        const boot::MemoryRegion& r = regions[i];
        if (r.type == boot::RegionType::Usable)
            release(r.base, r.length);
    }

    // Now take back everything that is spoken for. The first megabyte covers
    // the IVT, the BIOS data area, both bootloader stages, the E820 map and
    // stage 2's page tables in one stroke - none of it is worth reclaiming.
    reserve(0, 0x100000);
    reserve(kernel_phys_start, kernel_phys_end - kernel_phys_start);
    reserve(bitmap_base, bitmap_bytes);

    g_search_hint = 0;
}

paddr_t alloc()
{
    sync::Guard guard(g_lock);
    return alloc_locked();
}

paddr_t alloc_contiguous(usize frames)
{
    sync::Guard guard(g_lock);
    if (frames == 0)
        return 0;
    if (frames == 1)
        return alloc_locked();

    // Contiguous runs are rare enough (DMA buffers, page-table groups) that a
    // linear scan is fine; the bitmap is the only structure we have anyway.
    u64 run_start = 0;
    u64 run = 0;

    for (u64 frame = 0; frame < g_frame_count; ++frame) {
        if (test(frame)) {
            run = 0;
            continue;
        }
        if (run == 0)
            run_start = frame;
        if (++run == frames) {
            for (u64 i = 0; i < frames; ++i)
                mark_used(run_start + i);
            return run_start << kPageShift;
        }
    }
    return 0;
}

void free(paddr_t frame)
{
    sync::Guard guard(g_lock);
    free_locked(frame);
}

// --- reference counts -------------------------------------------------------
//
// One u16 per frame, holding the number of references *beyond the first*. Zero
// is the common case - a frame with a single owner - so the table needs no
// initialisation pass and an ordinary alloc/free costs nothing extra.

u16* g_refs = nullptr;
u64  g_ref_count = 0;

void init_refcounts()
{
    if (g_refs != nullptr)
        return;
    g_ref_count = g_highest_usable >> kPageShift;
    auto* table = static_cast<u16*>(kmalloc(g_ref_count * sizeof(u16)));
    if (table == nullptr)
        return;                     // no sharing available; fork stays a copy
    memset(table, 0, g_ref_count * sizeof(u16));
    g_refs = table;
}

bool share(paddr_t frame)
{
    sync::Guard guard(g_lock);
    const u64 index = frame >> kPageShift;
    if (g_refs == nullptr || index >= g_ref_count)
        return false;
    if (g_refs[index] == 0xFFFF)
        return false;               // saturated: caller must copy instead
    ++g_refs[index];
    return true;
}

bool is_shared(paddr_t frame)
{
    const u64 index = frame >> kPageShift;
    if (g_refs == nullptr || index >= g_ref_count)
        return false;
    return g_refs[index] > 0;
}

void release(paddr_t frame)
{
    sync::Guard guard(g_lock);
    const u64 index = frame >> kPageShift;
    if (g_refs != nullptr && index < g_ref_count && g_refs[index] > 0) {
        --g_refs[index];            // someone else still holds it
        return;
    }
    free_locked(frame);
}

void free_contiguous(paddr_t base, usize frames)
{
    sync::Guard guard(g_lock);
    for (usize i = 0; i < frames; ++i)
        free_locked(base + i * kPageSize);
}

void use_direct_map()
{
    g_can_reach_frames = true;
    // The low identity map is gone; reach the bitmap through the direct map.
    g_bitmap = reinterpret_cast<u64*>(memory::phys_to_direct(g_bitmap_phys));
}

u64 total_bytes()    { return g_total_bytes; }
u64 highest_usable() { return g_highest_usable; }
u64 usable_bytes() { return g_usable_bytes; }
u64 used_bytes()   { return g_used_frames * kPageSize; }
u64 free_bytes()   { return g_free_frames * kPageSize; }

} // namespace pmm
