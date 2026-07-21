#pragma once

#include <leah/types.hpp>

// The 8253/8254 PIT. Coarse and ancient, but it is the one timer that needs no
// discovery - no ACPI tables, no calibration, no APIC. Good enough to be the
// kernel's first sense of time passing.

namespace timer {

constexpr u32 kFrequencyHz = 100;    // 10 ms per tick

void init(u32 frequency_hz = kFrequencyHz);

u64 ticks();
u64 uptime_ms();

// Spin until the requested time has passed. Only safe with interrupts on, and
// only appropriate before there is a scheduler to yield to.
void sleep_ms(u64 milliseconds);

} // namespace timer
