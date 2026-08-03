#include <leah/framebuffer.hpp>
#include <leah/memory.hpp>
#include <leah/string.hpp>
#include <leah/vmm.hpp>

// The screen, as something userland can be given - not as something the kernel
// draws on.
//
// Glyph rendering, scrolling and clearing left with the console: wserver maps
// this from ring 3 and paints its own pixels, and once the kernel stopped
// printing here there was nothing else drawing at all. What stays is the part
// only the kernel can do - finding the framebuffer the bootloader left, and
// handing out its address, its shape and the ROM font that came with it.

namespace framebuffer {
namespace {

u8* g_pixels = nullptr;
const u8* g_font = nullptr;
paddr_t g_pixels_phys = 0;
paddr_t g_font_phys = 0;

u32 g_width = 0;
u32 g_height = 0;
u32 g_pitch = 0;
u32 g_bytes_per_pixel = 0;

u32 g_columns = 0;
u32 g_rows = 0;

} // namespace

bool init(const boot::Info& info)
{
    if (info.framebuffer == 0 || info.font == 0)
        return false;
    if (info.bits_per_pixel != 32 && info.bits_per_pixel != 24)
        return false;

    g_width  = info.width;
    g_height = info.height;
    g_pitch  = info.pitch;
    g_bytes_per_pixel = info.bits_per_pixel / 8;

    // The console comes up before the VMM, reaching the framebuffer and font
    // through the stage-2 low identity map (physical == virtual). Once the VMM
    // has replaced that with a higher-half direct map, use_direct_map() swings
    // these pointers over to it. Both are remembered by physical address.
    g_pixels_phys = info.framebuffer;
    g_font_phys   = info.font;
    g_pixels = reinterpret_cast<u8*>(info.framebuffer);
    g_font   = reinterpret_cast<const u8*>(static_cast<u64>(info.font));

    g_columns = g_width / kGlyphWidth;
    g_rows    = g_height / kGlyphHeight;
    return true;
}

bool available() { return g_pixels != nullptr; }

bool remap_as_device()
{
    if (g_pixels == nullptr)
        return false;

    // The low identity map is gone; the framebuffer and font are now reached
    // through the direct map. The direct map is built to cover the low-4-GiB
    // MMIO window, so no separate mapping is needed. It is cached rather than
    // write-combining, which is fine under emulation; a dedicated uncached
    // mapping is a later refinement for real hardware.
    g_pixels = reinterpret_cast<u8*>(memory::phys_to_direct(g_pixels_phys));
    g_font   = reinterpret_cast<const u8*>(memory::phys_to_direct(g_font_phys));
    return true;
}

u32 columns() { return g_columns; }
u32 rows()    { return g_rows; }
u32 width()   { return g_width; }
u32 height()  { return g_height; }
u32 pitch()   { return g_pitch; }
u32 bits_per_pixel()    { return g_bytes_per_pixel * 8; }
paddr_t physical_base() { return g_pixels_phys; }

u8 glyph_row(char c, u32 row)
{
    if (g_font == nullptr || row >= kGlyphHeight)
        return 0;
    return g_font[static_cast<u8>(c) * kGlyphHeight + row];
}

u32 rgb(u8 r, u8 g, u8 b)
{
    return static_cast<u32>(r) << 16 | static_cast<u32>(g) << 8 | b;
}

} // namespace framebuffer
