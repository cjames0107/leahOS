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

// The direct map: every byte of physical memory mapped at a fixed high-half
// offset, so the kernel can reach any frame as phys + kDirectMapBase. This is
// what frees the entire low half for user space - the kernel no longer keeps an
// identity map down there. PML4 slot 256, the first of the higher half.
constexpr vaddr_t kDirectMapBase = 0xFFFF800000000000ull;

// The kernel heap lives in its own higher-half slot, well clear of the direct
// map and the kernel image, so a heap pointer that strays lands somewhere
// obviously wrong rather than on top of RAM.
constexpr vaddr_t kHeapBase = 0xFFFFC00000000000ull;

constexpr vaddr_t phys_to_direct(paddr_t phys) { return phys + kDirectMapBase; }
constexpr paddr_t direct_to_phys(vaddr_t virt) { return virt - kDirectMapBase; }

// Where a user process's sbrk heap begins - well above the program image (which
// links at 0x400000) and far below the stack, so it can grow without colliding
// with either.
constexpr vaddr_t kUserBrkBase = 0x10000000ull;   // 256 MiB

} // namespace memory
