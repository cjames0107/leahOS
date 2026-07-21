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

u64 g_pml4_phys = 0;

// Everything here dereferences physical addresses directly. That is only legal
// because the kernel identity maps all of RAM; the day this moves to the higher
// half, every one of these needs to go through the direct map offset instead.
u64* table_of(u64 entry)
{
    return reinterpret_cast<u64*>(entry & kAddressMask);
}

u64 index_of(vaddr_t virt, int level)   // level 4 = PML4 ... 1 = PT
{
    return virt >> (12 + 9 * (level - 1)) & 0x1FF;
}

void invalidate(vaddr_t virt)
{
    asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

u64* alloc_table()
{
    const paddr_t frame = pmm::alloc();
    if (frame == 0)
        return nullptr;
    auto* table = reinterpret_cast<u64*>(frame);
    memset(table, 0, pmm::kPageSize);
    return table;
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
        u64* fresh = alloc_table();
        if (fresh == nullptr)
            return nullptr;
        table[index] = reinterpret_cast<u64>(fresh) | Present | Write | User;
        return fresh;
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

    u64* pt = alloc_table();
    if (pt == nullptr)
        return false;

    for (u64 i = 0; i < kEntriesPerTable; ++i)
        pt[i] = (base + i * kPageSize) | flags;

    pd[index] = reinterpret_cast<u64>(pt) | Present | Write | User;
    return true;
}

} // namespace

void init()
{
    // NXE has to be on before any entry sets bit 63, or the CPU faults on a
    // reserved bit rather than honouring the flag.
    constexpr u32 kIa32Efer = 0xC0000080;
    cpu::write_msr(kIa32Efer, cpu::read_msr(kIa32Efer) | (1ull << 11));

    u64* pml4 = alloc_table();
    if (pml4 == nullptr)
        panic("vmm: cannot allocate PML4");
    g_pml4_phys = reinterpret_cast<u64>(pml4);

    // Identity map the low 4 GiB with 2 MiB pages. Covers all of RAM on the
    // machines we target plus the MMIO window below 4 GiB where the LAPIC, PCI
    // BARs and any framebuffer live - so device mappings work before the
    // drivers that need them exist.
    const u64 identity_limit = 4ull * 1024 * 1024 * 1024;
    for (u64 addr = 0; addr < identity_limit; addr += kHugePageSize) {
        u64* pdpt = next_level(pml4, index_of(addr, 4), true);
        if (pdpt == nullptr)
            panic("vmm: out of memory building the identity map");
        u64* pd = next_level(pdpt, index_of(addr, 3), true);
        if (pd == nullptr)
            panic("vmm: out of memory building the identity map");

        pd[index_of(addr, 2)] = addr | Present | Write | Huge;
    }

    // Anything above 4 GiB that E820 called usable still needs to be reachable,
    // since the frame allocator will happily hand it out. Deliberately bounded
    // by the highest *usable* address, not the highest address described: E820
    // routinely reports reserved regions near the top of the 64-bit space, and
    // spanning the hole to reach them would burn megabytes of page tables
    // describing memory that does not exist.
    const u64 top = pmm::highest_usable();
    for (u64 addr = identity_limit; addr < top; addr += kHugePageSize) {
        u64* pdpt = next_level(pml4, index_of(addr, 4), true);
        u64* pd = pdpt != nullptr ? next_level(pdpt, index_of(addr, 3), true) : nullptr;
        if (pd == nullptr)
            break;      // out of memory for tables; the low 4 GiB still works
        pd[index_of(addr, 2)] = addr | Present | Write | Huge;
    }

    // The kernel is executing from the higher half right now, so the new
    // tables must describe it before CR3 is loaded - otherwise the very next
    // instruction fetch faults with no handler mapped to catch it.
    for (u64 offset = 0; offset < memory::kKernelWindowSize; offset += kHugePageSize) {
        const vaddr_t virt = memory::kKernelBase + offset;
        u64* pdpt = next_level(pml4, index_of(virt, 4), true);
        if (pdpt == nullptr)
            panic("vmm: out of memory mapping the kernel window");
        u64* pd = next_level(pdpt, index_of(virt, 3), true);
        if (pd == nullptr)
            panic("vmm: out of memory mapping the kernel window");

        pd[index_of(virt, 2)] = offset | Present | Write | Huge;
    }

    asm volatile("mov %0, %%cr3" : : "r"(g_pml4_phys) : "memory");
}

bool map(vaddr_t virt, paddr_t phys, u64 flags)
{
    if (g_pml4_phys == 0)
        return false;

    auto* pml4 = reinterpret_cast<u64*>(g_pml4_phys);

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

bool unmap(vaddr_t virt)
{
    auto* pml4 = reinterpret_cast<u64*>(g_pml4_phys);

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

paddr_t translate(vaddr_t virt)
{
    auto* pml4 = reinterpret_cast<u64*>(g_pml4_phys);

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

paddr_t kernel_page_table() { return g_pml4_phys; }

} // namespace vmm
