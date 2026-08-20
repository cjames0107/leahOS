#include <display.h>
#include <draw.h>
#include <font.h>
#include <paths.h>
#include <shm.h>
#include <wproto.h>
#include <stdio.h>
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
    /* A new target is a new window: whatever was clipped belonged to the old
     * one. */
    wg_clip_none();
}

/* The proportional font, opened once and shared by every control.
 *
 * Lazily, because most of the programs linking libc never draw anything and
 * should not pay forty kilobytes and a parse for a font they will not use. */
static struct font* g_face;
static int          g_face_tried;

static struct font* face(void)
{
    if (!g_face_tried) {
        g_face_tried = 1;
        g_face = font_open(PATH_FONTS "/sans.ttf");
    }
    return g_face;
}

/* Where drawing is allowed to land.
 *
 * A window's own edges, until something narrows it. A view that draws a list
 * with a pixel scroll puts a half-shown row above its own top edge, and with
 * nothing to stop it that row lands in the toolbar; the browser's answer was
 * to paint the toolbar again afterwards, which works right up until the thing
 * on top is a component drawn by the tree in tree order.
 *
 * Held as a rectangle rather than a stack. One at a time is all any of this
 * needs, and a stack is a thing to get wrong in a drawing loop. */
static int g_cx, g_cy, g_cw, g_ch;

void wg_clip(int x, int y, int w, int h)
{
    g_cx = x; g_cy = y; g_cw = w; g_ch = h;
}

void wg_clip_none(void)
{
    g_cx = g_cy = 0;
    g_cw = (int)g_w;
    g_ch = (int)g_h;
}

static int clipped_out(int x, int y)
{
    return x < g_cx || y < g_cy || x >= g_cx + g_cw || y >= g_cy + g_ch;
}

/* The surface draw.c wants, over whatever wg_target was pointed at, carrying
 * the clip so the rounded and blended paths honour it too. */
static void blend_px(int x, int y, uint32_t over);

static struct surface canvas(void)
{
    struct surface s;
    s.pixels = g_px;
    s.w = (int)g_w;
    s.h = (int)g_h;
    s.cx = g_cx;
    s.cy = g_cy;
    s.cw = g_cw;
    s.ch = g_ch;
    return s;
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
        (unsigned)x >= g_w || (unsigned)y >= g_h || clipped_out(x, y))
        return;
    g_px[(unsigned)y * g_w + (unsigned)x] = 0xFF000000u | colour;
}

/* An icon: 32x32 pixels with one bit of alpha, drawn straight over whatever is
 * already there. Transparent pixels are skipped rather than blended, because
 * these are cut-out shapes over an arbitrary background and there is nothing
 * to blend with that is not already on the screen. */
