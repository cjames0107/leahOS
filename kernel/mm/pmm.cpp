#include <leah/panic.hpp>
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

bool test(u64 frame)
{
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

} // namespace

void init(const boot::MemoryMap& map)
{
    // Pass 1: how much address space do we have to describe?
    for (u32 i = 0; i < map.count; ++i) {
        const boot::MemoryRegion& r = map.regions[i];
        const u64 end = r.base + r.length;
        if (end > g_total_bytes)
            g_total_bytes = end;
        if (r.type == boot::RegionType::Usable)
            g_usable_bytes += r.length;
    }

    // Tracking every frame up to the top of a machine with sparse high MMIO
    // would waste a lot of bitmap on holes, so cap at the highest usable byte.
    for (u32 i = 0; i < map.count; ++i) {
        const boot::MemoryRegion& r = map.regions[i];
        if (r.type == boot::RegionType::Usable && r.base + r.length > g_highest_usable)
            g_highest_usable = r.base + r.length;
    }

    g_frame_count  = page_align_up(g_highest_usable) >> kPageShift;
    g_bitmap_words = (g_frame_count + 63) / 64;

    // Pass 2: park the bitmap immediately after the kernel image. That is
    // inside the big usable region below 4 GiB on every machine we care about,
    // and it needs no allocator to place - which is the point, since this is
    // the allocator.
    const u64 bitmap_bytes = g_bitmap_words * sizeof(u64);
    const u64 bitmap_base  = page_align_up(reinterpret_cast<u64>(__kernel_end));

    bool bitmap_fits = false;
    for (u32 i = 0; i < map.count; ++i) {
        const boot::MemoryRegion& r = map.regions[i];
        if (r.type != boot::RegionType::Usable)
            continue;
        if (bitmap_base >= r.base && bitmap_base + bitmap_bytes <= r.base + r.length) {
            bitmap_fits = true;
            break;
        }
    }
    if (!bitmap_fits)
        panic("pmm: no usable region can hold the frame bitmap");

    g_bitmap = reinterpret_cast<u64*>(bitmap_base);

    memset(g_bitmap, 0xFF, bitmap_bytes);
    g_used_frames = g_frame_count;
    g_free_frames = 0;

    for (u32 i = 0; i < map.count; ++i) {
        const boot::MemoryRegion& r = map.regions[i];
        if (r.type == boot::RegionType::Usable)
            release(r.base, r.length);
    }

    // Now take back everything that is spoken for. The first megabyte covers
    // the IVT, the BIOS data area, both bootloader stages, the E820 map and
    // stage 2's page tables in one stroke - none of it is worth reclaiming.
    reserve(0, 0x100000);
    reserve(reinterpret_cast<u64>(__kernel_start),
            reinterpret_cast<u64>(__kernel_end) - reinterpret_cast<u64>(__kernel_start));
    reserve(bitmap_base, bitmap_bytes);

    g_search_hint = 0;
}

paddr_t alloc()
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
                return frame << kPageShift;
            }
        }
    }
    return 0;
}

paddr_t alloc_contiguous(usize frames)
{
    if (frames == 0)
        return 0;
    if (frames == 1)
        return alloc();

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
    if (frame == 0)
        return;
    const u64 index = frame >> kPageShift;
    mark_free(index);
    if (index / 64 < g_search_hint)
        g_search_hint = index / 64;
}

void free_contiguous(paddr_t base, usize frames)
{
    for (usize i = 0; i < frames; ++i)
        free(base + i * kPageSize);
}

u64 total_bytes()    { return g_total_bytes; }
u64 highest_usable() { return g_highest_usable; }
u64 usable_bytes() { return g_usable_bytes; }
u64 used_bytes()   { return g_used_frames * kPageSize; }
u64 free_bytes()   { return g_free_frames * kPageSize; }

} // namespace pmm
