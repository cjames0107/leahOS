#pragma once

#include <leah/types.hpp>

// Where the kernel lives, in both address spaces.
//
// The kernel is linked for the top 2 GiB but loaded at 1 MiB physical. The
// mapping is a straight offset - virtual 0xFFFFFFFF80000000 is physical 0 -
// which keeps the translation a subtraction rather than a page-table walk.
//
// These must agree with boot/layout.inc and kernel/linker.ld.

namespace memory {

constexpr vaddr_t kKernelBase     = 0xFFFFFFFF80000000ull;
constexpr paddr_t kKernelPhysical = 0x100000;
constexpr vaddr_t kKernelVirtual  = kKernelBase + kKernelPhysical;

// Valid only for addresses inside the kernel's own higher-half window. The
// heap and any MMIO mapped elsewhere are not covered - those need the VMM.
constexpr paddr_t kernel_virt_to_phys(vaddr_t virt)
{
    return virt - kKernelBase;
}

constexpr vaddr_t kernel_phys_to_virt(paddr_t phys)
{
    return phys + kKernelBase;
}

// How much of physical memory the higher-half window covers. One PDPT entry's
// worth of 2 MiB pages, which is plenty for a kernel image plus its stacks.
constexpr u64 kKernelWindowSize = 1024ull * 1024 * 1024;

} // namespace memory
