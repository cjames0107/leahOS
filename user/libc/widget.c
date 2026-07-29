#include <display.h>
#include <shm.h>
#include <wproto.h>
#include <string.h>
#include <widget.h>

static uint32_t* g_px;
static unsigned  g_w, g_h;
static unsigned char g_font[256 * 16];
static int g_have_font;

/* The theme, cached from the window server's control block. */
static struct ws_shared* g_ws;
static uint32_t g_sel = 0xB0C4DE, g_body = WG_PAPER, g_ink = WG_INK;
static unsigned g_scale = 1;

void wg_theme(void)
{
    if (g_ws == 0) {
        const int id = shm_open(WS_CONTROL_KEY, 0, 0);
        if (id < 0)
            return;
        g_ws = (struct ws_shared*)shm_map(id);
        if (g_ws == 0)
            return;
    }
    if (g_ws->magic != WS_MAGIC)
        return;
    if (g_ws->theme.selection != 0) g_sel = g_ws->theme.selection;
    if (g_ws->theme.body != 0)      g_body = g_ws->theme.body;
    g_ink = g_ws->theme.text;
    g_scale = g_ws->theme.text_scale == 2 ? 2u : 1u;
}

uint32_t wg_sel_colour(void)  { return g_sel; }
uint32_t wg_body_colour(void) { return g_body; }
uint32_t wg_ink_colour(void)  { return g_ink; }
unsigned wg_scale(void)       { return g_scale; }

void wg_target(uint32_t* pixels, unsigned width, unsigned height)
{
    g_px = pixels;
    g_w = width;
    g_h = height;
}

int wg_font(void)
{
    if (g_have_font)
        return 0;
    if (fb_font(g_font) != 0)
        return -1;
    g_have_font = 1;
    return 0;
}

void wg_plot(int x, int y, uint32_t colour)
{
    if (g_px == 0 || x < 0 || y < 0 ||
        (unsigned)x >= g_w || (unsigned)y >= g_h)
        return;
    g_px[(unsigned)y * g_w + (unsigned)x] = colour;
}

void wg_fill(int x, int y, int w, int h, uint32_t colour)
{
    /* Clipped once here rather than per pixel: filling a window's background is
     * most of its area, and doing the bounds check a million times shows. */
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w, y1 = y + h;
    if (x1 > (int)g_w) x1 = (int)g_w;
    if (y1 > (int)g_h) y1 = (int)g_h;
    if (g_px == 0)
        return;
    for (int row = y0; row < y1; ++row)
        for (int col = x0; col < x1; ++col)
            g_px[(unsigned)row * g_w + (unsigned)col] = colour;
}

void wg_bevel(int x, int y, int w, int h, int raised)
{
    const uint32_t tl = raised ? WG_LIGHT : WG_SHADOW;
    const uint32_t br = raised ? WG_SHADOW : WG_LIGHT;
    for (int i = 0; i < w; ++i) {
        wg_plot(x + i, y, tl);
        wg_plot(x + i, y + h - 1, br);
    }
    for (int i = 0; i < h; ++i) {
        wg_plot(x, y + i, tl);
        wg_plot(x + w - 1, y + i, br);
    }
}

void wg_text(int x, int y, const char* s, uint32_t colour)
{
    /* The font is a bitmap, so "larger text" can only mean whole-pixel doubling.
     * Saying so is better than pretending to a range of sizes there is no
     * outline to produce. */
    const unsigned k = g_scale;
    for (unsigned i = 0; s[i] != '\0'; ++i) {
        const unsigned char* glyph = &g_font[(unsigned char)s[i] * 16];
        for (int row = 0; row < WG_GLYPH_H; ++row)
            for (int col = 0; col < WG_GLYPH_W; ++col) {
                if (!(glyph[row] & (0x80 >> col)))
                    continue;
                for (unsigned dy = 0; dy < k; ++dy)
                    for (unsigned dx = 0; dx < k; ++dx)
                        wg_plot(x + (int)(i * WG_GLYPH_W * k + (unsigned)col * k + dx),
                                y + (int)((unsigned)row * k + dy), colour);
            }
    }
}

