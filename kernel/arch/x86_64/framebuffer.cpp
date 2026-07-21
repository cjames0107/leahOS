#include <leah/framebuffer.hpp>
#include <leah/string.hpp>
#include <leah/vmm.hpp>

namespace framebuffer {
namespace {

u8* g_pixels = nullptr;
const u8* g_font = nullptr;

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

    // No mapping happens here. The console is brought up before the VMM
    // exists - so that early failures can still be seen - and the framebuffer
    // is reachable through the 4 GiB identity map stage 2 built for exactly
    // this reason. remap_as_device() tightens the attributes once the VMM is
    // running.
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

    // The identity map covers this as ordinary cached RAM. It is device
    // memory: writes should not sit in a cache line waiting for an eviction
    // that only happens when something else needs the way.
    const u64 base = reinterpret_cast<u64>(g_pixels);
    const u64 bytes = static_cast<u64>(g_pitch) * g_height;
    return vmm::map_mmio(base, base, bytes);
}

u32 columns() { return g_columns; }
u32 rows()    { return g_rows; }
u32 width()   { return g_width; }
u32 height()  { return g_height; }

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
