#pragma once

#include <leah/bootinfo.hpp>
#include <leah/types.hpp>

// Linear framebuffer text output, using the 8x16 font stage 2 lifted out of
// the video BIOS.

namespace framebuffer {

constexpr u32 kGlyphWidth  = 8;
constexpr u32 kGlyphHeight = 16;

bool init(const boot::Info& info);
bool available();

// Re-maps the framebuffer with device attributes. Needs the VMM, so it runs
// after vmm::init() rather than at console startup.
bool remap_as_device();

u32 columns();
u32 rows();
u32 width();
u32 height();

void clear(u32 colour);
void draw_glyph(u32 column, u32 row, char c, u32 foreground, u32 background);
void scroll_up(u32 background);

// Packs to whatever the mode's pixel format is.
u32 rgb(u8 r, u8 g, u8 b);

// --- raw pixel access, for the window server --------------------------------
//
// The console only ever needed glyphs on a character grid. A compositor works
// in pixels and composes off-screen, so it needs to put single pixels down and
// to push a finished rectangle out in one go.

void plot(u32 x, u32 y, u32 colour);

// Copy a rectangle of packed 32-bit pixels from `source` (which is `stride`
// pixels wide) onto the screen at (x, y). This is the only path the compositor
// uses to reach the framebuffer, so every write to video memory goes through
// one place.
void blit(const u32* source, u32 stride, u32 x, u32 y, u32 width, u32 height);

// One row of the 8x16 BIOS font, as a bitmask with bit 7 leftmost. Drawing text
// at an arbitrary pixel position rather than on the character grid needs the
// glyph data itself.
u8 glyph_row(char c, u32 row);

} // namespace framebuffer
