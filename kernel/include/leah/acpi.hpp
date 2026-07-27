#pragma once

#include <leah/types.hpp>

// ACPI table discovery: enough of it to find the interrupt controllers and the
// HPET, and no more. There is no AML interpreter here and there does not need
// to be - the tables this reads are plain structures, and everything that would
// require executing bytecode (power management, device enumeration beyond PCI)
// is out of scope.
//
// The BIOS leaves a Root System Description Pointer in low memory; it names a
// table of tables, and the ones worth having are the MADT ("APIC"), which
// describes every local and I/O APIC in the machine, and the HPET.

namespace acpi {

// One I/O APIC: where its registers live and which global system interrupt its
// first input line corresponds to.
struct IoApic {
    u8  id;
    u32 address;        // physical
    u32 gsi_base;
};

// The MADT can say that a legacy ISA IRQ is wired to a different global system
// interrupt than its number suggests - almost universally that the PIT's IRQ 0
// arrives as GSI 2. Ignoring these is the classic reason a freshly enabled I/O
// APIC delivers nothing.
struct SourceOverride {
    u8  source;         // the ISA IRQ number
    u32 gsi;
    u16 flags;          // polarity in bits 1:0, trigger mode in bits 3:2
};

constexpr usize kMaxIoApics   = 4;
constexpr usize kMaxOverrides = 16;
constexpr usize kMaxCpus      = 32;

// Walk the ACPI tables. Returns false when no valid RSDP is found, in which
// case the caller keeps the PIC and PIT.
bool init();
bool available();

// --- what the MADT said -----------------------------------------------------

u64 local_apic_address();
usize io_apic_count();
const IoApic& io_apic_at(usize index);

usize override_count();
const SourceOverride& override_at(usize index);

// Local APIC ids of every enabled processor, in MADT order. The first is the
// bootstrap processor. This is what SMP startup will need.
usize cpu_count();
u8 cpu_apic_id(usize index);

// The HPET's register block, or 0 when the machine has no HPET table.
u64 hpet_address();

// Table signatures found, for the boot report.
usize table_count();

} // namespace acpi