void wg_text_clipped(int x, int y, const char* s, uint32_t colour, int max_w)
{
    const int fits = max_w / WG_GLYPH_W;
    if (fits <= 0)
        return;
    const int len = (int)strlen(s);
    if (len <= fits) {
        wg_text(x, y, s, colour);
        return;
    }
    char cut[160];
    int keep = fits - 2;
    if (keep < 1) keep = 1;
    if (keep > (int)sizeof(cut) - 3) keep = (int)sizeof(cut) - 3;
    for (int i = 0; i < keep; ++i)
        cut[i] = s[i];
    cut[keep] = '.';
    cut[keep + 1] = '.';
    cut[keep + 2] = '\0';
    wg_text(x, y, cut, colour);
}

void wg_button(int x, int y, int w, int h, const char* label, int down)
{
    wg_fill(x, y, w, h, WG_FACE);
    wg_bevel(x, y, w, h, !down);
    const int tw = (int)strlen(label) * WG_GLYPH_W;
    wg_text(x + (w - tw) / 2 + down, y + (h - WG_GLYPH_H) / 2 + down,
            label, WG_INK);
}

/* --- scrollbars ----------------------------------------------------------- */

/* The thumb's extent within the trough, as a fraction of the span. Kept to a
 * minimum size so it stays grabbable on a long list. */
static void thumb_of(int track, int first, int page, int span,
                     int* pos, int* len)
{
    if (span <= page || span <= 0) {
        *pos = 0;
        *len = track;
        return;
    }
    int l = track * page / span;
    if (l < 12) l = 12;
    if (l > track) l = track;
    int p = (track - l) * first / (span - page);
    if (p < 0) p = 0;
    if (p > track - l) p = track - l;
    *pos = p;
    *len = l;
}

static void arrow(int x, int y, int size, int dir)
{
    /* dir: 0 up, 1 down, 2 left, 3 right. A filled triangle by rows. */
    for (int i = 0; i < size / 2; ++i) {
        const int run = i * 2 + 1;
        for (int k = 0; k < run; ++k) {
            int px, py;
            if (dir == 0)      { px = x + size / 2 - i + k; py = y + 3 + i; }
            else if (dir == 1) { px = x + size / 2 - i + k; py = y + size - 4 - i; }
            else if (dir == 2) { px = x + 3 + i;            py = y + size / 2 - i + k; }
            else               { px = x + size - 4 - i;     py = y + size / 2 - i + k; }
            wg_plot(px, py, WG_INK);
        }
    }
}

void wg_scrollbar_v(int x, int y, int h, int first, int page, int span)
{
    wg_fill(x, y, WG_SCROLL_W, h, 0xA0A0A0);
    wg_button(x, y, WG_SCROLL_W, WG_SCROLL_W, "", 0);
    arrow(x, y, WG_SCROLL_W, 0);
    wg_button(x, y + h - WG_SCROLL_W, WG_SCROLL_W, WG_SCROLL_W, "", 0);
    arrow(x, y + h - WG_SCROLL_W, WG_SCROLL_W, 1);

    const int track = h - WG_SCROLL_W * 2;
    if (track <= 0)
        return;
    int pos, len;
    thumb_of(track, first, page, span, &pos, &len);
    wg_fill(x, y + WG_SCROLL_W + pos, WG_SCROLL_W, len, WG_FACE);
    wg_bevel(x, y + WG_SCROLL_W + pos, WG_SCROLL_W, len, 1);
}

void wg_scrollbar_h(int x, int y, int w, int first, int page, int span)
{
    wg_fill(x, y, w, WG_SCROLL_W, 0xA0A0A0);
    wg_button(x, y, WG_SCROLL_W, WG_SCROLL_W, "", 0);
    arrow(x, y, WG_SCROLL_W, 2);
    wg_button(x + w - WG_SCROLL_W, y, WG_SCROLL_W, WG_SCROLL_W, "", 0);
    arrow(x + w - WG_SCROLL_W, y, WG_SCROLL_W, 3);

    const int track = w - WG_SCROLL_W * 2;
    if (track <= 0)
        return;
    int pos, len;
    thumb_of(track, first, page, span, &pos, &len);
    wg_fill(x + WG_SCROLL_W + pos, y, len, WG_SCROLL_W, WG_FACE);
    wg_bevel(x + WG_SCROLL_W + pos, y, len, WG_SCROLL_W, 1);
}

