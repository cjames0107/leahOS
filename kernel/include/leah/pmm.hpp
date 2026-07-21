#pragma once

#include <leah/bootinfo.hpp>
#include <leah/types.hpp>

// Physical frame allocator.
//
// Owns every usable page of RAM the E820 map reported. A bitmap costs one bit
// per 4 KiB frame - 128 KiB to track 4 GiB - and needs no allocator of its own
// to bootstrap, which matters when this is the thing everything else allocates
// from.

namespace pmm {

constexpr u64 kPageSize = 4096;
constexpr u64 kPageShift = 12;

constexpr u64 page_align_down(u64 address) { return address & ~(kPageSize - 1); }
constexpr u64 page_align_up(u64 address)
{
    return (address + kPageSize - 1) & ~(kPageSize - 1);
}

void init(const boot::Info& info);

// Returns a physical address, or 0 on exhaustion. Frames are not zeroed.
paddr_t alloc();
paddr_t alloc_contiguous(usize frames);

void free(paddr_t frame);
void free_contiguous(paddr_t base, usize frames);

u64 total_bytes();      // highest address E820 described, holes included
u64 highest_usable();   // end of the topmost usable region
u64 usable_bytes();
u64 used_bytes();
u64 free_bytes();

} // namespace pmm
