#include <leah/acpi.hpp>
#include <leah/apic.hpp>
#include <leah/cpu.hpp>
#include <leah/gdt.hpp>
#include <leah/heap.hpp>
#include <leah/interrupts.hpp>
#include <leah/memory.hpp>
#include <leah/percpu.hpp>
#include <leah/scheduler.hpp>
#include <leah/smp.hpp>
#include <leah/spinlock.hpp>
#include <leah/timer.hpp>
#include <leah/string.hpp>
#include <leah/vmm.hpp>

extern "C" const u8 ap_trampoline_blob[];
extern "C" const u8 ap_trampoline_blob_end[];

namespace smp {
namespace {

// Where the trampoline is copied. It has to be page-aligned and below 1 MiB,
// because a startup IPI carries a page number rather than an address. The whole
// first megabyte is reserved from the frame allocator, and stage 2 - whose load
// address this was - finished with it long ago.
constexpr paddr_t kTrampolinePhys = 0x8000;
constexpr u8      kTrampolinePage = kTrampolinePhys >> 12;

// Offsets of the parameter block at the end of the trampoline page; they must
// agree with boot/ap_trampoline.asm.
constexpr u64 kParamEntry = 0xFE8;
constexpr u64 kParamStack = 0xFF0;
constexpr u64 kParamCr3   = 0xFF8;

constexpr usize kApStackSize = 16 * 1024;

// Written by whichever AP is currently coming up, read by the bootstrap
// processor spinning on it. Volatile because the two are genuinely different
// CPUs, not two threads the compiler knows about.
volatile u32 g_online = 1;              // the bootstrap processor counts itself
volatile u32 g_handshake = 0;
u32 g_next_slot = 0;                    // slot 0 is the bootstrap processor

usize g_cpu_count = 1;

u8* trampoline_ptr()
{
    return reinterpret_cast<u8*>(memory::phys_to_direct(kTrampolinePhys));
}

void write_param(u64 offset, u64 value)
{
    *reinterpret_cast<volatile u64*>(trampoline_ptr() + offset) = value;
}

} // namespace

// Where the trampoline's last instruction jumps. This runs on the application
// processor, on its own stack, with the kernel's page tables already loaded.
extern "C" [[noreturn]] void ap_main()
{
    // The GDT and IDT are shared - a descriptor table is read-only to the CPU,
    // so every processor can point at the same one. What is *not* shared is the
    // TSS, and this AP never needs one: it takes no interrupts and makes no
    // ring transition. That changes the moment it runs user tasks.
    // Its own GDT and TSS, then its own per-CPU block: the order matters,
    // because loading a data selector into GS resets the base the block needs.
    const u32 slot = __atomic_add_fetch(&g_next_slot, 1, __ATOMIC_SEQ_CST);
    gdt::init_cpu(slot);
    interrupts::load_on_this_cpu();
    percpu::init(slot, apic::local_id());

    // Its own local APIC still has to be switched on; only the bootstrap
    // processor's was touched during boot.
    apic::init_this_cpu();

    __atomic_add_fetch(&g_online, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_handshake, 1, __ATOMIC_SEQ_CST);

    // Parked, still. Everything an AP needs to *run* is now in place - its own
    // GDT and TSS, a per-CPU slot, a kernel lock that hands off across context
    // switches, TLB shootdown - and four processors do interleave real work when
    // this is switched on. What is not yet right is the handoff between that
    // lock and the idle loop: the deadlock it produces moves when timing
    // changes, which is exactly the class of bug that must not be shipped.
    // scheduler::enter_scheduler_on_this_cpu() is the one line to re-enable.
    for (;;)
        asm volatile("cli; hlt");
}

usize init()
{
    if (!apic::available() || acpi::cpu_count() <= 1)
        return 1;

    const usize blob_size =
        static_cast<usize>(ap_trampoline_blob_end - ap_trampoline_blob);
    if (blob_size == 0 || blob_size > vmm::kPageSize)
        return 1;

    // The kernel's tables map nothing in the low half, so the trampoline's own
    // instructions would fault the moment it enables paging. Identity-map the
    // page for the duration of startup.
    if (!vmm::map(kTrampolinePhys, kTrampolinePhys, vmm::Present | vmm::Write))
        return 1;

    memcpy(trampoline_ptr(), ap_trampoline_blob, blob_size);
    write_param(kParamCr3, vmm::kernel_page_table());
    write_param(kParamEntry, reinterpret_cast<u64>(&ap_main));

    const u8 self = apic::local_id();
    for (usize i = 0; i < acpi::cpu_count(); ++i) {
        const u8 target = acpi::cpu_apic_id(i);
        if (target == self)
            continue;

        auto* stack = static_cast<u8*>(kmalloc(kApStackSize));
        if (stack == nullptr)
            break;
        // Each AP gets its own stack, and they are started one at a time, so a
        // single parameter block is enough.
        write_param(kParamStack, reinterpret_cast<u64>(stack) + kApStackSize);
        __atomic_store_n(&g_handshake, 0, __ATOMIC_SEQ_CST);

        // INIT resets the target; the startup IPI then tells it which page to
        // begin executing. The second SIPI is not superstition - the spec calls
        // for it, and some parts genuinely miss the first.
        apic::send_init(target);
        apic::delay_us(10000);
        apic::send_startup(target, kTrampolinePage);
        apic::delay_us(200);
        if (__atomic_load_n(&g_handshake, __ATOMIC_SEQ_CST) == 0) {
            apic::send_startup(target, kTrampolinePage);
            apic::delay_us(200);
        }

        // Give it a moment to arrive before moving on; a CPU that never checks
        // in is skipped rather than fatal.
        for (int spin = 0; spin < 100; ++spin) {
            if (__atomic_load_n(&g_handshake, __ATOMIC_SEQ_CST) != 0)
                break;
            apic::delay_us(1000);
        }
        if (__atomic_load_n(&g_handshake, __ATOMIC_SEQ_CST) == 0)
            kfree(stack);           // never started; the stack is unused
    }

    // Not just the leaf: the tables above it must go too, or every address
    // space created later would inherit them from the kernel's PML4.
    vmm::release_low_half();
    g_cpu_count = __atomic_load_n(&g_online, __ATOMIC_SEQ_CST);
    return g_cpu_count;
}

usize cpu_count() { return g_cpu_count; }

bool multiprocessor() { return g_cpu_count > 1; }

// Flipped on when the application processors join the scheduler. They do not
// yet - see the comment in ap_main - so this is false and shootdowns stay off.
bool scheduling() { return false; }

} // namespace smp
