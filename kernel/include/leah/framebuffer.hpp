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

} // namespace framebuffer
