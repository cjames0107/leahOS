#include <leah/gdt.hpp>
#include <leah/string.hpp>

namespace gdt {
namespace {

struct [[gnu::packed]] Entry {
    u16 limit_low;
    u16 base_low;
    u8  base_mid;
    u8  access;
    u8  granularity;    // high limit nibble + flags (G, D/B, L, AVL)
    u8  base_high;
};

// A TSS descriptor is twice the width of a normal one in long mode: the base
// grew to 64 bits and had to go somewhere.
struct [[gnu::packed]] TssEntry {
    u16 limit_low;
    u16 base_low;
    u8  base_mid;
    u8  access;
    u8  granularity;
    u8  base_high;
    u32 base_upper;
    u32 reserved;
};

// The I/O permission bitmap: one bit per port, and a set bit means *denied*.
// 65536 ports is 8 KiB, plus the terminating 0xFF the CPU reads past the end.
//
// This is how a driver in ring 3 is given the ports it needs and no others.
// Rings 1 and 2 would look like the obvious answer and are not one: paging has
// a single privilege bit, so anything below ring 3 is supervisor and can reach
// all of kernel memory. A ring number would have named the privilege without
// enforcing anything. This bitmap is checked by the hardware on every IN and
// OUT, which is the difference.
constexpr usize kIoPorts     = 65536;
constexpr usize kIoMapBytes  = kIoPorts / 8;

struct [[gnu::packed]] Tss {
    u32 reserved0;
    u64 rsp[3];         // stack per privilege level; rsp[0] is the ring 0 stack
    u64 reserved1;
    u64 ist[7];
    u64 reserved2;
    u16 reserved3;
    u16 iomap_base;
    u8  iomap[kIoMapBytes];
    u8  iomap_end;      // the CPU reads one byte past the last port
};

static_assert(__builtin_offsetof(Tss, iomap) == 104, "the bitmap follows the 104-byte TSS");

struct [[gnu::packed]] Table {
    Entry    null;
    Entry    kernel_code;
    Entry    kernel_data;
    Entry    user_data;
    Entry    user_code;
    TssEntry tss;
};

struct [[gnu::packed]] Pointer {
    u16 limit;
    u64 base;
};

// Access byte:  P DPL DPL S E DC RW A
//   0x9A kernel code (present, ring 0, code, readable)
//   0x92 kernel data (present, ring 0, data, writable)
//   0xFA user code   (present, ring 3, code, readable)
//   0xF2 user data   (present, ring 3, data, writable)
//   0x89 TSS         (present, ring 0, available 64-bit TSS)
//
// Granularity byte: G D/B L AVL + limit[19:16]
//   0xAF  4 KiB granular, L=1  -> 64-bit code
//   0xCF  4 KiB granular, D/B=1 -> 32-bit data (base/limit ignored in 64-bit)
constexpr Entry make_entry(u8 access, u8 granularity)
{
    return Entry{
        .limit_low   = 0xFFFF,
        .base_low    = 0,
        .base_mid    = 0,
        .access      = access,
        .granularity = granularity,
        .base_high   = 0,
    };
}

// One GDT and TSS per processor. The descriptors themselves are identical, but
// the TSS holds this CPU's ring-0 stack pointer, so it cannot be shared: two
// cores taking a ring transition at once would land on the same stack.
constexpr usize kMaxCpus = 32;

// A base past the segment limit is how the manual spells "this task may touch
// no ports at all", and it is the state every task is in but a driver.
constexpr u16 kNoIoMap = 0xFFFF;

alignas(16) Table  g_gdt[kMaxCpus]{};
alignas(16) Tss    g_tss[kMaxCpus]{};

// Dedicated stacks for the traps that must not touch the interrupted stack.
//
// A double fault most often means the kernel stack overflowed. If #DF ran on
// that same broken stack, its own pushes would fault again - and a fault while
// handling a double fault is a triple fault, which the CPU answers by resetting
// the machine with no diagnostic whatsoever. Routing #DF through IST1 gives it
// known-good ground to stand on, turning the worst failure mode in the kernel
// into a readable panic.
alignas(16) u8 g_double_fault_stack[kMaxCpus][16 * 1024];
alignas(16) u8 g_kernel_stack[kMaxCpus][16 * 1024];

// Which slot the running CPU owns. Set by init_cpu before anything reads it,
// and read through GS so each core sees its own.
u32 g_boot_cpu_slot = 0;

void load(const Pointer& pointer)
{
    // Long mode has no far jump to an immediate, so CS is reloaded by faking a
    // far return: push the target selector and address, then lretq into it.
    asm volatile(
        "lgdt %0                     \n"
        "pushq %1                    \n"
        "leaq 1f(%%rip), %%rax       \n"
        "pushq %%rax                 \n"
        "lretq                       \n"
        "1:                          \n"
        "mov %2, %%ax                \n"
        "mov %%ax, %%ds              \n"
        "mov %%ax, %%es              \n"
        "mov %%ax, %%fs              \n"
        "mov %%ax, %%gs              \n"
        "mov %%ax, %%ss              \n"
        :
        : "m"(pointer), "i"(kKernelCode), "i"(static_cast<u32>(kKernelData))
        : "rax", "memory");
}

} // namespace

void init() { init_cpu(0); }

void init_cpu(u32 slot)
{
    if (slot >= kMaxCpus)
        return;
    g_boot_cpu_slot = slot;

    Table& gdt = g_gdt[slot];
    Tss&   tss = g_tss[slot];

    gdt.null        = Entry{};
    gdt.kernel_code = make_entry(0x9A, 0xAF);
    gdt.kernel_data = make_entry(0x92, 0xCF);
    gdt.user_data   = make_entry(0xF2, 0xCF);
    gdt.user_code   = make_entry(0xFA, 0xAF);

    memset(&tss, 0, sizeof(tss));
    tss.ist[kIstDoubleFault - 1] = reinterpret_cast<u64>(g_double_fault_stack[slot]) +
                                   sizeof(g_double_fault_stack[slot]);
    tss.rsp[0] = reinterpret_cast<u64>(g_kernel_stack[slot]) +
                 sizeof(g_kernel_stack[slot]);

    // Deny every port by default. The base is left past the limit, which the
    // manual defines as "no bitmap" and the CPU treats as all-denied; the
    // bitmap itself is filled in only when a task that has been granted ports
    // is switched to.
    memset(tss.iomap, 0xFF, sizeof(tss.iomap));
    tss.iomap_end = 0xFF;

    // No I/O permission bitmap. Setting the base past the TSS limit is the
    // documented way to say "ring 3 may not touch any port".
    tss.iomap_base = kNoIoMap;

    const u64 tss_base = reinterpret_cast<u64>(&tss);
    gdt.tss = TssEntry{
        .limit_low   = sizeof(Tss) - 1,
        .base_low    = static_cast<u16>(tss_base & 0xFFFF),
        .base_mid    = static_cast<u8>(tss_base >> 16 & 0xFF),
        .access      = 0x89,
        .granularity = 0x00,
        .base_high   = static_cast<u8>(tss_base >> 24 & 0xFF),
        .base_upper  = static_cast<u32>(tss_base >> 32),
        .reserved    = 0,
    };

    const Pointer pointer{
        .limit = sizeof(Table) - 1,
        .base  = reinterpret_cast<u64>(&gdt),
    };

    load(pointer);
    asm volatile("ltr %0" : : "r"(kTss));
}

/* Install a task's port permissions before it runs.
 *
 * Passing nullptr denies everything, which is the common case and costs one
 * store. A driver costs a copy of the whole bitmap, which is 8 KiB - paid only
 * on a switch to a task that actually has ports, of which this system has a
 * handful. The alternative, a TSS per task, would cost the same 8 KiB per task
 * permanently and a GDT reload on every switch. */
void set_io_bitmap(u32 slot, const u8* bitmap)
{
    if (slot >= kMaxCpus)
        return;
    Tss& tss = g_tss[slot];
    if (bitmap == nullptr) {
        tss.iomap_base = kNoIoMap;
        return;
    }
    memcpy(tss.iomap, bitmap, sizeof(tss.iomap));
    tss.iomap_end  = 0xFF;
    tss.iomap_base = static_cast<u16>(__builtin_offsetof(Tss, iomap));
}

usize io_bitmap_bytes() { return kIoMapBytes; }

void set_kernel_stack(u32 slot, u64 rsp0)
{
    if (slot < kMaxCpus)
        g_tss[slot].rsp[0] = rsp0;
}

} // namespace gdt
