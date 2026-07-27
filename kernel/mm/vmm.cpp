#include <leah/cpu.hpp>
#include <leah/memory.hpp>
#include <leah/panic.hpp>
#include <leah/pmm.hpp>
#include <leah/string.hpp>
#include <leah/vmm.hpp>

extern "C" u8 __kernel_start[];
extern "C" u8 __kernel_end[];

namespace vmm {
namespace {

constexpr u64 kEntriesPerTable = 512;
constexpr u64 kAddressMask = 0x000FFFFFFFFFF000ull;   // bits 51:12 of an entry

u64 g_pml4_phys = 0;         // the kernel's own top-level table (physical)
u64 g_current_pml4 = 0;      // the table CR3 currently points at (physical)

// Page-table entries always hold physical addresses. To read or write a table
// the kernel needs a virtual pointer to it, which comes from the direct map -
// phys + kDirectMapBase. During early boot, before the direct map exists, the
// stage-2 identity map still makes phys usable as a virtual address, so this
// starts as a plain identity and flips to the offset once init() has loaded the
// new tables.
bool g_direct_map = false;

u64* phys_ptr(paddr_t phys)
{
    return reinterpret_cast<u64*>(g_direct_map ? memory::phys_to_direct(phys) : phys);
}

u64* kernel_pml4()  { return phys_ptr(g_pml4_phys); }
u64* current_pml4() { return phys_ptr(g_current_pml4); }

u64* table_of(u64 entry)
{
    return phys_ptr(entry & kAddressMask);
}

u64 index_of(vaddr_t virt, int level)   // level 4 = PML4 ... 1 = PT
{
    return virt >> (12 + 9 * (level - 1)) & 0x1FF;
}

void invalidate(vaddr_t virt)
{
    asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

// Allocate a zeroed page table and return its physical address (0 on failure).
// The entry that will point at it stores this physical address; walking to it
// goes through phys_ptr.
paddr_t alloc_table()
{
    const paddr_t frame = pmm::alloc();
    if (frame == 0)
        return 0;
    memset(phys_ptr(frame), 0, pmm::kPageSize);
    return frame;
}

// Walk to the next level, creating it when asked. Intermediate entries are
// deliberately permissive - Present|Write|User - because on x86 the effective
// permission is the AND down the whole path, so restrictions belong on the leaf.
// Leaving User off here would make it impossible to ever map a user page below.
u64* next_level(u64* table, u64 index, bool create)
{
    if ((table[index] & Present) == 0) {
        if (!create)
            return nullptr;
        const paddr_t fresh = alloc_table();
        if (fresh == 0)
            return nullptr;
        table[index] = fresh | Present | Write | User;      // entry holds phys
        return phys_ptr(fresh);
    }
    return table_of(table[index]);
}

// Replace a 2 MiB leaf with a page table describing the same 512 frames, so a
// 4 KiB mapping can be punched into it.
bool split_huge_page(u64* pd, u64 index)
{
    const u64 entry = pd[index];
    const u64 base  = entry & kAddressMask;
    const u64 flags = entry & ~kAddressMask & ~static_cast<u64>(Huge);

    const paddr_t pt_phys = alloc_table();
    if (pt_phys == 0)
        return false;

    u64* pt = phys_ptr(pt_phys);
    for (u64 i = 0; i < kEntriesPerTable; ++i)
        pt[i] = (base + i * kPageSize) | flags;

    pd[index] = pt_phys | Present | Write | User;
    return true;
}

} // namespace

void init()
{
    // NXE has to be on before any entry sets bit 63, or the CPU faults on a
    // reserved bit rather than honouring the flag.
    constexpr u32 kIa32Efer = 0xC0000080;
    cpu::write_msr(kIa32Efer, cpu::read_msr(kIa32Efer) | (1ull << 11));

    // CR0.WP makes read-only pages read-only for the *kernel* too. Without it
    // ring 0 may write through any mapping regardless of its write bit, which
    // silently defeats copy-on-write: a kernel routine filling a user buffer
    // would write straight into the page a forked process still shares, instead
    // of faulting and being given a private copy first.
    constexpr u64 kCr0WriteProtect = 1ull << 16;
    u64 cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    asm volatile("mov %0, %%cr0" : : "r"(cr0 | kCr0WriteProtect) : "memory");

    // Built with the direct map not yet active: phys_ptr is still identity, and
    // the stage-2 low identity map that is live right now makes that valid.
    const paddr_t pml4_phys = alloc_table();
    if (pml4_phys == 0)
        panic("vmm: cannot allocate PML4");
    g_pml4_phys = pml4_phys;
    u64* pml4 = phys_ptr(pml4_phys);

    // The direct map: all physical memory at kDirectMapBase, 2 MiB pages, in the
    // high half. Bounded to cover at least the low 4 GiB - so the MMIO window
    // with the LAPIC, PCI BARs and framebuffer is reachable - and up to the top
    // of usable RAM. Nothing is mapped in the low half, which leaves it entirely
    // to user space. These leaves are kernel-only (no User bit).
    const u64 four_gib = 4ull * 1024 * 1024 * 1024;
    const u64 top = pmm::highest_usable() > four_gib ? pmm::highest_usable() : four_gib;
    for (u64 phys = 0; phys < top; phys += kHugePageSize) {
        const vaddr_t virt = memory::kDirectMapBase + phys;
        u64* pdpt = next_level(pml4, index_of(virt, 4), true);
        u64* pd = pdpt != nullptr ? next_level(pdpt, index_of(virt, 3), true) : nullptr;
        if (pd == nullptr)
            panic("vmm: out of memory building the direct map");
        pd[index_of(virt, 2)] = phys | Present | Write | Huge;
    }

    // The kernel is executing from the higher half right now, so the new tables
    // must describe its window before CR3 is loaded - otherwise the very next
    // instruction fetch faults with no handler mapped to catch it.
    for (u64 offset = 0; offset < memory::kKernelWindowSize; offset += kHugePageSize) {
        const vaddr_t virt = memory::kKernelBase + offset;
        u64* pdpt = next_level(pml4, index_of(virt, 4), true);
        u64* pd = pdpt != nullptr ? next_level(pdpt, index_of(virt, 3), true) : nullptr;
        if (pd == nullptr)
            panic("vmm: out of memory mapping the kernel window");
        pd[index_of(virt, 2)] = offset | Present | Write | Huge;
    }

    g_current_pml4 = g_pml4_phys;
    asm volatile("mov %0, %%cr3" : : "r"(g_pml4_phys) : "memory");

    // The low identity map is gone now; every physical dereference from here on
    // goes through the direct map we just installed.
    g_direct_map = true;
}

namespace {

bool map_into(u64* pml4, vaddr_t virt, paddr_t phys, u64 flags)
{
    if (pml4 == nullptr)
        return false;

    u64* pdpt = next_level(pml4, index_of(virt, 4), true);
    if (pdpt == nullptr)
        return false;
    u64* pd = next_level(pdpt, index_of(virt, 3), true);
    if (pd == nullptr)
        return false;

    const u64 pd_index = index_of(virt, 2);
    if ((pd[pd_index] & Present) != 0 && (pd[pd_index] & Huge) != 0) {
        if (!split_huge_page(pd, pd_index))
            return false;
    }

    u64* pt = next_level(pd, pd_index, true);
    if (pt == nullptr)
        return false;

    pt[index_of(virt, 1)] = (phys & kAddressMask) | flags | Present;
    invalidate(virt);
    return true;
}

bool unmap_into(u64* pml4, vaddr_t virt)
{
    u64* pdpt = next_level(pml4, index_of(virt, 4), false);
    if (pdpt == nullptr)
        return false;
    u64* pd = next_level(pdpt, index_of(virt, 3), false);
    if (pd == nullptr)
        return false;

    const u64 pd_index = index_of(virt, 2);
    if ((pd[pd_index] & Present) != 0 && (pd[pd_index] & Huge) != 0) {
        if (!split_huge_page(pd, pd_index))
            return false;
    }

    u64* pt = next_level(pd, pd_index, false);
    if (pt == nullptr)
        return false;

    pt[index_of(virt, 1)] = 0;
    invalidate(virt);
    return true;
}

// The address of the leaf entry describing `virt`, or null when the walk runs
// out of tables or lands on a huge page. Returning the entry itself (rather than
// its contents) is what lets the CoW fault rewrite it in place.
u64* walk_to_pte(u64* pml4, vaddr_t virt)
{
    u64* pdpt = next_level(pml4, index_of(virt, 4), false);
    if (pdpt == nullptr)
        return nullptr;
    u64* pd = next_level(pdpt, index_of(virt, 3), false);
    if (pd == nullptr)
        return nullptr;

    const u64 pd_index = index_of(virt, 2);
    if ((pd[pd_index] & Present) == 0 || (pd[pd_index] & Huge) != 0)
        return nullptr;

    u64* pt = table_of(pd[pd_index]);
    return &pt[index_of(virt, 1)];
}

paddr_t translate_into(u64* pml4, vaddr_t virt)
{
    u64* pdpt = next_level(pml4, index_of(virt, 4), false);
    if (pdpt == nullptr)
        return 0;
    u64* pd = next_level(pdpt, index_of(virt, 3), false);
    if (pd == nullptr)
        return 0;

    const u64 pd_entry = pd[index_of(virt, 2)];
    if ((pd_entry & Present) == 0)
        return 0;
    if ((pd_entry & Huge) != 0)
        return (pd_entry & kAddressMask) + (virt & (kHugePageSize - 1));

    u64* pt = table_of(pd_entry);
    const u64 pt_entry = pt[index_of(virt, 1)];
    if ((pt_entry & Present) == 0)
        return 0;
    return (pt_entry & kAddressMask) + (virt & (kPageSize - 1));
}

// Free a PDPT/PD/PT sub-tree and, at the leaf level, the frames it mapped. Used
// to tear down a user space. `level` is the table's own level (4=PML4 slot's
// PDPT ... 1=PT); a Huge entry is a leaf one level up.
void free_table_tree(u64 table_phys, int level)
{
    u64* table = phys_ptr(table_phys);
    for (u64 i = 0; i < kEntriesPerTable; ++i) {
        const u64 entry = table[i];
        if ((entry & Present) == 0)
            continue;
        const u64 target = entry & kAddressMask;
        if (level == 1 || (entry & Huge) != 0) {
            // A data frame, which another address space may still share after a
            // copy-on-write fork - so drop a reference rather than freeing it
            // outright. Handing a frame back to the allocator while a live
            // process still maps it is exactly how a fork corrupts its parent.
            pmm::release(target);
        } else {
            free_table_tree(target, level - 1);
        }
    }
    // The page tables themselves are never shared; this space built its own.
    pmm::free(table_phys);
}

} // namespace

bool map(vaddr_t virt, paddr_t phys, u64 flags)
{
    return map_into(current_pml4(), virt, phys, flags);
}

// The first write to a shared page. If we are the only owner left the page can
// simply be made writable again; otherwise it is copied and this space keeps the
// copy, dropping its reference to the original.
bool handle_cow_fault(vaddr_t virt)
{
    u64* pt_entry = walk_to_pte(current_pml4(), virt);
    if (pt_entry == nullptr)
        return false;

    const u64 entry = *pt_entry;
    if ((entry & Present) == 0 || (entry & CopyOnWrite) == 0)
        return false;

    const paddr_t source = entry & kAddressMask;
    const u64 flags = (entry & (User | NoExecute)) | Present | Write;

    if (!pmm::is_shared(source)) {
        // Everyone else has already copied away; reclaim it in place.
        *pt_entry = source | flags;
        invalidate(virt);
        return true;
    }

    const paddr_t copy = pmm::alloc();
    if (copy == 0)
        return false;
    memcpy(phys_ptr(copy), phys_ptr(source), pmm::kPageSize);

    *pt_entry = copy | flags;
    invalidate(virt);
    pmm::release(source);
    return true;
}

bool unmap(vaddr_t virt)
{
    return unmap_into(current_pml4(), virt);
}

paddr_t translate(vaddr_t virt)
{
    return translate_into(current_pml4(), virt);
}

bool map_range(vaddr_t virt, paddr_t phys, usize bytes, u64 flags)
{
    const usize pages = (bytes + kPageSize - 1) / kPageSize;
    for (usize i = 0; i < pages; ++i) {
        if (!map(virt + i * kPageSize, phys + i * kPageSize, flags))
            return false;
    }
    return true;
}

bool map_mmio(vaddr_t virt, paddr_t phys, usize bytes)
{
    return map_range(virt, phys, bytes, Write | NoCache | WriteThrough | NoExecute);
}

AddressSpace kernel_space()  { return g_pml4_phys; }
AddressSpace current_space() { return g_current_pml4; }

AddressSpace create_address_space()
{
    const paddr_t space_phys = alloc_table();
    if (space_phys == 0)
        return 0;

    // Copy every one of the kernel's top-level entries. They point at the
    // kernel's own sub-tables (the direct map, the heap, the kernel image, all
    // in the high half), so those are shared into this space by reference. The
    // low half is empty in the kernel PML4, so it stays entirely the process's.
    u64* space  = phys_ptr(space_phys);
    u64* kernel = kernel_pml4();
    for (u64 i = 0; i < kEntriesPerTable; ++i)
        space[i] = kernel[i];

    return space_phys;
}

AddressSpace fork_address_space(AddressSpace parent)
{
    const AddressSpace child = create_address_space();
    if (child == 0)
        return 0;

    u64* child_pml4  = phys_ptr(child);
    u64* parent_pml4 = phys_ptr(parent);
    u64* kernel      = kernel_pml4();

    // Only the slots the parent added over the kernel are the process's own
    // memory; the rest are shared kernel mappings already in the child.
    for (u64 m = 0; m < kEntriesPerTable; ++m) {
        if ((parent_pml4[m] & Present) == 0 || parent_pml4[m] == kernel[m])
            continue;

        u64* pdpt = table_of(parent_pml4[m]);
        for (u64 p = 0; p < kEntriesPerTable; ++p) {
            if ((pdpt[p] & Present) == 0)
                continue;
            u64* pd = table_of(pdpt[p]);
            for (u64 d = 0; d < kEntriesPerTable; ++d) {
                if ((pd[d] & Present) == 0 || (pd[d] & Huge) != 0)
                    continue;                       // no user huge pages exist
                u64* pt = table_of(pd[d]);
                for (u64 t = 0; t < kEntriesPerTable; ++t) {
                    const u64 entry = pt[t];
                    if ((entry & Present) == 0)
                        continue;

                    const vaddr_t virt = m << 39 | p << 30 | d << 21 | t << 12;
                    const u64 flags = entry & (Write | User | NoExecute);
                    const paddr_t source = entry & kAddressMask;

                    // Share the frame instead of copying it: both sides lose
                    // write permission and gain the CoW mark, so the first write
                    // from either faults and gets its own private copy.
                    //
                    // "Was it writable?" is not the right question on its own. A
                    // page this process already shares from an earlier fork is
                    // read-only *and* CoW, and must stay CoW here - forking a
                    // second time would otherwise hand the new child a plainly
                    // read-only page whose first write faults with nothing to
                    // resolve it. Only a page that is read-only and not CoW is
                    // genuinely read-only, and can be shared as it stands.
                    const bool needs_cow =
                        (entry & Write) != 0 || (entry & CopyOnWrite) != 0;

                    if (pmm::share(source)) {
                        const u64 shared_flags = needs_cow
                            ? (flags & ~static_cast<u64>(Write)) | CopyOnWrite
                            : flags;
                        if (!map_into(child_pml4, virt, source, shared_flags)) {
                            pmm::release(source);
                            destroy_address_space(child);
                            return 0;
                        }
                        if (needs_cow) {
                            // The parent has to fault too, or it would write
                            // through to a page the child can see.
                            pt[t] = (entry & ~static_cast<u64>(Write)) | CopyOnWrite;
                            invalidate(virt);
                        }
                        continue;
                    }

                    // No reference left to hand out (the table is full or
                    // absent): fall back to an eager copy, which is always
                    // correct, just slower.
                    const paddr_t frame = pmm::alloc();
                    if (frame == 0) {
                        destroy_address_space(child);
                        return 0;
                    }
                    memcpy(phys_ptr(frame), phys_ptr(source), kPageSize);

                    if (!map_into(child_pml4, virt, frame, flags)) {
                        pmm::free(frame);
                        destroy_address_space(child);
                        return 0;
                    }
                }
            }
        }
    }

    return child;
}

void destroy_address_space(AddressSpace space)
{
    if (space == 0 || space == g_pml4_phys)
        return;

    // Never free while it is the active table; the caller switches away first.
    if (space == g_current_pml4)
        switch_address_space(g_pml4_phys);

    u64* pml4 = phys_ptr(space);
    u64* kernel = kernel_pml4();

    // Only the slots this space added - the ones the kernel does not share -
    // are ours to free. Freeing a shared kernel sub-tree would unmap the kernel
    // out from under every other space.
    for (u64 i = 0; i < kEntriesPerTable; ++i) {
        if ((pml4[i] & Present) == 0)
            continue;
        if (pml4[i] == kernel[i])
            continue;                       // shared with the kernel, leave it
        free_table_tree(pml4[i] & kAddressMask, 3);
    }

    pmm::free(space);
}

void switch_address_space(AddressSpace space)
{
    if (space == 0 || space == g_current_pml4)
        return;
    g_current_pml4 = space;
    asm volatile("mov %0, %%cr3" : : "r"(space) : "memory");
}

paddr_t kernel_page_table() { return g_pml4_phys; }

} // namespace vmm
