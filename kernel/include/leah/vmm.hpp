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
    // Bits 9-11 are ignored by the CPU and left to the OS. This one marks a
    // page that several address spaces share read-only and must be copied
    // before anyone writes to it.
    CopyOnWrite  = 1ull << 9,
    // A genuinely shared mapping - shared memory, not a fork artefact. fork
    // must hand it to the child as it stands rather than making it
    // copy-on-write: the whole point of the segment is that writes are seen by
    // everyone mapping it, and copying it on the first write would quietly turn
    // one shared page into two private ones.
    Shared       = 1ull << 10,
    NoExecute    = 1ull << 63,
};

constexpr u64 kPageSize     = 4096;
constexpr u64 kHugePageSize = 2 * 1024 * 1024;

// An address space is just its top-level page table, named by physical address.
// The kernel's mappings (the identity map, the heap, the kernel image) are
// copied into every user space so the kernel is reachable no matter which one
// is active - which is what lets a syscall or interrupt run in a process's
// address space without switching back first.
using AddressSpace = paddr_t;

void init();

// The per-processor half of init(): control-register state an application
// processor must adopt before it can run user tasks.
void init_this_cpu();


// 4 KiB granularity, operating on whichever space is currently active. Splits a
// containing 2 MiB page if it has to, so callers never have to know how the
// region was originally mapped.
bool map(vaddr_t virt, paddr_t phys, u64 flags);
bool map_range(vaddr_t virt, paddr_t phys, usize bytes, u64 flags);
bool unmap(vaddr_t virt);
paddr_t translate(vaddr_t virt);        // 0 if nothing is mapped there

// Map physical device memory (PCI BARs, framebuffers) as uncacheable. Writing
// through a cache to a device register is a classic way to lose writes.
bool map_mmio(vaddr_t virt, paddr_t phys, usize bytes);

// --- address spaces ---------------------------------------------------------

AddressSpace kernel_space();
AddressSpace current_space();

// A fresh user space that shares the kernel's mappings. 0 on failure.
AddressSpace create_address_space();

// A new space that is a deep copy of `parent`: the kernel mappings are shared
// as usual, and every user page is copied into a freshly allocated frame - so
// the two processes read the same bytes but writes do not cross. This is the
// address-space half of fork(). 0 on failure.
AddressSpace fork_address_space(AddressSpace parent);

// Frees the space's private (user) page tables, the frames they mapped, and the
// top-level table itself. The shared kernel mappings are left untouched.
void destroy_address_space(AddressSpace space);

// Handle a write fault on a copy-on-write page: give the faulting space its own
// private copy and make it writable again. Returns false if `virt` was not a
// CoW page, in which case the fault is a real one.
bool handle_cow_fault(vaddr_t virt);

// The page table entry behind an address, so a fault report can say whether the
// page was missing, read-only, or something else again. Zero when unmapped.
u64 entry_for(vaddr_t virt);

// Remember what the tables said when a fault was taken, and read it back. The
// walk has to happen before anything that could switch address spaces, or the
// report describes whichever process happens to be loaded when it prints.
void note_fault_mapping(vaddr_t virt, u64 entry);
bool recorded_fault_mapping(vaddr_t virt, u64& entry);

// Drop every mapping in the kernel's low half and free the tables describing
// them, leaving the frames themselves untouched. The AP trampoline's identity
// map is the only such mapping, and it must not survive into the address spaces
// that copy the kernel's PML4 entries.
void release_low_half();

// --- TLB coherence ----------------------------------------------------------
//
// A page table is shared: threads of one process can run on different CPUs, and
// the kernel's own mappings are in every address space. Changing an entry only
// invalidates the TLB of the CPU that changed it, so every other CPU has to be
// told - or it keeps using a translation that no longer exists, which after the
// frame is reused means reading and writing someone else's memory.

// Enable shootdowns. Until the application processors are actually scheduling
// there is nobody to tell, and a CPU halted with interrupts off would never
// acknowledge.
void enable_tlb_shootdown(u32 total_cpus);

// Make every other CPU drop its cached translations. Returns once they have.
void shootdown();

// Called from the shootdown interrupt on the receiving CPU.
void on_shootdown_ipi();

// Answer an outstanding shootdown without needing an interrupt. Spin loops call
// this so a CPU waiting with interrupts masked can still acknowledge one.
void ack_shootdown();

// Make a space active: load CR3 and record it as current. Cheap to call with
// the already-active space (skips the reload).
void switch_address_space(AddressSpace space);

paddr_t kernel_page_table();

} // namespace vmm
