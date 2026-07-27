#include <leah/acpi.hpp>
#include <leah/apic.hpp>
#include <leah/cpu.hpp>
#include <leah/hpet.hpp>
#include <leah/io.hpp>
#include <leah/memory.hpp>
#include <leah/pic.hpp>
#include <leah/vmm.hpp>

namespace apic {
namespace {

// --- local APIC registers, offsets from its base ----------------------------
constexpr u32 kLapicId        = 0x020;
constexpr u32 kLapicVersion   = 0x030;
constexpr u32 kLapicTpr       = 0x080;   // task priority: 0 accepts everything
constexpr u32 kLapicEoi       = 0x0B0;
constexpr u32 kLapicSpurious  = 0x0F0;
constexpr u32 kLapicLvtTimer  = 0x320;
constexpr u32 kLapicTimerInit = 0x380;
constexpr u32 kLapicTimerCur  = 0x390;
constexpr u32 kLapicTimerDiv  = 0x3E0;

constexpr u32 kSpuriousEnable = 1u << 8;     // switches the local APIC on
constexpr u32 kSpuriousVector = 0xFF;

constexpr u32 kLvtMasked   = 1u << 16;
constexpr u32 kLvtPeriodic = 1u << 17;

constexpr u32 kDivideBy16 = 0x3;             // the encoding is not consecutive

// --- I/O APIC ---------------------------------------------------------------
constexpr u32 kIoRegSel = 0x00;
constexpr u32 kIoWin    = 0x10;

constexpr u32 kIoApicVersionReg = 0x01;
constexpr u32 kIoApicRedirBase  = 0x10;

constexpr u64 kRedirMasked      = 1ull << 16;
constexpr u64 kRedirLevelTrigger = 1ull << 15;
constexpr u64 kRedirActiveLow   = 1ull << 13;

constexpr u32 kIa32ApicBase = 0x1B;
constexpr u64 kApicGlobalEnable = 1ull << 11;

volatile u8* g_lapic = nullptr;
volatile u8* g_ioapic = nullptr;         // only the first is programmed for now
u32 g_ioapic_gsi_base = 0;
u32 g_ioapic_lines = 0;
bool g_up = false;
u64 g_timer_hz = 0;

u32 lapic_read(u32 offset)
{
    return *reinterpret_cast<volatile u32*>(g_lapic + offset);
}

void lapic_write(u32 offset, u32 value)
{
    *reinterpret_cast<volatile u32*>(g_lapic + offset) = value;
}

u32 ioapic_read(u32 reg)
{
    *reinterpret_cast<volatile u32*>(g_ioapic + kIoRegSel) = reg;
    return *reinterpret_cast<volatile u32*>(g_ioapic + kIoWin);
}

void ioapic_write(u32 reg, u32 value)
{
    *reinterpret_cast<volatile u32*>(g_ioapic + kIoRegSel) = reg;
    *reinterpret_cast<volatile u32*>(g_ioapic + kIoWin) = value;
}

// A redirection entry is 64 bits across two consecutive 32-bit registers.
void write_redirect(u32 line, u64 entry)
{
    ioapic_write(kIoApicRedirBase + line * 2, static_cast<u32>(entry));
    ioapic_write(kIoApicRedirBase + line * 2 + 1, static_cast<u32>(entry >> 32));
}

// Translate an ISA IRQ to the global system interrupt it actually arrives on,
// and report the polarity/trigger the MADT specified. Skipping this is the
// classic reason a newly enabled I/O APIC delivers nothing: on most machines
// the timer's IRQ 0 is wired to GSI 2.
u32 resolve_gsi(u8 irq, u16& flags_out)
{
    flags_out = 0;
    for (usize i = 0; i < acpi::override_count(); ++i) {
        const acpi::SourceOverride& ov = acpi::override_at(i);
        if (ov.source == irq) {
            flags_out = ov.flags;
            return ov.gsi;
        }
    }
    return irq;
}

// Wait a known interval without the HPET and without interrupts, by polling PIT
// channel 2. Channel 2 is the one wired to the speaker rather than to IRQ 0, so
// it can be gated on and watched directly - which is exactly what is needed
// here, since the PIC is already masked by the time calibration runs. Machines
// with no HPET (and QEMU, which disables it by default) depend on this path.
void pit_busy_wait_us(u64 microseconds)
{
    constexpr u32 kPitHz = 1193182;
    constexpr u16 kChannel2 = 0x42;
    constexpr u16 kCommand  = 0x43;
    constexpr u16 kGatePort = 0x61;

    u64 count = kPitHz * microseconds / 1000000;
    if (count > 0xFFFF)
        count = 0xFFFF;                  // ~54.9 ms is one full countdown

    // Gate channel 2 on, speaker output off - we only want the counter.
    io::out8(kGatePort, (io::in8(kGatePort) & ~0x02) | 0x01);
    io::out8(kCommand, 0xB2);            // channel 2, lo/hi, mode 1, binary
    io::out8(kChannel2, static_cast<u8>(count));
    io::out8(kChannel2, static_cast<u8>(count >> 8));

    // Toggling the gate restarts the count from the value just loaded.
    const u8 gate = io::in8(kGatePort) & ~0x01;
    io::out8(kGatePort, gate);
    io::out8(kGatePort, gate | 0x01);

    // Bit 5 mirrors OUT2, which goes high when the count expires.
    while ((io::in8(kGatePort) & 0x20) == 0)
        asm volatile("pause");
}

void busy_wait_us(u64 microseconds)
{
    if (hpet::available())
        hpet::busy_wait_us(microseconds);
    else
        pit_busy_wait_us(microseconds);
}

} // namespace

bool init()
{
    if (!acpi::available() || acpi::local_apic_address() == 0)
        return false;

    if (!vmm::map_mmio(memory::kLapicMmio, acpi::local_apic_address(), 0x1000))
        return false;
    g_lapic = reinterpret_cast<volatile u8*>(memory::kLapicMmio);

    // The PIC has to go before the APIC starts delivering, or the same device
    // would raise interrupts through both controllers.
    pic::mask_all();

    // The global enable in IA32_APIC_BASE is separate from the software enable
    // in the spurious vector register; firmware usually leaves it set, but not
    // always, and clearing it is how a machine ends up with a dead APIC.
    cpu::write_msr(kIa32ApicBase, cpu::read_msr(kIa32ApicBase) | kApicGlobalEnable);

    lapic_write(kLapicTpr, 0);           // do not filter by priority
    lapic_write(kLapicSpurious, kSpuriousVector | kSpuriousEnable);

    // Map the first I/O APIC. Machines with more than one are rare outside
    // large servers, and the extra ones carry GSIs nothing here uses yet.
    if (acpi::io_apic_count() > 0) {
        const acpi::IoApic& io = acpi::io_apic_at(0);
        if (vmm::map_mmio(memory::kIoApicMmio, io.address, 0x1000)) {
            g_ioapic = reinterpret_cast<volatile u8*>(memory::kIoApicMmio);
            g_ioapic_gsi_base = io.gsi_base;
            // Bits 23:16 of the version register hold the highest input line.
            g_ioapic_lines = ((ioapic_read(kIoApicVersionReg) >> 16) & 0xFF) + 1;

            // Everything starts masked; route_irq opens the ones we want.
            for (u32 line = 0; line < g_ioapic_lines; ++line)
                write_redirect(line, kRedirMasked);
        }
    }

    g_up = true;
    return true;
}

bool available() { return g_up; }

u8 local_id()
{
    return g_lapic == nullptr ? 0 : static_cast<u8>(lapic_read(kLapicId) >> 24);
}

void eoi()
{
    if (g_lapic != nullptr)
        lapic_write(kLapicEoi, 0);
}

bool route_irq(u8 irq, u8 vector)
{
    if (g_ioapic == nullptr)
        return false;

    u16 flags = 0;
    const u32 gsi = resolve_gsi(irq, flags);
    if (gsi < g_ioapic_gsi_base || gsi - g_ioapic_gsi_base >= g_ioapic_lines)
        return false;

    u64 entry = vector;
    entry |= static_cast<u64>(local_id()) << 56;      // deliver to this CPU

    // MADT flags: polarity in bits 1:0, trigger mode in bits 3:2, where 0 means
    // "conforms to the bus" - and the ISA bus is active-high, edge-triggered.
    if ((flags & 0x3) == 0x3)
        entry |= kRedirActiveLow;
    if ((flags & 0xC) == 0xC)
        entry |= kRedirLevelTrigger;

    write_redirect(gsi - g_ioapic_gsi_base, entry);   // unmasked: no mask bit
    return true;
}

void mask_irq(u8 irq)
{
    if (g_ioapic == nullptr)
        return;
    u16 flags = 0;
    const u32 gsi = resolve_gsi(irq, flags);
    if (gsi < g_ioapic_gsi_base || gsi - g_ioapic_gsi_base >= g_ioapic_lines)
        return;
    write_redirect(gsi - g_ioapic_gsi_base, kRedirMasked);
}

u64 calibrate_timer()
{
    if (g_lapic == nullptr)
        return 0;

    lapic_write(kLapicTimerDiv, kDivideBy16);
    lapic_write(kLapicLvtTimer, kLvtMasked);          // count, do not interrupt

    // Count down from the top for a known interval and see how far it got. The
    // local APIC timer runs off the core clock, whose rate is not architectural,
    // so measuring it against a timer of known frequency is the only way.
    constexpr u64 kSampleUs = 50000;                  // 50 ms
    lapic_write(kLapicTimerInit, 0xFFFFFFFF);
    busy_wait_us(kSampleUs);
    const u32 remaining = lapic_read(kLapicTimerCur);
    lapic_write(kLapicTimerInit, 0);                  // stop

    const u64 elapsed = 0xFFFFFFFFull - remaining;
    g_timer_hz = elapsed * (1000000ull / kSampleUs) * 16;   // undo the divisor
    return g_timer_hz;
}

bool start_timer(u8 vector, u32 frequency_hz)
{
    if (g_lapic == nullptr || frequency_hz == 0)
        return false;
    if (g_timer_hz == 0 && calibrate_timer() == 0)
        return false;

    const u64 count = g_timer_hz / 16 / frequency_hz;  // the divisor again
    if (count == 0)
        return false;

    lapic_write(kLapicTimerDiv, kDivideBy16);
    lapic_write(kLapicLvtTimer, vector | kLvtPeriodic);
    lapic_write(kLapicTimerInit, static_cast<u32>(count));
    return true;
}

u64 timer_frequency() { return g_timer_hz; }

} // namespace apic
