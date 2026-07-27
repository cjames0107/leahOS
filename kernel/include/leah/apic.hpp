#pragma once

#include <leah/types.hpp>

// The local APIC and I/O APIC, which between them replace the 8259 PIC.
//
// The PIC has two controllers, fifteen usable lines and no notion of more than
// one CPU. The APIC splits the job: an I/O APIC per chipset routes each device
// interrupt to a chosen vector on a chosen CPU, and a local APIC per core
// receives them, plus provides that core's own timer. That per-core half is
// what SMP needs; the PIC could never have provided it.

namespace apic {

// Bring up the local APIC on this CPU and every I/O APIC the MADT described,
// masking the PIC on the way. Returns false if there is no usable APIC, in
// which case the caller stays on the PIC.
bool init();
bool available();

// This CPU's local APIC id.
u8 local_id();

// Acknowledge the interrupt being serviced. Unlike the PIC there is no line
// number: the local APIC knows what it delivered.
void eoi();

// Route a legacy ISA IRQ to `vector` on the bootstrap CPU, honouring any MADT
// source override, and unmask it.
bool route_irq(u8 irq, u8 vector);

void mask_irq(u8 irq);

// --- the local APIC timer ---------------------------------------------------
//
// One per CPU, driven by the core clock rather than a shared chip, so every CPU
// can have its own scheduling tick. Its frequency is not architecturally
// known - it has to be measured against a timer that is.

// Measure the timer's frequency against the HPET (or the PIT when there is no
// HPET). Returns ticks per second, or 0 if it could not be measured.
u64 calibrate_timer();

// Start delivering a periodic interrupt on `vector` at `frequency_hz`.
bool start_timer(u8 vector, u32 frequency_hz);

u64 timer_frequency();

} // namespace apic