void wg_icon_scaled(int x, int y, const uint32_t* px, int sw, int sh,
                    int dw, int dh)
{
    if (g_px == 0 || px == 0 || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
        return;
    for (int row = 0; row < dh; ++row) {
        const int py = y + row;
        if (py < 0 || (unsigned)py >= g_h || py < g_cy || py >= g_cy + g_ch)
            continue;
        const int sy = row * sh / dh;
        for (int col = 0; col < dw; ++col) {
            const int pxx = x + col;
            if (pxx < 0 || (unsigned)pxx >= g_w || clipped_out(pxx, py))
                continue;
            /* Nearest neighbour, which for halving a pixel-drawn icon means
             * taking every other pixel - the right answer for artwork with
             * hard edges, where averaging would only make it muddy. */
            const uint32_t s = px[(unsigned)sy * (unsigned)sw +
                                  (unsigned)(col * sw / dw)];
            if ((s >> 24) != 0)
                /* Opaque, explicitly. Stripping the alpha to zero used to mean
                 * "no alpha here"; since the server started reading that byte
                 * it means "not there at all", and every icon in the system
                 * turned into an empty square. */
                g_px[(unsigned)py * g_w + (unsigned)pxx] =
                    0xFF000000u | (s & 0x00FFFFFFu);
        }
    }
}

void wg_icon(int x, int y, const uint32_t* px, int w, int h)
{
    wg_icon_scaled(x, y, px, w, h, w, h);
}

void wg_fill(int x, int y, int w, int h, uint32_t colour)
{
    /* Clipped once here rather than per pixel: filling a window's background is
     * most of its area, and doing the bounds check a million times shows. */
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w, y1 = y + h;
    if (x1 > (int)g_w) x1 = (int)g_w;
    if (y1 > (int)g_h) y1 = (int)g_h;
    if (x0 < g_cx) x0 = g_cx;
    if (y0 < g_cy) y0 = g_cy;
    if (x1 > g_cx + g_cw) x1 = g_cx + g_cw;
    if (y1 > g_cy + g_ch) y1 = g_cy + g_ch;
    if (g_px == 0)
        return;
    const uint32_t solid = 0xFF000000u | colour;
    for (int row = y0; row < y1; ++row)
        for (int col = x0; col < x1; ++col)
            g_px[(unsigned)row * g_w + (unsigned)col] = solid;
}

/* --- the RGB picker -------------------------------------------------------
 *
 * Three tracks, each showing what its own channel does to the colour being
 * built while the other two hold still. That is the whole trick: you can see
 * the effect of a slider before you touch it, so choosing is looking rather
 * than guessing.
 */
#define RGB_TRACK_X 22          /* room for the R/G/B letter */
#define RGB_ROW     18
#define RGB_SWATCH  54

static int rgb_track_w(int w) { return w - RGB_TRACK_X - RGB_SWATCH - 12; }

static unsigned rgb_channel(uint32_t colour, int ch)
{
    return (colour >> (16 - 8 * ch)) & 0xFFu;
}

static uint32_t rgb_with(uint32_t colour, int ch, unsigned v)
{
    const unsigned shift = (unsigned)(16 - 8 * ch);
    return (colour & ~(0xFFu << shift)) | ((v & 0xFFu) << shift);
}

void wg_rgb_draw(int x, int y, int w, uint32_t colour)
{
    const int tw = rgb_track_w(w);
    static const char kLabel[3] = { 'R', 'G', 'B' };
    for (int ch = 0; ch < 3; ++ch) {
        const int ty = y + ch * RGB_ROW;
        const char lbl[2] = { kLabel[ch], 0 };
        wg_text(x, ty + 1, lbl, WG_INK);
        /* The gradient is the colour itself swept along one axis. */
        for (int i = 0; i < tw; ++i) {
            const unsigned v = (unsigned)(i * 255 / (tw > 1 ? tw - 1 : 1));
            wg_fill(x + RGB_TRACK_X + i, ty + 2, 1, 12, rgb_with(colour, ch, v));
        }
        wg_bevel(x + RGB_TRACK_X - 1, ty + 1, tw + 2, 14, 0);
        const int at = (int)(rgb_channel(colour, ch)) * (tw - 1) / 255;
        wg_fill(x + RGB_TRACK_X + at - 2, ty, 5, 16, WG_FACE);
        wg_bevel(x + RGB_TRACK_X + at - 2, ty, 5, 16, 1);
    }
    const int sx = x + RGB_TRACK_X + tw + 8;
    wg_fill(sx, y, RGB_SWATCH, 3 * RGB_ROW - 4, colour);
    wg_bevel(sx, y, RGB_SWATCH, 3 * RGB_ROW - 4, 0);
    char v[24];
    snprintf(v, sizeof(v), "%u %u %u", rgb_channel(colour, 0),
             rgb_channel(colour, 1), rgb_channel(colour, 2));
    wg_text_clipped(x + RGB_TRACK_X, y + 3 * RGB_ROW, v, WG_DIM, w - RGB_TRACK_X);
}

int wg_rgb_hit(int x, int y, int w, int mx, int my)
{
    const int tw = rgb_track_w(w);
    /* Generous vertically: a 14-pixel track is hard to hit exactly, and the
     * rows are far enough apart that being loose cannot pick the wrong one. */
    for (int ch = 0; ch < 3; ++ch) {
        const int ty = y + ch * RGB_ROW;
        if (my >= ty && my < ty + RGB_ROW &&
            mx >= x + RGB_TRACK_X - 4 && mx < x + RGB_TRACK_X + tw + 4)
            return ch;
    }
    return -1;
}

uint32_t wg_rgb_move(uint32_t colour, int channel, int x, int w, int mx)
{
    if (channel < 0 || channel > 2)
        return colour;
    const int tw = rgb_track_w(w);
    int at = mx - (x + RGB_TRACK_X);
    if (at < 0) at = 0;
    if (at > tw - 1) at = tw - 1;
    return rgb_with(colour, channel,
                    (unsigned)(at * 255 / (tw > 1 ? tw - 1 : 1)));
}

/* --- a plain slider ------------------------------------------------------- */

#define SLIDER_THUMB 9

static int slider_span(int w) { return w - SLIDER_THUMB; }

void wg_slider_draw(int x, int y, int w, int value, int max)
{
    if (max <= 0) max = 1;
    if (value < 0) value = 0;
    if (value > max) value = max;
    const int span = slider_span(w);
    const int at = value * span / max;

    /* The track is sunken and the part behind the thumb is filled, so the
     * setting reads at a glance without having to find the thumb first. */
    wg_fill(x, y + 6, w, 6, WG_PAPER);
    wg_bevel(x, y + 6, w, 6, 0);
    if (at > 0)
        wg_fill(x + 1, y + 7, at, 4, wg_sel_colour());

    wg_fill(x + at, y, SLIDER_THUMB, WG_SLIDER_H, WG_FACE);
    wg_bevel(x + at, y, SLIDER_THUMB, WG_SLIDER_H, 1);
}

int wg_slider_hit(int x, int y, int w, int mx, int my)
{
    /* Generous vertically: the track is six pixels and nobody aims at that. */
    return mx >= x - 4 && mx < x + w + 4 && my >= y - 2 && my < y + WG_SLIDER_H + 2;
}

int wg_slider_value(int x, int w, int mx, int max)
{
    const int span = slider_span(w);
    int at = mx - x - SLIDER_THUMB / 2;
    if (at < 0) at = 0;
    if (at > span) at = span;
    return span > 0 ? at * max / span : 0;
}

/* What used to be two one-pixel lines suggesting a light source.
 *
 * There is no light source any more - depth comes from a shadow outside a
 * shape rather than from a highlight drawn inside its edge - so this is a
 * rounded hairline, darker when the caller asked for sunken. The name and the
 * signature stay because dozens of call sites use them and all of them mean
 * "outline this box". */
void wg_bevel(int x, int y, int w, int h, int raised)
{
    const struct surface c = canvas();
    draw_round_rect_outline(&c, x, y, w, h, WG_RADIUS, 1,
                            raised ? 0x59FFFFFFu : 0x26000000u);
}

int wg_text_size(void)
{
    /* Chosen so a line still occupies the sixteen-pixel slot every caller was
     * written against: thirteen pixels of em gives about twelve of ascent and
     * four of descent, which lands where the bitmap cell used to. Callers that
     * laid text out by counting sixteens keep working. */
    return (int)(13 * g_scale);
}

int wg_text_width(const char* s)
{
    struct font* f = face();
    if (f == 0)
        return (int)strlen(s) * WG_GLYPH_W * (int)g_scale;
    return font_width(f, wg_text_size(), s);
}

int wg_text_height(void)
{
    struct font* f = face();
    return f != 0 ? font_line_height(f, wg_text_size())
                  : WG_GLYPH_H * (int)g_scale;
}

/* `y` is the top of the line, as it always was - not the baseline. Every
 * caller in this system positions text by the top of a row, and changing that
 * would mean touching all of them to gain nothing. */
void wg_text(int x, int y, const char* s, uint32_t colour)
{
    struct font* f = face();
    if (f == 0)
        return;

    const int size = wg_text_size();
    const int baseline = y + font_ascent(f, size);
    const unsigned alpha = (colour >> 24) != 0 ? (colour >> 24) & 0xFF : 255;
    const uint32_t rgb = colour & 0x00FFFFFFu;

    const char* at = s;
    for (;;) {
        const unsigned ch = utf8_next(&at);
        if (ch == 0)
            break;
        struct glyph g;
        if (font_glyph(f, size, ch, &g) != 0)
            continue;
        for (int gy = 0; gy < g.h; ++gy)
            for (int gx = 0; gx < g.w; ++gx) {
                const unsigned cov = g.coverage[(long)gy * g.w + gx];
                if (cov == 0)
                    continue;
                blend_px(x + g.left + gx, baseline - g.top + gy,
                         (((alpha * cov + 127) / 255) << 24) | rgb);
            }
        x += g.advance;
    }
}

/* --- styled text ------------------------------------------------------------
 *
 * One typeface, so weight and slant are effects rather than faces. See the
 * note in widget.h.
 */

/* How far a row of a glyph leans. A quarter of its height above the baseline
 * is about the slant a real italic has, and it costs a shift of the row rather
 * than a resampling of it. */
static int slant_of(int above_baseline)
{
    return above_baseline / 4;
}

int wg_styled(int x, int y, const char* s, int len, uint32_t colour,
              int size_px, unsigned style)
{
    struct font* f = face();
    if (f == 0 || s == 0 || len <= 0 || size_px <= 0)
        return x;

    const int baseline = y + font_ascent(f, size_px);
    const unsigned alpha = (colour >> 24) != 0 ? (colour >> 24) & 0xFF : 255;
    const uint32_t rgb = colour & 0x00FFFFFFu;
    const int bold = (style & WG_STYLE_BOLD) != 0;
    const int italic = (style & WG_STYLE_ITALIC) != 0;
    const int start = x;

    const char* at = s;
    const char* end = s + len;
    while (at < end) {
        const unsigned ch = utf8_next(&at);
        if (ch == 0)
            break;
        struct glyph g;
        if (font_glyph(f, size_px, ch, &g) != 0)
            continue;
        for (int gy = 0; gy < g.h; ++gy) {
            /* The lean is per row and grows with height above the baseline, so
             * the bottom of a letter stays put and the top moves right. */
            const int lean = italic ? slant_of(g.top - gy) : 0;
            for (int gx = 0; gx < g.w; ++gx) {
                const unsigned cov = g.coverage[(long)gy * g.w + gx];
                if (cov == 0)
                    continue;
                const uint32_t ink =
                    (((alpha * cov + 127) / 255) << 24) | rgb;
                blend_px(x + g.left + gx + lean, baseline - g.top + gy, ink);
                if (bold)
                    blend_px(x + g.left + gx + lean + 1,
                             baseline - g.top + gy, ink);
            }
        }
        /* A bold glyph is a pixel wider than the one it was made from, and
         * without this the extra pixel is drawn over the next letter. */
        x += g.advance + (bold ? 1 : 0);
    }

    if ((style & WG_STYLE_UNDERLINE) != 0 && x > start) {
        const int under = baseline + (size_px / 8 > 1 ? size_px / 8 : 1);
        wg_fill(start, under, x - start, size_px >= 24 ? 2 : 1, colour);
    }
    return x;
}

int wg_styled_width(const char* s, int len, int size_px, unsigned style)
{
    struct font* f = face();
    if (f == 0 || s == 0 || len <= 0 || size_px <= 0)
        return 0;
    const int bold = (style & WG_STYLE_BOLD) != 0;
    int w = 0;
    const char* at = s;
    const char* end = s + len;
    while (at < end) {
        const unsigned ch = utf8_next(&at);
        if (ch == 0)
            break;
        struct glyph g;
        if (font_glyph(f, size_px, ch, &g) != 0)
            continue;
        w += g.advance + (bold ? 1 : 0);
    }
    return w;
}

int wg_styled_height(int size_px)
{
    struct font* f = face();
    return f != 0 ? font_line_height(f, size_px) : size_px + 4;
}

int wg_styled_ascent(int size_px)
{
    struct font* f = face();
    return f != 0 ? font_ascent(f, size_px) : size_px;
}

void wg_text_clipped(int x, int y, const char* s, uint32_t colour, int max_w)
{
    if (max_w <= 0)
        return;
    if (wg_text_width(s) <= max_w) {
        wg_text(x, y, s, colour);
        return;
    }
    /* Trimmed a character at a time and measured each time, because the glyphs
     * are no longer all one width and there is no count that means "this many
     * pixels". The ellipsis has to fit too. */
    char cut[192];
    int keep = (int)strlen(s);
    if (keep > (int)sizeof(cut) - 3)
        keep = (int)sizeof(cut) - 3;
    while (keep > 0) {
        memcpy(cut, s, (size_t)keep);
        cut[keep] = '.';
        cut[keep + 1] = '.';
        cut[keep + 2] = '\0';
        if (wg_text_width(cut) <= max_w)
            break;
        --keep;
    }
    if (keep > 0)
        wg_text(x, y, cut, colour);
}


/* --- the glass vocabulary -----------------------------------------------------
 *
 * Five things the new interface is made of, in one place so that every window
 * agrees about them: the surface a window is painted on, a group of controls
 * that share one pill, a container that holds other things, a sidebar, and the
 * bright edge that makes any of them look like glass rather than paint.
 *
 * All of them ask whether the glass is on, because the answer changes what
 * "slightly darker" has to mean. Over a blurred backdrop a container is a wash
 * of white and the blur does the rest; over a flat panel there is nothing
 * behind to show through, so the same container has to be a shade darker than
 * what it sits on or it is invisible.
 */

static int glass_on(void);

int wg_glass_on(void)
{
    return glass_on();
}

static int glass_on(void)
{
    return g_ws != 0 && g_ws->magic == WS_MAGIC && g_ws->theme.blur != 0;
}

/* The colour a window's own background is, which is the colour its title bar
 * is: the server paints the frame in this and the client paints its content in
 * it, so the two meet without a seam. */
uint32_t wg_base_colour(void)
{
    const uint32_t face = g_ws != 0 ? (g_ws->theme.face & 0x00FFFFFFu) : WG_FACE;
    /* A quarter of a coat of white rather than two thirds.
     *
     * The heavier wash took every theme to within a few points of pure white,
     * so a window was white whatever the desktop behind it looked like and the
     * theme colour may as well not have existed. Left mostly alone, the face
     * shows through as a tint - still light enough to read black text on, and
     * recognisably the colour the theme asked for. */
    return draw_over(0xFF000000u | face, 0x2AFFFFFFu);
}

/* Shift a colour toward black by `amount` out of 255, which is what "slightly
 * darker" is when there is nothing behind to be slightly more opaque than. */
static uint32_t darken(uint32_t colour, unsigned amount)
{
    unsigned r = (colour >> 16) & 0xFF, g = (colour >> 8) & 0xFF, b = colour & 0xFF;
    r = r > amount ? r - amount : 0;
    g = g > amount ? g - amount : 0;
    b = b > amount ? b - amount : 0;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* A bright line just inside the top edge and a fainter one inside the bottom.
 *
 * This is the whole trick of a glassy surface: light collects along the upper
 * lip of something transparent and again, more weakly, where it curves away at
 * the bottom. Two arcs of the shape's own outline, clipped to the top and
 * bottom bands, and the eye supplies the thickness. */
static void inner_glow(int x, int y, int w, int h, int radius)
{
    if (w <= 2 || h <= 4)
        return;
    struct surface c = canvas();
    const int band = h / 3 > 1 ? h / 3 : 1;

    if (glass_on()) {
        /* One faint edge on the glass rather than two banded ones: the clip
         * that makes the bands lives on the surface, and the alpha-keeping
         * path does not go through a surface. */
        wg_glass_outline(x, y, w, h, radius, 1, 0x30FFFFFFu);
        return;
    }
    c.cx = x; c.cy = y; c.cw = w; c.ch = band;
    draw_round_rect_outline(&c, x, y, w, h, radius, 1, 0x59FFFFFFu);

    c.cy = y + h - band; c.ch = band;
    draw_round_rect_outline(&c, x, y, w, h, radius, 1, 0x24FFFFFFu);
}

/* A soft shadow under a floating control, for the glass only.
 *
 * On the glass a control is a pane of frosted white on a blurred backdrop, and
 * without a shadow it has no depth - it reads as a lighter patch rather than as
 * something sitting above. Opaque mode gets none: there is no depth to imply
 * when every surface is the same flat sheet, and a drop shadow there just
 * makes a crisp panel look smudged.
 *
 * Drawn as a few rings, each fainter and one pixel further out, which is a
 * blur cheap enough to do per control per frame. */
static void soft_shadow(int x, int y, int w, int h, int r)
{
    if (!glass_on())
        return;
    /* Through the alpha-keeping path, which is the whole reason this looked
     * like a border rather than a shadow.
     *
     * draw_round_rect_outline composites with draw_over, and draw_over returns
     * something opaque. On the glass a client's buffer starts as transparent
     * black, so a ring of black at alpha 0x26 came out as *solid* black - and
     * three of them, one pixel apart, drew a hard dark outline around every
     * button and pill. It was a shadow the whole time; it just had its
     * transparency thrown away by the function drawing it.
     *
     * Fainter as well as translucent: a shadow that reads as an edge is too
     * dark whatever it is composited with. */
    static const unsigned kRing[3] = { 0x1Au, 0x10u, 0x08u };
    for (int i = 2; i >= 0; --i)
        wg_glass_outline(x - i, y - i + 1, w + 2 * i, h + 2 * i, r + i, 1,
                         kRing[i] << 24);
}

/* Source-over onto a buffer that may itself be see-through.
 *
 * draw_over assumes what is underneath is opaque and hands back something
 * opaque, which is right for painting onto a surface and wrong for painting
 * *into* one. On a window whose background is a wash with the alpha left in,
 * every antialiased edge came out solid and blended toward the wash instead of
 * toward whatever the server would later put behind it - which is why text on
 * the glass had a halo and looked heavier than it was.
 *
 * This keeps the alpha: an edge pixel stays partly transparent and the server
 * finishes the blend against the real backdrop, which is the only place the
 * right answer is known. */
static void blend_px(int x, int y, uint32_t over)
{
    if (g_px == 0 || x < 0 || y < 0 ||
        (unsigned)x >= g_w || (unsigned)y >= g_h || clipped_out(x, y))
        return;
    const unsigned ca = (over >> 24) & 0xFF;
    if (ca == 0)
        return;
    const unsigned long at = (unsigned long)y * g_w + (unsigned)x;
    const uint32_t under = g_px[at];
    const unsigned ua = (under >> 24) & 0xFF;
    if (ca == 255 || ua == 0) {
        g_px[at] = (ca << 24) | (over & 0x00FFFFFFu);
        return;
    }
    /* What the pixel underneath still contributes once this is over it. */
    const unsigned rest = ua * (255 - ca) / 255;
    const unsigned oa = ca + rest;
    if (oa == 0) {
        g_px[at] = 0;
        return;
    }
    const unsigned r = ((((over >> 16) & 0xFF) * ca) +
                        (((under >> 16) & 0xFF) * rest)) / oa;
    const unsigned g = ((((over >> 8) & 0xFF) * ca) +
                        (((under >> 8) & 0xFF) * rest)) / oa;
    const unsigned b = (((over & 0xFF) * ca) + ((under & 0xFF) * rest)) / oa;
    g_px[at] = (oa << 24) | ((r > 255 ? 255 : r) << 16) |
               ((g > 255 ? 255 : g) << 8) | (b > 255 ? 255 : b);
}

/* Fill a rounded shape with a chosen alpha, written rather than blended.
 *
 * The difference matters: blending would fold the colour into whatever the
 * buffer already held and hand back something opaque, which is right for a
 * control drawn on a surface and wrong for the surface itself. This leaves the
 * alpha in the pixel for the server to blend against the blur. */
/* A rounded outline that keeps its alpha.
 *
 * draw_round_rect_outline goes through draw_over, which hands back something
 * opaque because it assumes it is painting *onto* a surface. On the glass the
 * buffer starts as transparent black, so a translucent white line composited
 * that way comes out dark grey and solid - which is what every border on a
 * blurred window was, and why the buttons looked like slate.
 *
 * Coverage of the shape minus coverage of the shape one thickness in, blended
 * per pixel: the same edge with the transparency left in it. */
void wg_glass_outline(int x, int y, int w, int h, int radius, int thick,
                      uint32_t argb)
{
    const unsigned a = (argb >> 24) & 0xFF;
    const uint32_t rgb = argb & 0x00FFFFFFu;
    if (thick < 1) thick = 1;
    for (int py = y; py < y + h; ++py) {
        if (py < 0 || (unsigned)py >= g_h)
            continue;
        for (int px = x; px < x + w; ++px) {
            if (px < 0 || (unsigned)px >= g_w)
                continue;
            const int outer = draw_round_coverage(px, py, x, y, w, h, radius);
            if (outer == 0)
                continue;
            const int inner = draw_round_coverage(px, py, x + thick, y + thick,
                                                  w - 2 * thick, h - 2 * thick,
                                                  radius > thick ? radius - thick
                                                                 : 0);
            const int edge = outer - inner;
            if (edge <= 0)
                continue;
            const unsigned pa = (a * (unsigned)edge + 127) / 255;
            blend_px(px, py, (pa << 24) | rgb);
        }
    }
}

void wg_glass_fill(int x, int y, int w, int h, int radius, uint32_t argb)
{
    const unsigned a = (argb >> 24) & 0xFF;
    const uint32_t rgb = argb & 0x00FFFFFFu;
    for (int py = y; py < y + h; ++py) {
        if (py < 0 || (unsigned)py >= g_h)
            continue;
        /* Only the rows level with a corner are curved. Everything between
         * them is a straight run at full coverage, and asking for the
         * distance to a rounded rectangle once per pixel of a window-sized
         * box was most of what this cost. */
        const int straight = py >= y + radius && py < y + h - radius;
        for (int px = x; px < x + w; ++px) {
            if (px < 0 || (unsigned)px >= g_w)
                continue;
            const int cov = straight ? 255
                          : draw_round_coverage(px, py, x, y, w, h, radius);
            if (cov == 0)
                continue;
            /* Composited onto the background, not written over it. Writing
             * made a half-covered corner *more* see-through than the surface
             * around it, so every rounded box had four dark notches where its
             * corners punched a hole in the window. */
            const unsigned pa = (a * (unsigned)cov + 127) / 255;
            blend_px(px, py, (pa << 24) | rgb);
        }
    }
}

/* A window's own background.
 *
 * With the glass on this is a wash of white with the alpha left in it, so the
 * server blends it over the blurred backdrop and the interior becomes the same
 * material as the title bar. With the glass off there is nothing behind to
 * show through, so it is the flat panel colour, opaque. */
void wg_glass_clear(void)
{
    if (g_px == 0)
        return;
    /* Nothing at all, when the glass is on.
     *
     * The server already washes the whole frame - title bar and body alike -
     * before a client's pixels reach it. A client that washed its body again
     * was laying a second coat over the first, so the body came out more
     * opaque than the title bar above it: the same colour, mixed twice. Left
     * clear, the one wash the server applies is the only one, and the two are
     * identical because they are literally the same paint. */
    const uint32_t c = glass_on() ? 0x00000000u : wg_base_colour();
    for (unsigned i = 0; i < g_w * g_h; ++i)
        g_px[i] = c;
}

/* A container: a box holding other things.
 *
 * `pad` is how much room there is between it and the window's edge, and the
 * corner follows from it. A box pressed against the edge wants a small radius
 * or it fights the window's own corner; one floating in the middle of a lot of
 * space can afford a large one. This is the rule the whole layout reads by,
 * so it lives here rather than in each window's idea of a nice number. */
int wg_container_radius(int pad)
{
    int r = 4 + pad;
    if (r > 14) r = 14;
    return r;
}

void wg_container(int x, int y, int w, int h, int pad)
{
    const struct surface c = canvas();
    const int r = wg_container_radius(pad);
    if (glass_on())
        wg_glass_fill(x, y, w, h, r, 0x30FFFFFFu);
    else
        draw_round_rect(&c, x, y, w, h, r, darken(wg_base_colour(), 14));
    inner_glow(x, y, w, h, r);
}

/* The selected row, wherever a list has one.
 *
 * This exists because there were as many answers as there were lists: a white
 * pill here, a flat blue rectangle there, a square-cornered wash somewhere
 * else - each app reaching for wg_fill and picking its own shape. They were
 * all "the selected thing", and they should look like it, so the shape lives
 * here now and the lists ask for it by name.
 *
 * On the glass it is a brighter pane of the same frosted material, because a
 * solid colour there would punch a hole in the blur. Opaque, it is the accent
 * colour, which is the one place a strong colour belongs. */
void wg_row_select(int x, int y, int w, int h)
{
    const struct surface c = canvas();
    const int r = h / 2 > WG_RADIUS ? WG_RADIUS : h / 2;
    if (glass_on()) {
        wg_glass_fill(x, y, w, h, r, 0x8CFFFFFFu);
        wg_glass_outline(x, y, w, h, r, 1, 0x66FFFFFFu);
    } else {
        draw_round_rect(&c, x, y, w, h, r, 0xFF000000u | g_sel);
    }
}

/* A sidebar: full height, and a shade apart from the content beside it.
 *
 * Square on the outside because it runs into the window's own edges, which
 * already have corners of their own - a rounded sidebar inside a rounded
 * window is two curves fighting over the same pixels. */
void wg_sidebar(int x, int y, int w, int h)
{
    const struct surface c = canvas();
    if (glass_on())
        /* The same amount the server adds over the title bar, so the column
         * is one tone from the very top of the window. */
        wg_glass_fill(x, y, w, h, 0, 0x33FFFFFFu);
    else
        draw_round_rect(&c, x, y, w, h, 0, darken(wg_base_colour(), 18));
    /* No line down the inside edge. The change of tone is the edge, and a
     * hairline on top of it is the drawn border this design does without. */
}

/* A row of controls sharing one pill.
 *
 * Things that belong together are drawn together: back and forward are one
 * control with two ends, and putting a gap between them says they are two
 * unrelated buttons that happen to be adjacent. Anything that does a different
 * kind of job gets a pill of its own.
 *
 * `count` segments of `seg_w` each. `selected` is the index drawn as pressed,
 * or -1 for none. */
void wg_pill_group(int x, int y, int seg_w, int h, int count,
                   const char* const* labels, int selected)
{
    if (count <= 0)
        return;
    const struct surface c = canvas();
    const int w = seg_w * count;
    const int r = h / 2;

    soft_shadow(x, y, w, h, r);
    if (glass_on())
        wg_glass_fill(x, y, w, h, r, 0x59FFFFFFu);
    else
        /* Darker than it was: at ten counts off the base this was a control
         * you had to look for against the window behind it. */
        draw_round_rect(&c, x, y, w, h, r, darken(wg_base_colour(), 26));

    /* The chosen segment, inside the pill rather than instead of it. */
    if (selected >= 0 && selected < count) {
        const int sx = x + selected * seg_w;
        if (glass_on())
            wg_glass_fill(sx + 1, y + 1, seg_w - 2, h - 2, r - 1, 0xB3FFFFFFu);
        else
            draw_round_rect(&c, sx + 1, y + 1, seg_w - 2, h - 2, r - 1,
                            0x99FFFFFFu);
    }

    /* Hairlines between the segments, short of the ends so they do not cut
     * across the curve. */
    for (int i = 1; i < count; ++i)
        draw_rect(&c, x + i * seg_w, y + 3, 1, h - 6,
                  glass_on() ? 0x24FFFFFFu : 0x18000000u);

    /* An outline on the glass and none without it. The translucent white one
     * is what gives a frosted pane an edge when the thing behind it could be
     * any colour; the dark one it used to draw when opaque was a drawn border
     * on a design that otherwise has none, and it made every control look
     * stuck onto the window rather than part of it. */
    if (glass_on())
        wg_glass_outline(x, y, w, h, r, 1, 0x59FFFFFFu);
    inner_glow(x, y, w, h, r);

    for (int i = 0; i < count; ++i) {
        if (labels[i] == 0)
            continue;
        const int tw = wg_text_width(labels[i]);
        wg_text(x + i * seg_w + (seg_w - tw) / 2,
                y + (h - wg_text_height()) / 2, labels[i],
                glass_on() ? 0xF0101810u : wg_ink_colour());
    }
}

/* One control on its own, which is the same shape with one segment. */
void wg_pill(int x, int y, int w, int h, const char* label, int down)
{
    const char* one[1];
    one[0] = label;
    wg_pill_group(x, y, w, h, 1, one, down ? 0 : -1);
}

void wg_button(int x, int y, int w, int h, const char* label, int down)
{
    const struct surface c = canvas();
    /* A soft pill: a wash of white, brighter while it is held. Nothing moves
     * by a pixel when pressed - the old chrome shifted its label down and
     * right to fake the button going in, and a change of tone says it without
     * the text jumping. */
    soft_shadow(x, y, w, h, WG_RADIUS);
    if (glass_on()) {
        wg_glass_fill(x, y, w, h, WG_RADIUS, down ? 0xA6FFFFFFu : 0x73FFFFFFu);
        wg_glass_outline(x, y, w, h, WG_RADIUS, 1, 0x59FFFFFFu);
    } else {
        /* A wash of white over a window that is already almost white is not a
         * button, it is a rumour of one. Opaque, it gets a tone of its own and
         * no outline: the tone is the edge. */
        draw_round_rect(&c, x, y, w, h, WG_RADIUS,
                        darken(wg_base_colour(), down ? 46 : 26));
    }
    inner_glow(x, y, w, h, WG_RADIUS);

    const int tw = wg_text_width(label);
    wg_text(x + (w - tw) / 2, y + (h - wg_text_height()) / 2, label,
            glass_on() ? 0xF0101810u : wg_ink_colour());
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

/* --- the controls that were still 1991 --------------------------------------
 *
 * A checkbox drawn as a sunken white square with a bevel round it, and a radio
 * button drawn as the same square with a dot in it, are the two things that
 * gave the whole interface away however carefully everything around them was
 * redrawn. These are the same controls in the vocabulary the rest of it uses:
 * a rounded well, a wash, a bright lip, and the accent colour to say yes.
 */

/* A tick, drawn rather than loaded: two strokes, thickened by drawing each of
 * them three times a pixel apart. */
static void tick(int x, int y, int size, uint32_t ink)
{
    const int a = size / 4, b = size / 2;
    for (int t = 0; t < 2; ++t)
        for (int i = 0; i <= a; ++i) {
            blend_px(x + b - a + i + t, y + b + i, 0xFF000000u | ink);
            blend_px(x + b + i + t, y + b + a - i, 0xFF000000u | ink);
        }
}

void wg_check(int x, int y, int size, int on)
{
    const struct surface c = canvas();
    const int r = size / 3;
    if (on) {
        draw_round_rect(&c, x, y, size, size, r, 0xFF000000u | wg_sel_colour());
        tick(x, y, size, 0xFFFFFF);
    } else if (glass_on()) {
        wg_glass_fill(x, y, size, size, r, 0x59FFFFFFu);
        wg_glass_outline(x, y, size, size, r, 1, 0x8CFFFFFFu);
    } else {
        /* An empty box needs an edge to be a box at all.
         *
         * Twelve counts off the window's own colour was almost visible when
         * the window was nearly white, and stopped being visible at all once
         * windows took a tint from the theme - an unticked checkbox simply was
         * not there. This is the one place a drawn outline is the control
         * rather than decoration on it, which is why it is the exception to
         * having removed them. */
        draw_round_rect(&c, x, y, size, size, r, darken(wg_base_colour(), 20));
        draw_round_rect_outline(&c, x, y, size, size, r, 1,
                                darken(wg_base_colour(), 90));
    }
    inner_glow(x, y, size, size, r);
}

void wg_radio(int x, int y, int size, int on)
{
    const struct surface c = canvas();
    const int r = size / 2;
    if (glass_on()) {
        wg_glass_fill(x, y, size, size, r, 0x59FFFFFFu);
        wg_glass_outline(x, y, size, size, r, 1, 0x8CFFFFFFu);
    } else {
        draw_round_rect(&c, x, y, size, size, r, darken(wg_base_colour(), 20));
        draw_round_rect_outline(&c, x, y, size, size, r, 1,
                                darken(wg_base_colour(), 90));
    }
    if (on) {
        const int inset = size / 4;
        draw_round_rect(&c, x + inset, y + inset, size - inset * 2,
                        size - inset * 2, (size - inset * 2) / 2,
                        0xFF000000u | wg_sel_colour());
    }
    inner_glow(x, y, size, size, r);
}

/* A field is a container that has been pressed in rather than raised: the same
 * shape, darker instead of lighter, and no bright lip along the top - light
 * does not collect on the upper edge of something recessed. */
void wg_field(int x, int y, int w, int h, const char* text, int focused)
{
    const struct surface c = canvas();
    const int r = h / 3 > 8 ? 8 : h / 3;
    if (glass_on())
        wg_glass_fill(x, y, w, h, r, 0x1FFFFFFFu);
    else
        draw_round_rect(&c, x, y, w, h, r, darken(wg_base_colour(), 22));
    if (focused)
        draw_round_rect_outline(&c, x, y, w, h, r, 1,
                                0xCC000000u | wg_sel_colour());
    if (text != 0)
        wg_text(x + 8, y + (h - wg_text_height()) / 2, text, wg_ink_colour());
}

/* --- grouped settings rows ---------------------------------------------------
 *
 * A page of settings is not a flat list of controls. It is a handful of small
 * groups, each with a heading, and a row inside a group is a label on the left
 * and its control on the right. That shape is what makes a long page readable
 * without every window inventing its own spacing for it.
 *
 * The group is a container; the rows are hairlines between, not boxes within,
 * because a box inside a box is the thing this design keeps having to remove.
 */
#define WG_ROW_H 30

void wg_group_begin(int x, int y, int w, int rows, const char* title)
{
    if (title != 0 && title[0] != '\0')
        wg_text(x + 4, y - wg_text_height() - 4, title, WG_DIM);
    wg_container(x, y, w, rows * WG_ROW_H, 10);
}

/* The label side of one row, and where its control should go. */
int wg_row(int x, int y, int w, int index, const char* label)
{
    const int ry = y + index * WG_ROW_H;
    if (index > 0)
        draw_rect(&(struct surface){ g_px, (int)g_w, (int)g_h, 0, 0, 0, 0 },
                  x + 12, ry, w - 24, 1,
                  glass_on() ? 0x1AFFFFFFu : 0x14000000u);
    wg_text(x + 14, ry + (WG_ROW_H - wg_text_height()) / 2, label,
            wg_ink_colour());
    return ry + (WG_ROW_H - 22) / 2;    /* where a 22-tall control sits */
}
