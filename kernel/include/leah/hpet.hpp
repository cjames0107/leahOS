#pragma once

#include <leah/types.hpp>

// The High Precision Event Timer: a free-running counter of known frequency,
// which is exactly what the PIT is not. Nothing here programs its comparators -
// the local APIC timer raises the scheduling tick - but a monotonic counter the
// kernel can trust is what lets that timer be calibrated at all, and it gives
// uptime a resolution the PIT's 100 Hz never could.

namespace hpet {

// Map and start the counter, using the address ACPI reported. False when the
// machine has no HPET.
bool init();
bool available();

// Femtoseconds per tick, straight from the capability register.
u32 period_fs();
u64 frequency_hz();

// The main counter. Monotonic from init(), and wide enough not to wrap in any
// timeframe that matters.
u64 counter();

u64 uptime_us();

// Spin until `microseconds` have passed. Used during calibration, where there
// is nothing to yield to yet.
void busy_wait_us(u64 microseconds);

} // namespace hpet
