#include <leah/framebuffer.hpp>
#include <leah/memory.hpp>
#include <leah/string.hpp>
#include <leah/vmm.hpp>

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

inline u8* pixel_at(u32 x, u32 y)
{
    return g_pixels + static_cast<u64>(y) * g_pitch + static_cast<u64>(x) * g_bytes_per_pixel;
}

inline void put_pixel(u32 x, u32 y, u32 colour)
{
    u8* p = pixel_at(x, y);
    p[0] = static_cast<u8>(colour);
    p[1] = static_cast<u8>(colour >> 8);
    p[2] = static_cast<u8>(colour >> 16);
    if (g_bytes_per_pixel == 4)
        p[3] = 0;
}

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

void plot(u32 x, u32 y, u32 colour)
{
    if (g_pixels == nullptr || x >= g_width || y >= g_height)
        return;
    put_pixel(x, y, colour);
}

void blit(const u32* source, u32 stride, u32 x, u32 y, u32 width, u32 height)
{
    if (g_pixels == nullptr || source == nullptr)
        return;
    for (u32 row = 0; row < height; ++row) {
        const u32 screen_y = y + row;
        if (screen_y >= g_height)
            break;
        const u32* in = source + static_cast<u64>(row) * stride;
        // 32bpp is the common case and copies a scanline at a time; anything
        // else goes pixel by pixel through the packer.
        if (g_bytes_per_pixel == 4 && x + width <= g_width) {
            auto* out = reinterpret_cast<u32*>(pixel_at(x, screen_y));
            memcpy(out, in, static_cast<usize>(width) * sizeof(u32));
        } else {
            for (u32 column = 0; column < width; ++column) {
                if (x + column < g_width)
                    put_pixel(x + column, screen_y, in[column]);
            }
        }
    }
}

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

void clear(u32 colour)
{
    for (u32 y = 0; y < g_height; ++y) {
        for (u32 x = 0; x < g_width; ++x)
            put_pixel(x, y, colour);
    }
}

void draw_glyph(u32 column, u32 row, char c, u32 foreground, u32 background)
{
    if (column >= g_columns || row >= g_rows)
        return;

    // The BIOS font is one byte per scanline, most significant bit leftmost.
    const u8* glyph = g_font + static_cast<u32>(static_cast<u8>(c)) * kGlyphHeight;

    const u32 origin_x = column * kGlyphWidth;
    const u32 origin_y = row * kGlyphHeight;

    for (u32 line = 0; line < kGlyphHeight; ++line) {
        const u8 bits = glyph[line];
        for (u32 bit = 0; bit < kGlyphWidth; ++bit) {
            const bool set = (bits & (0x80 >> bit)) != 0;
            put_pixel(origin_x + bit, origin_y + line, set ? foreground : background);
        }
    }
}

void scroll_up(u32 background)
{
    const u64 line_bytes = static_cast<u64>(g_pitch) * kGlyphHeight;
    const u64 total = static_cast<u64>(g_pitch) * g_height;

    memmove(g_pixels, g_pixels + line_bytes, total - line_bytes);

    for (u32 y = g_height - kGlyphHeight; y < g_height; ++y) {
        for (u32 x = 0; x < g_width; ++x)
            put_pixel(x, y, background);
    }
}

} // namespace framebuffer
