/* Drawing straight onto the screen, for the two things that run before there
 * is a window server to ask.
 *
 * The boot splash and the login screen both need pixels, and both happen while
 * wserver is either not started or not yet the owner of anything. They cannot
 * open a window because there is nothing to open one with, and they cannot
 * print, because the kernel's console goes to the serial port now and nothing
 * else. So they draw.
 *
 * This is deliberately small: a rectangle, some text, and the font the system
 * already has. Anything that wants more than that should be a window.
 */

#include <display.h>
#include <screen.h>
#include <string.h>
#include <unistd.h>

#define GLYPH_W 8
#define GLYPH_H 16

static unsigned char* g_pixels;
static struct fb_info g_fb;
static unsigned char g_font[256 * GLYPH_H];
static int g_ready;

int screen_open(void)
{
    if (g_ready)
        return 0;
    if (fb_info(&g_fb) != 0 || g_fb.width == 0 || g_fb.height == 0)
        return -1;
    g_pixels = (unsigned char*)fb_map();
    if (g_pixels == 0)
        return -1;
    if (fb_font(g_font) != 0)
        return -1;
    g_ready = 1;
    return 0;
}

unsigned screen_width(void)  { return g_ready ? g_fb.width : 0; }
unsigned screen_height(void) { return g_ready ? g_fb.height : 0; }

void screen_fill(int x, int y, int w, int h, unsigned rgb)
{
    int row, col;
    if (!g_ready)
        return;
    for (row = 0; row < h; ++row) {
        const int py = y + row;
        unsigned char* line;
        if (py < 0 || (unsigned)py >= g_fb.height)
            continue;
        line = g_pixels + (unsigned)py * g_fb.pitch;
        for (col = 0; col < w; ++col) {
            const int px = x + col;
            if (px < 0 || (unsigned)px >= g_fb.width)
                continue;
            *(unsigned*)(line + (unsigned)px * 4) = rgb;
        }
    }
}

/* Two colours on alternate pixels, which at any distance reads as a shade
 * between them. Written straight into the framebuffer rather than through
 * screen_fill: a screenful is nearly a million pixels, and a function call with
 * bounds checks for each of them is slow enough to see. */
void screen_dither(int x, int y, int w, int h, unsigned a, unsigned b)
{
    int row, col;
    if (!g_ready)
        return;
    for (row = 0; row < h; ++row) {
        const int py = y + row;
        unsigned char* line;
        if (py < 0 || (unsigned)py >= g_fb.height)
            continue;
        line = g_pixels + (unsigned)py * g_fb.pitch;
        for (col = 0; col < w; ++col) {
            const int px = x + col;
            if (px < 0 || (unsigned)px >= g_fb.width)
                continue;
            *(unsigned*)(line + (unsigned)px * 4) = ((px + py) & 1) ? a : b;
        }
    }
}

/* A one-pixel outline, drawn as four fills - which is shorter than the loop
 * that would draw it a pixel at a time and no less clear. */
void screen_frame(int x, int y, int w, int h, unsigned rgb)
{
    screen_fill(x, y, w, 1, rgb);
    screen_fill(x, y + h - 1, w, 1, rgb);
    screen_fill(x, y, 1, h, rgb);
    screen_fill(x + w - 1, y, 1, h, rgb);
}

/* Light above and left, dark below and right, and the eye reads it as raised -
 * inverted, as sunken, which is what a text field is. The whole visual language
 * of this interface is these two lines. */
void screen_bevel(int x, int y, int w, int h, int raised)
{
    const unsigned tl = raised ? 0xFFFFFFu : 0x888888u;
    const unsigned br = raised ? 0x888888u : 0xFFFFFFu;
    screen_fill(x, y, w, 1, tl);
    screen_fill(x, y, 1, h, tl);
    screen_fill(x, y + h - 1, w, 1, br);
    screen_fill(x + w - 1, y, 1, h, br);
}

void screen_char(int x, int y, char c, unsigned fg, unsigned bg, int transparent)
{
    const unsigned char* glyph;
    int row, bit;
    if (!g_ready)
        return;
    glyph = g_font + (unsigned char)c * GLYPH_H;
    for (row = 0; row < GLYPH_H; ++row) {
        const unsigned char bits = glyph[row];
        for (bit = 0; bit < GLYPH_W; ++bit) {
            /* The ROM font is one byte per scanline, most significant bit
             * leftmost - the same order the kernel used to read it in. */
            const int on = (bits & (0x80 >> bit)) != 0;
            if (on)
                screen_fill(x + bit, y + row, 1, 1, fg);
            else if (!transparent)
                screen_fill(x + bit, y + row, 1, 1, bg);
        }
    }
}

void screen_text(int x, int y, const char* s, unsigned fg, unsigned bg,
                 int transparent)
{
    int i;
    for (i = 0; s[i] != '\0'; ++i)
        screen_char(x + i * GLYPH_W, y, s[i], fg, bg, transparent);
}

int screen_text_width(const char* s)
{
    return (int)strlen(s) * GLYPH_W;
}

void screen_text_centred(int centre_x, int y, const char* s, unsigned fg,
                         unsigned bg, int transparent)
{
    screen_text(centre_x - screen_text_width(s) / 2, y, s, fg, bg, transparent);
}

int screen_glyph_width(void)  { return GLYPH_W; }
int screen_glyph_height(void) { return GLYPH_H; }
