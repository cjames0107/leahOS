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

struct [[gnu::packed]] Tss {
    u32 reserved0;
    u64 rsp[3];         // stack per privilege level; rsp[0] is the ring 0 stack
    u64 reserved1;
    u64 ist[7];
    u64 reserved2;
    u16 reserved3;
    u16 iomap_base;
};

static_assert(sizeof(Tss) == 104, "64-bit TSS is 104 bytes");

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

alignas(16) Table  g_gdt{};
alignas(16) Tss    g_tss{};

// Dedicated stacks for the traps that must not touch the interrupted stack.
//
// A double fault most often means the kernel stack overflowed. If #DF ran on
// that same broken stack, its own pushes would fault again - and a fault while
// handling a double fault is a triple fault, which the CPU answers by resetting
// the machine with no diagnostic whatsoever. Routing #DF through IST1 gives it
// known-good ground to stand on, turning the worst failure mode in the kernel
// into a readable panic.
alignas(16) u8 g_double_fault_stack[16 * 1024];
alignas(16) u8 g_kernel_stack[16 * 1024];

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

void init()
{
    g_gdt.null        = Entry{};
    g_gdt.kernel_code = make_entry(0x9A, 0xAF);
    g_gdt.kernel_data = make_entry(0x92, 0xCF);
    g_gdt.user_data   = make_entry(0xF2, 0xCF);
    g_gdt.user_code   = make_entry(0xFA, 0xAF);

    memset(&g_tss, 0, sizeof(g_tss));
    g_tss.ist[kIstDoubleFault - 1] =
        reinterpret_cast<u64>(g_double_fault_stack) + sizeof(g_double_fault_stack);
    g_tss.rsp[0] =
        reinterpret_cast<u64>(g_kernel_stack) + sizeof(g_kernel_stack);

    // No I/O permission bitmap. Setting the base past the TSS limit is the
    // documented way to say "ring 3 may not touch any port".
    g_tss.iomap_base = sizeof(Tss);

    const u64 tss_base = reinterpret_cast<u64>(&g_tss);
    g_gdt.tss = TssEntry{
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
        .limit = sizeof(g_gdt) - 1,
        .base  = reinterpret_cast<u64>(&g_gdt),
    };

    load(pointer);
    asm volatile("ltr %0" : : "r"(kTss));
}

void set_kernel_stack(u64 rsp0)
{
    g_tss.rsp[0] = rsp0;
}

} // namespace gdt
