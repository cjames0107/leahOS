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

// Valid while the kernel lock is held; see the header.
volatile u32 g_active_slot = 0;

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

    // While the kernel runs, GS points at this block. The user's GS lives in
    // KERNEL_GS_BASE and the two are exchanged by SWAPGS on entry and exit, so
    // kernel code can always read gs: without asking where it came from.
    g_slot_by_apic[apic_id & 0xFF] = static_cast<u8>(slot);

    // GS carries the block for a future SYSCALL stub that reaches it through
    // SWAPGS. Nothing reads it that way yet - see set_syscall_stack below.
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

void set_active(u32 s) { g_active_slot = s; }
u32  active()          { return g_active_slot; }

Cpu& current() { return g_cpus[active()]; }

// The stack SYSCALL lands on. This is still a single global inside
// syscall_entry.asm, which is correct only because one CPU runs user tasks: the
// stub has no free register on entry (RSP still points into user memory), so
// reaching a per-CPU block needs SWAPGS - and SWAPGS is only sound once *every*
// entry from ring 3 does it, interrupts included, and every exit undoes it. Half
// of that discipline is worse than none: a task first entered through IRETQ
// inherits the kernel's GS, and its next syscall swaps in a zero base and writes
// to address 0x10. Doing it properly is the prerequisite for an application
// processor running user code.
extern "C" void set_syscall_stack(u64 rsp);

void set_syscall_stack_for_this_cpu(u64 rsp)
{
    g_cpus[active()].syscall_stack = rsp;
    set_syscall_stack(rsp);
}

} // namespace percpu
