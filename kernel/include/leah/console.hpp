#pragma once

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

void init();
void clear();
void set_color(Color fg, Color bg = Color::Black);

void put(char c);
void write(const char* str);

// Supports %s %c %% and %d/%i %u %x %X %p, with an optional zero-padded
// minimum width (e.g. %016llx). Length modifiers l/ll/z are accepted and
// ignored - everything is promoted to 64-bit internally.
void printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

} // namespace console
