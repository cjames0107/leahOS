#pragma once

#include <leah/types.hpp>

// PS/2 keyboard on IRQ 1, scancode set 1.

namespace keyboard {

void init();

// Feed a scancode through the decoder as though it had arrived on IRQ 1.
// Exists so the translation tables and modifier handling can be exercised
// without depending on the emulator's ability to inject key events.
void inject_scancode(u8 scancode);

// Feed an already-decoded character into the same buffer. USB keyboards report
// HID usage codes rather than PS/2 scancodes, so their driver translates and
// hands the result straight here.
void inject_char(char c);

// Non-blocking: returns 0 when the buffer is empty.
char read();

// Blocks until a key is available, halting between interrupts rather than
// spinning hot.
char read_blocking();

bool has_input();

} // namespace keyboard
