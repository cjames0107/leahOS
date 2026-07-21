#pragma once

#include <leah/types.hpp>

// The pair of 8259A controllers every PC still emulates. The APIC supersedes
// it, but the PIC is what is live at boot and it has to be dealt with either
// way - even "just use the APIC" starts by masking this off properly.

namespace pic {

// Move the IRQ lines to vectors 32-47.
//
// At power-on the master PIC raises IRQ 0-7 as vectors 8-15, which collide
// head-on with the CPU's own exceptions: a timer tick would arrive as #DF and
// a keypress as #TS. Intel reserved 0-31 after the PC had already shipped, so
// every x86 kernel starts by undoing this.
void init();

void mask(u8 irq);
void unmask(u8 irq);
void mask_all();

void end_of_interrupt(u8 irq);

// Spurious IRQs come from the PIC dropping a line between raising it and our
// acknowledging it. They arrive on line 7 or 15 and must not be EOI'd.
bool is_spurious(u8 irq);
void handle_spurious(u8 irq);

} // namespace pic
