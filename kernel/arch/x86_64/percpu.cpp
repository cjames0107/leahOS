#include <leah/apic.hpp>
#include <leah/cpu.hpp>
#include <leah/percpu.hpp>
#include <leah/string.hpp>

namespace percpu {
namespace {

constexpr usize kMaxCpus = 32;
constexpr u32 kIa32GsBase       = 0xC0000101;
constexpr u32 kIa32KernelGsBase = 0xC0000102;

alignas(64) Cpu g_cpus[kMaxCpus]{};

// Which slot each local APIC id owns. The LAPIC id register is per-CPU in
// hardware, so reading it identifies the running core without needing a segment
// base - which matters because entering ring 3 reloads GS with a data selector
// and resets its base. Doing this through GS instead would mean SWAPGS on every
// kernel entry and exit; the table costs one uncached read and no new invariant.
u8 g_slot_by_apic[256]{};

} // namespace

void init(u32 slot, u32 apic_id)
{
    if (slot >= kMaxCpus)
        return;

    Cpu& cpu = g_cpus[slot];
    memset(&cpu, 0, sizeof(cpu));
    cpu.self    = &cpu;
    cpu.slot    = slot;
    cpu.apic_id = apic_id;
    cpu.previous_task = 0xFFFFFFFFu;    // nothing has been switched away from yet

    // While the kernel runs, GS points at this block. The user's GS lives in
    // KERNEL_GS_BASE and the two are exchanged by SWAPGS on entry and exit, so
    // kernel code can always read gs: without asking where it came from.
    g_slot_by_apic[apic_id & 0xFF] = static_cast<u8>(slot);

    // The kernel-side half of the invariant: while this CPU executes kernel
    // code GS_BASE is its own block. The other half - the user's GS parked in
    // IA32_KERNEL_GS_BASE - is established by the first exit to ring 3.
    cpu::write_msr(kIa32GsBase, reinterpret_cast<u64>(&cpu));
    cpu::write_msr(kIa32KernelGsBase, 0);
}

u32 slot()
{
    // Before the local APIC is up there is only one CPU, and it is slot 0.
    if (!apic::available())
        return 0;
    return g_slot_by_apic[apic::local_id()];
}

// Both read straight out of the segment base, so they describe the processor
// actually executing rather than whichever one last took a lock. Valid from
// percpu::init onwards, which runs before anything that can ask.
Cpu& current()
{
    Cpu* self;
    asm volatile("movq %%gs:0, %0" : "=r"(self));
    return *self;
}

u32 active()
{
    u32 slot;
    asm volatile("movl %%gs:24, %0" : "=r"(slot));
    return slot;
}

// Writes gs:[8] on the processor that calls it, so the SYSCALL stub lands on
// the running task's own kernel stack whichever CPU that task is on.
extern "C" void set_syscall_stack(u64 rsp);

void set_syscall_stack_for_this_cpu(u64 rsp)
{
    set_syscall_stack(rsp);
}

} // namespace percpu
