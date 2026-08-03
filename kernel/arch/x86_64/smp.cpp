#include <leah/acpi.hpp>
#include <leah/apic.hpp>
#include <leah/cpu.hpp>
#include <leah/fpu.hpp>
#include <leah/gdt.hpp>
#include <leah/heap.hpp>
#include <leah/interrupts.hpp>
#include <leah/memory.hpp>
#include <leah/percpu.hpp>
#include <leah/scheduler.hpp>
#include <leah/smp.hpp>
#include <leah/syscall.hpp>
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
volatile bool g_scheduling = false;
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

    // Floating point is a per-processor setting: CR0 and CR4 are not shared,
    // and an AP that skips this faults with #UD on the first SSE instruction
    // any task executes there - which is to say, almost immediately.
    fpu::init_this_cpu();

    // Its own local APIC still has to be switched on; only the bootstrap
    // processor's was touched during boot.
    apic::init_this_cpu();

    // Everything else that lives in this processor's own registers rather than
    // in shared memory: the paging control bits, and the MSRs that make
    // SYSCALL legal and point it at the kernel.
    vmm::init_this_cpu();
    syscall::init_this_cpu();

    __atomic_add_fetch(&g_online, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_handshake, 1, __ATOMIC_SEQ_CST);

    // Its own tick, so this processor preempts on its own rather than waiting
    // for the bootstrap one to notice.
    apic::start_timer(interrupts::kIrqBase + interrupts::kIrqTimer,
                      timer::kFrequencyHz);

    // An idle task of its own, adopted before anything can switch away: until
    // that happens current_index() still names task 0, and the first context
    // switch would save this CPU's state straight over the bootstrap
    // processor's.
    scheduler::start_idle_for(slot);
    scheduler::enter_scheduler_on_this_cpu();
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
    if (g_cpu_count > 1)
        __atomic_store_n(&g_scheduling, true, __ATOMIC_SEQ_CST);
    return g_cpu_count;
}

usize cpu_count() { return g_cpu_count; }

bool multiprocessor() { return g_cpu_count > 1; }

// True once the application processors are running tasks. A shootdown has to
// be gated on this rather than on the CPU count: a parked processor halted with
// interrupts off can never acknowledge one, so counting cores instead made
// every unmap wait out the full timeout.
bool scheduling() { return __atomic_load_n(&g_scheduling, __ATOMIC_SEQ_CST); }

} // namespace smp
