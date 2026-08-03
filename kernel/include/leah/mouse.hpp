#pragma once

#include <leah/types.hpp>

// PS/2 mouse on the 8042's second port, IRQ 12.

namespace mouse {

struct State {
    i32  x;
    i32  y;
    bool left;
    bool right;
    bool middle;
};

void init();

State state();

// Where the pointer is, as reported by whichever ring 3 driver owns the
// device. Clamped here, because the bound is the framebuffer's and the
// framebuffer is still the kernel's.
void set_state(i32 x, i32 y, u32 buttons);

// Total packets decoded - the cheapest way to tell whether the device is
// actually reporting.
u64 packet_count();

} // namespace mouse
