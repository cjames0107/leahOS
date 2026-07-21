#pragma once

#include <leah/types.hpp>

// Virtual memory. Builds and owns a 4-level page table, replacing the throwaway
// identity map stage 2 left behind.

namespace vmm {

// Page table entry flags. NoExecute needs EFER.NXE, which init() enables.
enum Flags : u64 {
    None         = 0,
    Present      = 1ull << 0,
    Write        = 1ull << 1,
    User         = 1ull << 2,
    WriteThrough = 1ull << 3,
    NoCache      = 1ull << 4,
    Accessed     = 1ull << 5,
    Dirty        = 1ull << 6,
    Huge         = 1ull << 7,
    Global       = 1ull << 8,
    NoExecute    = 1ull << 63,
};

constexpr u64 kPageSize     = 4096;
constexpr u64 kHugePageSize = 2 * 1024 * 1024;

void init();

// 4 KiB granularity. Splits a containing 2 MiB page if it has to, so callers
// never have to know how the region was originally mapped.
bool map(vaddr_t virt, paddr_t phys, u64 flags);
bool map_range(vaddr_t virt, paddr_t phys, usize bytes, u64 flags);

bool unmap(vaddr_t virt);

// Returns 0 if nothing is mapped there.
paddr_t translate(vaddr_t virt);

paddr_t kernel_page_table();

// Map physical device memory (PCI BARs, framebuffers) as uncacheable. Writing
// through a cache to a device register is a classic way to lose writes.
bool map_mmio(vaddr_t virt, paddr_t phys, usize bytes);

} // namespace vmm
