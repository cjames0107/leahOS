#pragma once

#include <leah/types.hpp>

// PS/2 keyboard on IRQ 1, scancode set 1.

namespace keyboard {

// Modifier bits, as reported by modifiers(). Mirrored in <display.h>.
constexpr u32 kModShift = 1u << 0;
constexpr u32 kModCtrl  = 1u << 1;

// Which modifiers are held right now. What a click needs.
u32 modifiers();

// Which were held when the key that read() last returned was pressed.
//
// Not the same question, and the difference is the whole reason this exists:
// most modifiers are folded into the character - shift+a is 'A' - but the ones
// that are not, shift with an arrow above all, cannot be recovered by asking
// what is held afterwards. By then it is not.
u32 last_modifiers();

// What a USB keyboard says it is holding, since it never goes through the PS/2
// decoder that maintains the state above. Whatever is held on either keyboard
// counts as held: a machine can have both, and the answer to "is shift down"
// should not depend on which one the hand is on.
void set_usb_modifiers(u32 mods);

// False when the machine has no PS/2 controller at all, which is normal on
// anything recent: the keys come from usbd instead, through the same queue.

void init();

// Feed a scancode through the decoder as though it had arrived on IRQ 1.
// Exists so the translation tables and modifier handling can be exercised
// without depending on the emulator's ability to inject key events.

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
