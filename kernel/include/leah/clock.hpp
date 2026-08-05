#pragma once

#include <leah/types.hpp>

// What time it is, as opposed to how long we have been running.
//
// The kernel already knows the second of those - the timer counts ticks and
// the HPET counts microseconds - and neither says anything about the date.
// Until this existed the system had no idea: every file on the disk had a
// timestamp of zero, `ls` could not show when anything was written, and the
// application in the dock called Clock was honestly titled Uptime and drew a
// bar from a tick counter.
//
// The wall clock is read once, from the CMOS real-time clock, during boot.
// After that the time is that reading plus however long the machine has been
// up, which is what every operating system does: the RTC is slow to read (a
// handful of port accesses, each one a trap out of the guest under emulation)
// and asking it on every timestamp would be absurd.
//
// This lives in the kernel rather than in a ring-3 driver, which is a
// departure from the rule that drivers are processes. The reason is that the
// answer is needed before there is a ring 3 to run one in - the filesystem
// server timestamps its first write, and it is started by the kernel - and
// because POSIX time is the same kind of personality as fork and signals,
// which this kernel already keeps. It is four port reads at boot and an
// addition thereafter, not a driver.

namespace clock {

// Seconds and nanoseconds since the start of 1970, the way POSIX counts.
struct Time {
    i64 seconds;
    u32 nanoseconds;
};

// Read the CMOS clock and take the current uptime as the origin. Called once,
// during boot, after the timer exists.
void init();

// Now. Monotonic in practice - it is an offset plus the uptime counter - so it
// never goes backwards even though it is called a wall clock.
Time now();

// Just the seconds, which is what almost every caller wants.
i64 now_seconds();

// Whether the CMOS held something believable. When it did not, the clock reads
// from zero, which is 1970 - wrong, but wrong in a way that is obvious rather
// than a plausible date nobody questions.
bool from_hardware();

}  // namespace clock