static int clamp_first(int first, int page, int span)
{
    if (first > span - page) first = span - page;
    if (first < 0) first = 0;
    return first;
}

int wg_scroll_hit_v(int x, int y, int bx, int by, int bh,
                    int first, int page, int span)
{
    if (x < bx || x >= bx + WG_SCROLL_W || y < by || y >= by + bh)
        return first;
    if (y < by + WG_SCROLL_W)
        return clamp_first(first - 1, page, span);
    if (y >= by + bh - WG_SCROLL_W)
        return clamp_first(first + 1, page, span);

    /* On the trough: jump so the thumb centres on the click, which is what a
     * long list needs far more than paging one screen at a time. */
    const int track = bh - WG_SCROLL_W * 2;
    if (track <= 0 || span <= page)
        return first;
    int pos, len;
    thumb_of(track, first, page, span, &pos, &len);
    const int at = y - (by + WG_SCROLL_W);
    if (at < pos)  return clamp_first(first - page, page, span);
    if (at >= pos + len) return clamp_first(first + page, page, span);
    return first;
}

int wg_scroll_hit_h(int x, int y, int bx, int by, int bw,
                    int first, int page, int span)
{
    if (y < by || y >= by + WG_SCROLL_W || x < bx || x >= bx + bw)
        return first;
    if (x < bx + WG_SCROLL_W)
        return clamp_first(first - 1, page, span);
    if (x >= bx + bw - WG_SCROLL_W)
        return clamp_first(first + 1, page, span);
    const int track = bw - WG_SCROLL_W * 2;
    if (track <= 0 || span <= page)
        return first;
    int pos, len;
    thumb_of(track, first, page, span, &pos, &len);
    const int at = x - (bx + WG_SCROLL_W);
    if (at < pos) return clamp_first(first - page, page, span);
    if (at >= pos + len) return clamp_first(first + page, page, span);
    return first;
}

int wg_scroll_on_thumb_v(int y, int by, int bh, int first, int page, int span)
{
    const int track = bh - WG_SCROLL_W * 2;
    if (track <= 0 || span <= page)
        return 0;
    int pos, len;
    thumb_of(track, first, page, span, &pos, &len);
    const int at = y - (by + WG_SCROLL_W);
    return at >= pos && at < pos + len;
}

int wg_scroll_on_thumb_h(int x, int bx, int bw, int first, int page, int span)
{
    const int track = bw - WG_SCROLL_W * 2;
    if (track <= 0 || span <= page)
        return 0;
    int pos, len;
    thumb_of(track, first, page, span, &pos, &len);
    const int at = x - (bx + WG_SCROLL_W);
    return at >= pos && at < pos + len;
}

int wg_scroll_drag_v(int y, int by, int bh, int page, int span)
{
    const int track = bh - WG_SCROLL_W * 2;
    if (track <= 0 || span <= page)
        return 0;
    int pos, len;
    thumb_of(track, 0, page, span, &pos, &len);
    const int usable = track - len;
    if (usable <= 0)
        return 0;
    /* Take the pointer as the middle of the thumb, so the thing under the
     * cursor is the thing that moves. */
    int at = y - (by + WG_SCROLL_W) - len / 2;
    if (at < 0) at = 0;
    if (at > usable) at = usable;
    return clamp_first(at * (span - page) / usable, page, span);
}

int wg_scroll_drag_h(int x, int bx, int bw, int page, int span)
{
    const int track = bw - WG_SCROLL_W * 2;
    if (track <= 0 || span <= page)
        return 0;
    int pos, len;
    thumb_of(track, 0, page, span, &pos, &len);
    const int usable = track - len;
    if (usable <= 0)
        return 0;
    int at = x - (bx + WG_SCROLL_W) - len / 2;
    if (at < 0) at = 0;
    if (at > usable) at = usable;
    return clamp_first(at * (span - page) / usable, page, span);
}
