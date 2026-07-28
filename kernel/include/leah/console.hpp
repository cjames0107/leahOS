#pragma once

#include <leah/bootinfo.hpp>
#include <leah/types.hpp>

// The kernel's only output device for now: the VGA text buffer, mirrored to
// COM1 so QEMU can capture a log we can scroll back through.

namespace console {

enum class Color : u8 {
    Black = 0,   Blue,         Green,        Cyan,
    Red,         Magenta,      Brown,        LightGray,
    DarkGray,    LightBlue,    LightGreen,   LightCyan,
    LightRed,    LightMagenta, Yellow,       White,
};

void init(const boot::Info& info);

bool graphical();
u32 columns();
u32 rows();
void clear();
void set_color(Color fg, Color bg = Color::Black);

// Stop (or resume) drawing to the framebuffer, without touching the serial
// output. The window server takes the screen when it starts, and text written
// over a composed desktop would simply corrupt it - but the boot log and any
// panic still need somewhere to go, and the serial port is unaffected.
void suspend_display(bool suspended);

void put(char c);
void write(const char* str);

// Supports %s %c %% and %d/%i %u %x %X %p, with an optional zero-padded
// minimum width (e.g. %016llx). Length modifiers l/ll/z are accepted and
// ignored - everything is promoted to 64-bit internally.
void printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

} // namespace console
