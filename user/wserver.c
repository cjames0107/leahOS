/* wserver - the window server, as an ordinary process.
 *
 * It owns three things the kernel hands it and nothing else: the framebuffer,
 * the raw input devices, and a public shared-memory block that clients find by
 * a well-known key. Everything a window is - its geometry, its title, its event
 * queue - lives in that block, and its pixels live in a segment of the client's
 * own. Compositing is then just reading other processes' memory and writing the
 * screen, with no kernel involvement at all beyond the page tables.
 *
 * This used to be in the kernel, where it had the run of every structure it
 * needed. Moving it out cost two things and bought one. It cost a rendezvous -
 * hence the well-known key - and it cost the ability to be told when a client
 * dies, so the server has to notice for itself. What it bought is that a bug
 * here is a segfault in a process rather than a panic, and that the kernel no
 * longer has an opinion about what a window looks like.
 *
 * The look is deliberately of its period - grey chrome, bevelled edges, a title
 * bar and a close box - because that style is what a bitmap font and a handful
 * of flat colours are actually good at.
 */

#include <display.h>
#include <draw.h>
#include <font.h>
#include <image.h>
#include <time.h>
#include <paths.h>
#include <shm.h>
#include <sys/mman.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <svg.h>
#include <unistd.h>
#include <wproto.h>

/* --- the palette ---------------------------------------------------------
 *
 * Two shades either side of the face colour is all a bevel needs: light above
 * and left, dark below and right, and the eye reads it as raised. It is the
 * whole visual language of this era of interface. */
/* The palette is no longer fixed: it is read from the control block so that
 * settings can change it. These are only the defaults the server starts with. */
#define OUTLINE      0x000000
#define DESKTOP      (g_control->theme.desktop)
#define FACE         (g_control->theme.face)
/* Contrast pushes the bevel shades apart, or together, around the face. */
#define LIGHT        tint(g_control->theme.light,  g_control->theme.contrast)
#define SHADOW       tint(g_control->theme.shadow, -g_control->theme.contrast)
#define TITLE_ACTIVE (g_control->theme.title_active)
#define TITLE_IDLE   (g_control->theme.title_idle)
#define TITLE_TEXT   (g_control->theme.title_text)
#define CURSOR_FILL  (g_control->theme.cursor)

#define TITLE_HEIGHT WS_TITLE_HEIGHT
#define BORDER       WS_BORDER
#define CLOSE_SIZE   12
#define CORNER       WS_CORNER
/* How far a shadow reaches past the window, and how far it falls. Both are
 * needed outside the frame drawing, because the damage rectangle has to cover
 * the shadow or it is composed into a region that is then thrown away. */
#define SHADOW_BLUR   9
#define SHADOW_DROP   6
#define SHADOW_SPREAD (SHADOW_BLUR * 2 + SHADOW_DROP)
#define CONTROL_SIZE 13         /* one title-bar control, square */
/* The grip bar along the bottom, and the grow box sitting at its right end.
 * A strip of its own rather than a corner overlapping the content, so a click
 * near the bottom-right of a window is unambiguously a resize and never a
 * stroke the client was expecting. */
/* The resize band along the bottom. It is part of the frame and the panel
 * covers it, so nothing is drawn in it - a grow box with three diagonal lines
 * belongs to the chrome this replaces. Its height is what a pointer needs to
 * find, not what an eye needs to see. */
#define GRIP_H       8
#define GRIP_W       16

/* Polling rates. Fast enough that the pointer does not feel detached, slow
 * enough that the server is not a busy-wait. */
#define kFrameSleepMs 10
#define kIdleSleepMs  10

#define CURSOR_W 12
#define CURSOR_H 19

/* A plain arrow. 0 is transparent, 1 the black outline, 2 the white fill - an
 * outline is what makes a cursor visible over both light and dark windows. */
static const unsigned char kCursor[CURSOR_H][CURSOR_W] = {
    {1,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0,0},
    {1,2,1,0,0,0,0,0,0,0,0,0},
    {1,2,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0,0},
    {1,2,2,2,2,2,1,0,0,0,0,0},
    {1,2,2,2,2,2,2,1,0,0,0,0},
    {1,2,2,2,2,2,2,2,1,0,0,0},
    {1,2,2,2,2,2,2,2,2,1,0,0},
    {1,2,2,2,2,2,2,2,2,2,1,0},
    {1,2,2,2,2,2,2,1,1,1,1,1},
    {1,2,2,2,1,2,2,1,0,0,0,0},
    {1,2,2,1,1,2,2,1,0,0,0,0},
    {1,2,1,0,0,1,2,2,1,0,0,0},
    {1,1,0,0,0,1,2,2,1,0,0,0},
    {1,0,0,0,0,0,1,2,2,1,0,0},
    {0,0,0,0,0,0,1,2,2,1,0,0},
    {0,0,0,0,0,0,0,1,1,0,0,0},
};

static struct ws_shared* g_control;
static struct fb_info    g_fb;
static unsigned char*    g_screen;      /* the framebuffer, as bytes */
static uint32_t*         g_back;        /* the whole screen, composed off-line */
static unsigned char     g_font[256 * 16];

/* Per-slot server-side state. The client owns the shared block; this is what
 * only the server needs to know. */
static uint32_t* g_pixels[WS_MAX_WINDOWS];   /* the client's pixels, mapped here */
static uint32_t  g_mapped_gen[WS_MAX_WINDOWS];
static uint32_t  g_pixel_gen[WS_MAX_WINDOWS];  /* which segment generation is mapped */
static unsigned long g_pixel_bytes[WS_MAX_WINDOWS];
/* The server's own copy of each window's size, taken once and checked. The
 * client's fields stay writable by the client; these do not. */
static unsigned  g_width[WS_MAX_WINDOWS];
static unsigned  g_height[WS_MAX_WINDOWS];
static int       g_order[WS_MAX_WINDOWS];    /* front to back; [0] is focused */
static int       g_count;

/* --- damage ---------------------------------------------------------------
 *
 * Recomposing the whole screen to show a blinking cursor or a moved window is
 * most of a megabyte of work for a few hundred pixels of change. Instead each
 * thing that changes says which rectangle it changed, and a pass recomposes and
 * blits only those - the rest of the backbuffer is still correct from last
 * time, which is also what lets the cursor be erased by copying back out of it.
 *
 * The list is short and merges greedily. Being imprecise here costs a few
 * redundant pixels; being wrong costs a stale screen, so overlapping rectangles
 * are unioned rather than tracked separately. */
struct rect { int x, y, w, h; };

#define WS_MAX_DAMAGE 12
static struct rect g_damage[WS_MAX_DAMAGE];
static int         g_damage_count;

/* The rectangle currently being composed. Drawing outside it is not merely
 * wasted - it would write parts of the backbuffer this pass is not going to
 * blit, so what is on screen and what is in the buffer would drift apart. */
static struct rect g_clip;

static int g_cursor_x, g_cursor_y;
static int g_last_cursor_x = -1, g_last_cursor_y = -1;
static int g_dragging = -1, g_drag_dx, g_drag_dy;
/* Resizing draws a rubber-band outline and commits on release, rather than
 * reallocating the client's segment on every pixel of pointer movement. */
static int g_resizing = -1;
static int g_resize_w, g_resize_h;
static int g_resize_start_w, g_resize_start_h;
static int g_resize_from_x, g_resize_from_y;
static struct rect g_band;
static int g_band_shown;
static int g_mouse_grab = -1;
static int g_last_left;
static int g_last_right;

/* Shift a colour towards white or black by `amount` percent. Used for the
 * bevels, which is where contrast actually lives: the face colour stays put and
 * the light and shadow move apart or together around it. */
static uint32_t tint(uint32_t c, int amount)
{
    int r = (int)((c >> 16) & 0xFF), g = (int)((c >> 8) & 0xFF), b = (int)(c & 0xFF);
    if (amount >= 0) {
        r += (255 - r) * amount / 100;
        g += (255 - g) * amount / 100;
        b += (255 - b) * amount / 100;
    } else {
        r += r * amount / 100;
        g += g * amount / 100;
        b += b * amount / 100;
    }
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static int imax(int a, int b) { return a > b ? a : b; }
static int imin(int a, int b) { return a < b ? a : b; }

static int rects_overlap(const struct rect* a, const struct rect* b)
{
    return a->x < b->x + b->w && b->x < a->x + a->w &&
           a->y < b->y + b->h && b->y < a->y + a->h;
}

static void damage_rect(int x, int y, int w, int h)
{
    /* Clip to the screen first, so nothing downstream has to think about it. */
    const int x0 = imax(x, 0), y0 = imax(y, 0);
    const int x1 = imin(x + w, (int)g_fb.width), y1 = imin(y + h, (int)g_fb.height);
    if (x1 <= x0 || y1 <= y0)
        return;

    struct rect r;
    r.x = x0; r.y = y0; r.w = x1 - x0; r.h = y1 - y0;

    /* Merged until nothing overlaps anything, not merged once.
     *
     * The damage list has to be disjoint, and this used to union a new
     * rectangle into the first one it touched and return - leaving the grown
     * rectangle overlapping others further down the list. Two overlapping
     * regions are not merely composed twice: compose_rect repaints the
     * wallpaper across each one, so the second pass erases whatever the first
     * drew in the shared area and only redraws the windows whose frames
     * overlap *that* rectangle. Any other window is left as background.
     *
     * It survived because frame-sized rectangles rarely touched. Growing them
     * to cover a shadow made it happen constantly, and looked like the shadow
     * having broken the frames.
     *
     * Merging by taking the union out, removing the slot and re-inserting
     * handles the transitive case for free: the enlarged rectangle goes back
     * through the same test against everything that is left. */
    for (int i = 0; i < g_damage_count; ++i) {
        if (!rects_overlap(&r, &g_damage[i]))
            continue;
        const int nx = imin(r.x, g_damage[i].x), ny = imin(r.y, g_damage[i].y);
        const int mx = imax(r.x + r.w, g_damage[i].x + g_damage[i].w);
        const int my = imax(r.y + r.h, g_damage[i].y + g_damage[i].h);
        g_damage[i] = g_damage[--g_damage_count];
        damage_rect(nx, ny, mx - nx, my - ny);
        return;
    }

    if (g_damage_count < WS_MAX_DAMAGE) {
        g_damage[g_damage_count++] = r;
        return;
    }
    /* Out of room: collapse everything into one bounding rectangle rather than
     * dropping a region and leaving the screen stale. */
    for (int i = 1; i < g_damage_count; ++i) {
        const int nx = imin(g_damage[0].x, g_damage[i].x);
        const int ny = imin(g_damage[0].y, g_damage[i].y);
        const int mx = imax(g_damage[0].x + g_damage[0].w,
                            g_damage[i].x + g_damage[i].w);
        const int my = imax(g_damage[0].y + g_damage[0].h,
                            g_damage[i].y + g_damage[i].h);
        g_damage[0].x = nx; g_damage[0].y = ny;
        g_damage[0].w = mx - nx; g_damage[0].h = my - ny;
    }
    g_damage_count = 1;
    damage_rect(r.x, r.y, r.w, r.h);
}

static void damage_all(void)
{
    g_damage_count = 0;
    damage_rect(0, 0, (int)g_fb.width, (int)g_fb.height);
}

/* --- drawing into the backbuffer ---------------------------------------- */

/* --- soft edges --------------------------------------------------------------
 *
 * Square corners and a hard black edge. There was a version of this with
 * rounded corners and a soft drop shadow; it looked like something from
 * twenty years later, which is the wrong twenty years. A window here is a
 * grey panel with a bevel inside a black outline, and nothing outside it. */

/* Defined below, where fill() exists; used above it, where the frame is drawn. */

static void back_plot(int x, int y, uint32_t colour)
{
    if (x < g_clip.x || y < g_clip.y ||
        x >= g_clip.x + g_clip.w || y >= g_clip.y + g_clip.h)
        return;
    g_back[(unsigned)y * g_fb.width + (unsigned)x] = colour;
}

/* The same, blended. The old chrome never needed this - every pixel it drew
 * was opaque - and the new one needs it for almost every pixel it draws. */
static void back_blend(int x, int y, uint32_t colour)
{
    if (x < g_clip.x || y < g_clip.y ||
        x >= g_clip.x + g_clip.w || y >= g_clip.y + g_clip.h)
        return;
    uint32_t* p = &g_back[(unsigned)y * g_fb.width + (unsigned)x];
    *p = draw_over(*p, colour);
}

/* --- the new chrome ---------------------------------------------------------
 *
 * A window is a pane of frosted glass: what is behind it, blurred; a wash of
 * white over that; a hairline round the edge; and a shadow underneath. All
 * four come from draw.c, and all this has to do is hand them the damage
 * rectangle so nothing is painted outside the region being recomposed.
 */

static struct font*    g_typeface;      /* the proportional one, for titles */
static struct svg_icon g_close_glyph;
static int             g_close_ready;

/* The back buffer as draw.c wants it, narrowed to whatever is being composed.
 * Rebuilt per call rather than kept, because the clip changes every time and a
 * stale one is a corrupted screen rather than a wrong pixel. */
static struct surface canvas(void)
{
    struct surface s;
    s.pixels = g_back;
    s.w = (int)g_fb.width;
    s.h = (int)g_fb.height;
    s.cx = g_clip.x;
    s.cy = g_clip.y;
    s.cw = g_clip.w;
    s.ch = g_clip.h;
    return s;
}

/* One shadow per window size. A shadow depends only on the shape casting it,
 * never on what is inside, so this is the difference between blurring one per
 * window per frame and blurring one per size ever. */
#define SHADOW_CACHE 8
static struct { int w, h; struct shadow* shadow; } g_shadows[SHADOW_CACHE];
static int g_shadow_next;

static struct shadow* shadow_for(int w, int h)
{
    for (int i = 0; i < SHADOW_CACHE; ++i)
        if (g_shadows[i].shadow != 0 && g_shadows[i].w == w &&
            g_shadows[i].h == h)
            return g_shadows[i].shadow;

    struct shadow* made = draw_shadow_make(w, h, CORNER, SHADOW_BLUR, 80);
    if (made == 0)
        return 0;
    const int slot = g_shadow_next;
    g_shadow_next = (g_shadow_next + 1) % SHADOW_CACHE;
    if (g_shadows[slot].shadow != 0)
        draw_shadow_free(g_shadows[slot].shadow);
    g_shadows[slot].w = w;
    g_shadows[slot].h = h;
    g_shadows[slot].shadow = made;
    return made;
}

static int text_width(int px, const char* text)
{
    return g_typeface != 0 ? font_width(g_typeface, px, text)
                           : (int)strlen(text) * 8;
}

/* Proportional text, blended per pixel through the glyph's coverage. */
static void draw_string(int x, int baseline, int px, const char* text,
                        uint32_t colour)
{
    if (g_typeface == 0)
        return;
    const unsigned alpha = (colour >> 24) & 0xFF;
    const uint32_t rgb = colour & 0x00FFFFFFu;
    const char* at = text;
    for (;;) {
        const unsigned c = utf8_next(&at);
        if (c == 0)
            break;
        struct glyph g;
        if (font_glyph(g_typeface, px, c, &g) != 0)
            continue;
        for (int gy = 0; gy < g.h; ++gy)
            for (int gx = 0; gx < g.w; ++gx) {
                const unsigned cov = g.coverage[(long)gy * g.w + gx];
                if (cov == 0)
                    continue;
                back_blend(x + g.left + gx, baseline - g.top + gy,
                           (((alpha * cov + 127) / 255) << 24) | rgb);
            }
        x += g.advance;
    }
}

/* Where the three controls sit, left to right: close, minimise, maximise.
 * `which` is 0, 1 or 2. */
static void control_box(const struct ws_window* w, int which, int* x, int* y)
{
    *x = w->x + BORDER + 8 + which * (CONTROL_SIZE + 9);
    *y = w->y + BORDER + (TITLE_HEIGHT - CONTROL_SIZE) / 2;
}


/* Sizes come from the server's validated copy, never from the client's fields. */
/* A desktop window is all content: no border, no title bar, no grip. */
static int is_desktop(int slot)
{
    return (g_control->windows[slot].flags & WS_FLAG_DESKTOP) != 0;
}

static unsigned frame_width(int slot)
{
    return is_desktop(slot) ? g_width[slot] : g_width[slot] + BORDER * 2;
}
static unsigned frame_height(int slot)
{
    return is_desktop(slot) ? g_height[slot]
                            : g_height[slot] + BORDER * 2 + TITLE_HEIGHT + GRIP_H;
}

/* The whole window, frame included, as a rectangle - which is what damage is
 * expressed in. */
static struct rect frame_rect(int slot)
{
    struct ws_window* w = &g_control->windows[slot];
    struct rect r;
    r.x = w->x; r.y = w->y;
    r.w = (int)frame_width(slot); r.h = (int)frame_height(slot);
    return r;
}

/* --- the blurred backdrop, kept ---------------------------------------------
 *
 * Blurring what is behind a window costs about seventy times what copying the
 * answer costs - 1.7 milliseconds against 0.024 for a window of any size worth
 * having - and most of the time the answer has not changed. A terminal
 * printing a line, a clock ticking, a menu opening: in every one of those the
 * window's own contents change and what is *behind* it does not.
 *
 * So the blurred backdrop is kept per window and rebuilt only when something
 * underneath it actually moved. What counts as underneath is precise: this
 * window's own geometry, the wallpaper, and the position, size and generation
 * of every window below it in the stacking order. Its own generation is
 * deliberately absent - a window redrawing itself changes nothing behind it,
 * and that is the case this exists for.
 *
 * Dragging is not helped and cannot be: a window in motion has a different
 * backdrop at every position, so every frame is a genuine miss. That is not a
 * shortcoming of the cache, it is what moving a window means.
 */
static uint32_t* g_blur[WS_MAX_WINDOWS];
static int       g_blur_w[WS_MAX_WINDOWS], g_blur_h[WS_MAX_WINDOWS];
static unsigned  g_blur_stamp[WS_MAX_WINDOWS];
static int       g_blur_valid[WS_MAX_WINDOWS];

/* Bumped whenever a window is going to be repainted, which is exactly when
 * anything about it that a window above could see has changed. */
static unsigned  g_generation[WS_MAX_WINDOWS];
static unsigned  g_paper_generation;

static void blur_cache_drop(int slot)
{
    free(g_blur[slot]);
    g_blur[slot] = 0;
    g_blur_valid[slot] = 0;
}

static void mix(unsigned* h, unsigned v)
{
    *h = (*h ^ v) * 16777619u;          /* FNV-1a, one word at a time */
}

static unsigned backdrop_stamp(int slot)
{
    const struct ws_window* w = &g_control->windows[slot];
    unsigned h = 2166136261u;

    mix(&h, (unsigned)w->x);
    mix(&h, (unsigned)w->y);
    mix(&h, frame_width(slot));
    mix(&h, frame_height(slot));
    mix(&h, g_paper_generation);

    /* Everything below this one. Where it sits in the order is part of the
     * answer too: raising a window changes what is behind the ones it passes. */
    int at = 0;
    while (at < g_count && g_order[at] != slot)
        ++at;
    for (int i = at + 1; i < g_count; ++i) {
        const int below = g_order[i];
        const struct ws_window* b = &g_control->windows[below];
        mix(&h, (unsigned)below);
        mix(&h, (unsigned)b->x);
        mix(&h, (unsigned)b->y);
        mix(&h, g_width[below]);
        mix(&h, g_height[below]);
        mix(&h, g_generation[below]);
    }
    return h;
}

static void damage_window(int slot)
{
    ++g_generation[slot];

    /* The frame, grown to cover the shadow it casts. A damage rectangle that
     * stops at the window's edge clips the shadow away entirely. */
    const struct rect r = frame_rect(slot);
    damage_rect(r.x - SHADOW_SPREAD, r.y - SHADOW_SPREAD + SHADOW_DROP,
                r.w + SHADOW_SPREAD * 2, r.h + SHADOW_SPREAD * 2);
}

/* The grow box, bottom-right, inside the grip bar. */
static void grow_box(int slot, int* x, int* y)
{
    struct ws_window* w = &g_control->windows[slot];
    *x = w->x + (int)frame_width(slot) - BORDER - GRIP_W;
    *y = w->y + (int)frame_height(slot) - BORDER - GRIP_H;
}

/* Where the close box sits, in screen coordinates. On the left, as this era of
 * interface had it. */
static void close_box(struct ws_window* w, int* x, int* y)
{
    *x = w->x + BORDER + 3;
    *y = w->y + BORDER + 3;
}

/* What sits behind a window's own contents: frosted glass, or a flat panel.
 *
 * The blurred form is copied if it is still good and blurred if it is not. It
 * can only be *captured* when the damage rectangle covers the whole window,
 * because outside that rectangle the back buffer still holds the previous
 * frame - including this window's own panel from last time, which blurred back
 * into itself would smear a little more with every repaint. So a partial
 * repaint with a stale stamp falls back to blurring what it can see, and the
 * cache is filled the next time the whole window is painted.
 */
static void paint_backdrop(int slot, const struct surface* c)
{
    const struct ws_window* w = &g_control->windows[slot];
    const unsigned fw = frame_width(slot), fh = frame_height(slot);

    if (g_control->theme.blur == 0) {
        /* Nothing behind to sample, so nothing to cache either. The wash and
         * the hairline the caller draws still apply, which is what keeps the
         * focused window distinguishable once the glass is gone. */
        draw_round_rect(c, w->x, w->y, (int)fw, (int)fh, CORNER,
                        0xFF000000u | (g_control->theme.face & 0x00FFFFFFu));
        if (g_blur[slot] != 0)
            blur_cache_drop(slot);
        return;
    }

    const unsigned stamp = backdrop_stamp(slot);
    const int covers_all = g_clip.x <= w->x && g_clip.y <= w->y &&
                           g_clip.x + g_clip.w >= w->x + (int)fw &&
                           g_clip.y + g_clip.h >= w->y + (int)fh;

    if (g_blur_valid[slot] && g_blur_stamp[slot] == stamp &&
        g_blur_w[slot] == (int)fw && g_blur_h[slot] == (int)fh) {
        /* A hit: copy back only the rows the clip actually wants. */
        const int x0 = imax(w->x, g_clip.x), y0 = imax(w->y, g_clip.y);
        const int x1 = imin(w->x + (int)fw, g_clip.x + g_clip.w);
        const int y1 = imin(w->y + (int)fh, g_clip.y + g_clip.h);
        if (x1 <= x0)
            return;
        for (int y = y0; y < y1; ++y)
            memcpy(&g_back[(unsigned)y * g_fb.width + (unsigned)x0],
                   &g_blur[slot][(long)(y - w->y) * fw + (x0 - w->x)],
                   (size_t)(x1 - x0) * sizeof(uint32_t));
        return;
    }

    draw_blur(c, w->x, w->y, (int)fw, (int)fh, 22, CORNER);

    if (!covers_all) {
        g_blur_valid[slot] = 0;
        return;
    }
    if (g_blur[slot] != 0 &&
        (g_blur_w[slot] != (int)fw || g_blur_h[slot] != (int)fh))
        blur_cache_drop(slot);
    if (g_blur[slot] == 0)
        g_blur[slot] = (uint32_t*)malloc((size_t)fw * fh * sizeof(uint32_t));
    if (g_blur[slot] == 0)
        return;
    for (unsigned y = 0; y < fh; ++y)
        memcpy(&g_blur[slot][(long)y * fw],
               &g_back[(unsigned)(w->y + (int)y) * g_fb.width + (unsigned)w->x],
               (size_t)fw * sizeof(uint32_t));
    g_blur_w[slot] = (int)fw;
    g_blur_h[slot] = (int)fh;
    g_blur_stamp[slot] = stamp;
    g_blur_valid[slot] = 1;
}

/* The panel colour when there is no glass: the face with the wash already
 * folded into it. Blending a translucent wash over an opaque fill gives an
 * opaque result, so doing it once here saves painting every pixel of every
 * window a second time to reach the same colour. */
static uint32_t opaque_panel(int focused)
{
    const uint32_t face = 0xFF000000u | (g_control->theme.face & 0x00FFFFFFu);
    return draw_over(face, focused ? 0x66FFFFFFu : 0x4DF2F2F2u);
}

/* One band of the frame, clipped to what is already being repainted. The
 * window's full geometry is still passed to draw_round_rect so that a band
 * containing a corner still gets the curve right. */
static void fill_band(struct surface c, int bx, int by, int bw, int bh,
                      int wx, int wy, int fw, int fh, uint32_t colour)
{
    if (bw <= 0 || bh <= 0)
        return;
    const int x0 = imax(bx, c.cx), y0 = imax(by, c.cy);
    const int x1 = imin(bx + bw, c.cx + c.cw), y1 = imin(by + bh, c.cy + c.ch);
    if (x1 <= x0 || y1 <= y0)
        return;
    c.cx = x0; c.cy = y0; c.cw = x1 - x0; c.ch = y1 - y0;
    draw_round_rect(&c, wx, wy, fw, fh, CORNER, colour);
}

/* One band of the margin a shadow falls in, clipped to what is being
 * repainted. The shadow's own origin is unchanged so the shape stays right. */
static void cast_band(struct surface c, struct shadow* shade,
                      int bx, int by, int bw, int bh, int wx, int wy)
{
    if (bw <= 0 || bh <= 0)
        return;
    const int x0 = imax(bx, c.cx), y0 = imax(by, c.cy);
    const int x1 = imin(bx + bw, c.cx + c.cw), y1 = imin(by + bh, c.cy + c.ch);
    if (x1 <= x0 || y1 <= y0)
        return;
    c.cx = x0; c.cy = y0; c.cw = x1 - x0; c.ch = y1 - y0;
    draw_shadow_cast(&c, shade, wx, wy, SHADOW_DROP);
}

static void draw_window(int slot, int focused)
{
    struct ws_window* w = &g_control->windows[slot];
    const unsigned fw = frame_width(slot), fh = frame_height(slot);

    if (is_desktop(slot)) {
        /* Straight to the pixels: the client owns every one of them. */
        const uint32_t* dp = g_pixels[slot];
        if (dp == 0)
            return;
        const int x0 = imax(w->x, g_clip.x), y0 = imax(w->y, g_clip.y);
        const int x1 = imin(w->x + (int)g_width[slot], g_clip.x + g_clip.w);
        const int y1 = imin(w->y + (int)g_height[slot], g_clip.y + g_clip.h);
        /* Both axes, not just the rows. A run of zero or fewer pixels is a
         * negative length, and the byte count memcpy takes is unsigned. */
        if (x1 <= x0)
            return;
        for (int y = y0; y < y1; ++y) {
            const uint32_t* row = &dp[(unsigned long)(y - w->y) * g_width[slot]];
            memcpy(&g_back[(unsigned)y * g_fb.width + (unsigned)x0],
                   &row[x0 - w->x], (size_t)(x1 - x0) * sizeof(uint32_t));
        }
        return;
    }

    /* Frosted glass, in the order light would build it: a shadow underneath,
     * the backdrop blurred through the window's own shape, a wash of white
     * over that, and a hairline to catch the edge. */
    const struct surface c = canvas();

    /* A shadow falls outside the window, so when the damage lies entirely
     * within the frame every pixel of it would be painted over by the panel
     * before anyone saw it. Typing in a terminal is that case. */
    const int clip_inside = g_clip.x >= w->x && g_clip.y >= w->y &&
                            g_clip.x + g_clip.w <= w->x + (int)fw &&
                            g_clip.y + g_clip.h <= w->y + (int)fh;
    if (!clip_inside) {
        struct shadow* shade = shadow_for((int)fw, (int)fh);
        if (shade != 0) {
            /* Only the margin. Everything the shadow puts inside the frame is
             * painted over by the panel and then by the client, so casting it
             * there is work nobody ever sees - and a shadow is a per-pixel
             * blend over an area larger than the window, which made it the
             * most expensive thing left in an opaque frame. */
            const int m = SHADOW_SPREAD;
            const int ox = w->x - m, oy = w->y - m;
            const int ow = (int)fw + m * 2, oh = (int)fh + m * 2;
            cast_band(c, shade, ox, oy, ow, w->y - oy, w->x, w->y);
            cast_band(c, shade, ox, w->y + (int)fh, ow,
                      oy + oh - (w->y + (int)fh), w->x, w->y);
            cast_band(c, shade, ox, w->y, w->x - ox, (int)fh, w->x, w->y);
            cast_band(c, shade, w->x + (int)fw, w->y,
                      ox + ow - (w->x + (int)fw), (int)fh, w->x, w->y);
        }
    }

    const uint32_t* px = g_pixels[slot];

    if (g_control->theme.blur == 0) {
        /* No backdrop to sample, so the panel is one flat colour - and the
         * client is about to cover everything inside the frame, so only the
         * frame itself is worth painting. That turns a window-sized fill into
         * a title bar and three hairlines. */
        const uint32_t panel = opaque_panel(focused);
        if (px != 0) {
            const int cx0 = w->x + BORDER;
            const int cy0 = w->y + BORDER + TITLE_HEIGHT;
            const int cw = (int)g_width[slot], ch = (int)g_height[slot];
            fill_band(c, w->x, w->y, (int)fw, cy0 - w->y,
                      w->x, w->y, (int)fw, (int)fh, panel);
            fill_band(c, w->x, cy0 + ch, (int)fw,
                      w->y + (int)fh - (cy0 + ch),
                      w->x, w->y, (int)fw, (int)fh, panel);
            fill_band(c, w->x, cy0, cx0 - w->x, ch,
                      w->x, w->y, (int)fw, (int)fh, panel);
            fill_band(c, cx0 + cw, cy0, w->x + (int)fw - (cx0 + cw), ch,
                      w->x, w->y, (int)fw, (int)fh, panel);
        } else {
            draw_round_rect(&c, w->x, w->y, (int)fw, (int)fh, CORNER, panel);
        }
        if (g_blur[slot] != 0)
            blur_cache_drop(slot);
    } else {
        paint_backdrop(slot, &c);
        /* The focused window is a shade brighter and a shade more opaque.
         * That is the whole focus signal now - the pinstripes it replaces
         * were a way of saying this in one colour, which is a constraint that
         * has gone. */
        draw_round_rect(&c, w->x, w->y, (int)fw, (int)fh, CORNER,
                        focused ? 0x66FFFFFFu : 0x4DF2F2F2u);
    }
    draw_round_rect_outline(&c, w->x, w->y, (int)fw, (int)fh, CORNER, 1,
                            focused ? 0x59FFFFFFu : 0x33FFFFFFu);

    /* The controls and the title both live in the title bar; when the damage
     * does not reach it, drawing them - and measuring and shaping the text -
     * is pure loss. Terminal output never touches it. */
    const int bar_bottom = w->y + BORDER + TITLE_HEIGHT;
    if (g_clip.y >= bar_bottom || g_clip.y + g_clip.h <= w->y)
        goto contents;

    /* The three controls, left to right. Only the close box has a vector
     * glyph; a minimise is a bar and a maximise is a square outline, both of
     * which are quicker to draw than to load. */
    for (int which = 0; which < 3; ++which) {
        int bx, by;
        control_box(w, which, &bx, &by);
        const uint32_t ink = focused ? 0xD9101810u : 0x73101810u;

        if (which == 0 && g_close_ready) {
            svg_draw(&c, &g_close_glyph, bx, by, ink);
        } else if (which == 0) {
            /* No glyph loaded: an X of two strokes, so a window is never left
             * without a way to close it. */
            for (int i = 2; i < CONTROL_SIZE - 2; ++i) {
                back_blend(bx + i, by + i, ink);
                back_blend(bx + i, by + CONTROL_SIZE - 1 - i, ink);
            }
        } else if (which == 1) {
            draw_round_rect(&c, bx + 2, by + CONTROL_SIZE / 2 - 1,
                            CONTROL_SIZE - 4, 2, 1, ink);
        } else {
            draw_round_rect_outline(&c, bx + 2, by + 2,
                                    CONTROL_SIZE - 4, CONTROL_SIZE - 4,
                                    2, 1, ink);
        }
    }

    char title[WS_TITLE_LEN];
    memcpy(title, (const void*)w->title, WS_TITLE_LEN);
    title[WS_TITLE_LEN - 1] = '\0';
    if (title[0] != '\0') {
        /* Centred on the window, not on the space left over beside the
         * controls - a title that shifts when a control appears reads as the
         * window twitching. Pushed right only if it would collide. */
        const int tw = text_width(13, title);
        int tx = w->x + ((int)fw - tw) / 2;
        int least;
        int ignored;
        control_box(w, 2, &least, &ignored);
        least += CONTROL_SIZE + 10;
        if (tx < least)
            tx = least;
        if (tw > 0 && tx + tw < w->x + (int)fw - 8)
            draw_string(tx, w->y + BORDER + TITLE_HEIGHT / 2 + 5, 13, title,
                        focused ? 0xF0101810u : 0x90101810u);
    }

contents:
    /* No grip bar and no grow box. The band is still there in the geometry,
     * because that is what a pointer grabs to resize, but the panel covers it
     * and nothing is drawn in it. */

    /* The client's own pixels. Bounded by the clip rather than by the window,
     * so a pass repainting one corner copies one corner.
     *
     * Blended through the frame's own rounded shape rather than copied
     * straight down. A client draws a rectangle - it has no idea its window
     * has corners - and copying that over the bottom two would square them off
     * and leave the shadow showing through a notch. */
    if (px == 0)
        return;
    const int content_x = w->x + BORDER;
    const int content_y = w->y + BORDER + TITLE_HEIGHT;
    const int x0 = imax(content_x, g_clip.x), y0 = imax(content_y, g_clip.y);
    const int x1 = imin(content_x + (int)g_width[slot], g_clip.x + g_clip.w);
    const int y1 = imin(content_y + (int)g_height[slot], g_clip.y + g_clip.h);
    if (x1 <= x0)
        return;

    /* Only the last few rows can meet a corner; everything above them is a
     * straight copy and stays as fast as it was. */
    const int curved_from = w->y + (int)fh - CORNER - BORDER;

    for (int y = y0; y < y1; ++y) {
        const uint32_t* row = &px[(unsigned long)(y - content_y) * g_width[slot]];
        if (y < curved_from) {
            memcpy(&g_back[(unsigned)y * g_fb.width + (unsigned)x0],
                   &row[x0 - content_x], (size_t)(x1 - x0) * sizeof(uint32_t));
            continue;
        }
        for (int x = x0; x < x1; ++x) {
            const int cov = draw_round_coverage(x, y, w->x, w->y,
                                                (int)fw, (int)fh, CORNER);
            if (cov == 0)
                continue;
            const uint32_t colour = row[x - content_x];
            back_blend(x, y, ((unsigned)cov << 24) | (colour & 0x00FFFFFFu));
        }
    }
}

/* The wallpaper, when there is one. Held at its own size and sampled rather
 * than scaled properly: a nearest-neighbour stretch is what this can afford,
 * and it is honest about being a stretch. */
static uint32_t* g_paper;
static unsigned  g_paper_w, g_paper_h;
static uint32_t  g_theme_seen = 0xFFFFFFFFu;

static void reload_theme(void)
{
    const uint32_t gen = __atomic_load_n(&g_control->theme.generation,
                                         __ATOMIC_ACQUIRE);
    if (gen == g_theme_seen)
        return;
    g_theme_seen = gen;
    ++g_paper_generation;       /* every backdrop is now stale */
    if (g_paper != 0) {
        free(g_paper);
        g_paper = 0;
        g_paper_w = g_paper_h = 0;
    }
    if (g_control->theme.wallpaper[0] != '\0')
        g_paper = img_read_png(g_control->theme.wallpaper, &g_paper_w, &g_paper_h);
    damage_all();
}

static void compose_cursor(void);
static void compose_drag(void);
static struct rect drag_rect(void);

/* Desktop, then windows back to front so the topmost is drawn last and wins -
 * within one rectangle, and skipping the windows that do not touch it. */
/* Whether some window already covers every pixel of this rectangle with
 * something opaque. If one does, the wallpaper underneath is painted and then
 * immediately painted over, which is a whole damage rectangle of work for
 * pixels nobody was ever going to see.
 *
 * Frosted glass is not opaque, so with the glass on nothing qualifies. And a
 * window's own corners are rounded and antialiased, so the wallpaper does show
 * through there - which is why a normal window only counts for the rectangle
 * inside its corners.
 */
static void compose_rect(const struct rect* r)
{

    g_clip = *r;
    for (int y = r->y; y < r->y + r->h; ++y) {
        uint32_t* row = &g_back[(unsigned)y * g_fb.width + (unsigned)r->x];
        if (g_paper != 0) {
            const unsigned sy = (unsigned)y * g_paper_h / g_fb.height;
            const uint32_t* src = &g_paper[(unsigned long)sy * g_paper_w];
            for (int x = 0; x < r->w; ++x)
                /* & 0xFFFFFF: the decoder reports opacity in the high byte,
                 * which is not part of a colour once it is on the screen. */
                row[x] = src[(unsigned)(r->x + x) * g_paper_w / g_fb.width]
                         & 0xFFFFFF;
        } else {
            /* A pattern is drawn from the desktop colour rather than a second
             * one, so it stays consistent with whatever was chosen. */
            const uint32_t base = DESKTOP;
            const uint32_t lit = tint(base, 18);
            const uint32_t dim = tint(base, -18);
            for (int x = 0; x < r->w; ++x) {
                const int gx = r->x + x;
                uint32_t c = base;
                switch (g_control->theme.pattern) {
                case WS_PATTERN_GRID:
                    if ((gx % 32) == 0 || (y % 32) == 0) c = lit;
                    break;
                case WS_PATTERN_DOTS:
                    if ((gx % 16) == 0 && (y % 16) == 0) c = lit;
                    break;
                case WS_PATTERN_WEAVE:
                    c = (((gx >> 2) + (y >> 2)) & 1) ? lit : dim;
                    break;
                case WS_PATTERN_DITHER:
                    c = ((gx + y) & 1) ? lit : dim;
                    break;
                default:
                    break;
                }
                row[x] = c;
            }
        }
    }
    for (int i = g_count - 1; i >= 0; --i) {
        struct rect f = frame_rect(g_order[i]);
        /* Grown by the shadow, so a window whose frame is outside this region
         * but whose shadow falls inside it still gets its turn - otherwise the
         * shadow appears only when its own window happens to be repainted. */
        f.x -= SHADOW_SPREAD;
        f.y -= SHADOW_SPREAD;
        f.w += SHADOW_SPREAD * 2;
        f.h += SHADOW_SPREAD * 2;
        if (rects_overlap(&f, r))
            draw_window(g_order[i], i == 0);
    }

    /* Above the windows and below the cursor: it is being carried. */
    if (g_control->drag.phase != WS_DRAG_NONE) {
        const struct rect d = drag_rect();
        if (rects_overlap(&d, r))
            compose_drag();
    }

    /* Last, and above every window: it is the one thing always on top. */
    struct rect c;
    c.x = g_cursor_x; c.y = g_cursor_y; c.w = CURSOR_W; c.h = CURSOR_H;
    if (rects_overlap(&c, r))
        compose_cursor();
}

/* --- getting it onto the screen ----------------------------------------- */

static void present_region(int x, int y, unsigned w, unsigned h)
{
    if (x < 0) { w += (unsigned)(-x); x = 0; }
    if (y < 0) { h += (unsigned)(-y); y = 0; }
    if ((unsigned)x >= g_fb.width || (unsigned)y >= g_fb.height)
        return;
    if ((unsigned)x + w > g_fb.width)  w = g_fb.width - (unsigned)x;
    if ((unsigned)y + h > g_fb.height) h = g_fb.height - (unsigned)y;

    for (unsigned row = 0; row < h; ++row) {
        unsigned char* dst = g_screen + (unsigned long)(y + (int)row) * g_fb.pitch
                           + (unsigned long)x * 4;
        const uint32_t* src = &g_back[(unsigned long)(y + (int)row) * g_fb.width + x];
        memcpy(dst, src, (unsigned long)w * 4);
    }
}

/* The cursor is composed like anything else, into the backbuffer, and the whole
 * frame is blitted once.
 *
 * It used to be drawn straight to the screen after the blit, with the
 * backbuffer never containing it. That works only while nothing else is being
 * repainted: every damage pass blits a rectangle that does not have the cursor
 * in it and then draws the cursor back, so for the width of that gap the
 * pointer is not on the screen. With a client presenting steadily - a clock, a
 * task list, the desktop rescanning - the gap comes round often enough to read
 * as a blink.
 *
 * Composing it means the screen only ever shows finished frames. A move damages
 * where it was and where it is, which is two rectangles of 12x19 - cheaper than
 * the recompose this arrangement was originally avoiding. */
static void compose_cursor(void)
{
    for (int row = 0; row < CURSOR_H; ++row) {
        for (int col = 0; col < CURSOR_W; ++col) {
            const unsigned char v = kCursor[row][col];
            if (v == 0)
                continue;
            back_plot(g_cursor_x + col, g_cursor_y + row,
                      (v == 1) ? OUTLINE : CURSOR_FILL);
        }
    }
}

static void damage_cursor(int x, int y)
{
    damage_rect(x, y, CURSOR_W, CURSOR_H);
}

static uint32_t blend_half(uint32_t over, uint32_t under)
{
    return (((over >> 16) & 0xFF) + ((under >> 16) & 0xFF)) / 2 << 16 |
           (((over >> 8) & 0xFF) + ((under >> 8) & 0xFF)) / 2 << 8 |
           ((over & 0xFF) + (under & 0xFF)) / 2;
}

static void ghost_plot(int x, int y, uint32_t colour)
{
    if (x < g_clip.x || y < g_clip.y ||
        x >= g_clip.x + g_clip.w || y >= g_clip.y + g_clip.h)
        return;
    if (x < 0 || y < 0 || (unsigned)x >= g_fb.width || (unsigned)y >= g_fb.height)
        return;
    uint32_t* p = &g_back[(unsigned)y * g_fb.width + (unsigned)x];
    *p = blend_half(colour, *p);
}

static void ghost_fill(int x, int y, int w, int h, uint32_t colour)
{
    for (int row = 0; row < h; ++row)
        for (int col = 0; col < w; ++col)
            ghost_plot(x + col, y + row, colour);
}

static struct rect drag_rect(void)
{
    struct rect r;
    r.x = g_control->drag.x;
    r.y = g_control->drag.y;
    r.w = WS_DRAG_W;
    r.h = WS_DRAG_H;
    return r;
}

static void compose_drag(void)
{
    const int x = g_control->drag.x, y = g_control->drag.y;
    const uint32_t icon = g_control->drag.icon;
    const uint32_t body = (icon == WS_DRAG_FOLDER) ? 0xE8C86A : 0xF0F0F0;
    const uint32_t edge = (icon == WS_DRAG_FOLDER) ? 0x8A6D39 : 0x606060;

    /* The same shapes the file manager and the desktop draw, at the same size,
     * so what follows the cursor is recognisably the thing that was picked up
     * rather than a generic marker. */
    if (icon == WS_DRAG_FOLDER) {
        ghost_fill(x + 12, y + 4, 12, 3, body);
        ghost_fill(x + 12, y + 7, 28, 20, body);
        for (int i = 0; i < 28; ++i) {
            ghost_plot(x + 12 + i, y + 7, edge);
            ghost_plot(x + 12 + i, y + 26, edge);
        }
    } else {
        ghost_fill(x + 16, y + 3, 20, 25, body);
        for (int i = 0; i < 25; ++i) {
            ghost_plot(x + 16, y + 3 + i, edge);
            ghost_plot(x + 35, y + 3 + i, edge);
        }
        for (int i = 0; i < 20; ++i) {
            ghost_plot(x + 16 + i, y + 3, edge);
            ghost_plot(x + 16 + i, y + 27, edge);
        }
        for (int r = 0; r < 4; ++r)
            ghost_fill(x + 20, y + 9 + r * 4, 12, 1, edge);
        if (icon == WS_DRAG_APP)
            ghost_fill(x + 22, y + 14, 8, 8, 0x4060A0);
    }

    /* The label, half-strength like the rest of it. Drawn a character at a
     * time through the same blend so it reads over any background. */
    const char* label = (const char*)g_control->drag.label;
    const int span = WS_DRAG_W / 8;
    for (int i = 0; i < span && label[i] != '\0'; ++i) {
        const unsigned char* glyph = &g_font[(unsigned char)label[i] * 16];
        for (int row = 0; row < 16; ++row)
            for (int col = 0; col < 8; ++col)
                if ((glyph[row] >> (7 - col)) & 1)
                    ghost_plot(x + i * 8 + col, y + 30 + row, 0x000000);
    }
}

/* --- events -------------------------------------------------------------- */

static uint32_t g_mods;         /* what was held at the last input poll */

static void push_event(int slot, uint32_t type, int x, int y,
                       uint32_t button, uint32_t key)
{
    struct ws_window* w = &g_control->windows[slot];
    const uint32_t head = w->head;
    /* A client that has stopped reading must not block the server: drop the
     * event rather than stall the desktop for everyone. */
    if (head - w->tail >= WS_EVENT_SLOTS)
        return;
    struct win_event* e = &w->events[head % WS_EVENT_SLOTS];
    e->type = type;
    e->window = (uint32_t)slot;
    e->x = x;
    e->y = y;
    e->button = button;
    e->key = key;
    e->modifiers = g_mods;
    __atomic_store_n(&w->head, head + 1, __ATOMIC_RELEASE);
}

static int window_at(int x, int y)
{
    for (int i = 0; i < g_count; ++i) {
        struct ws_window* w = &g_control->windows[g_order[i]];
        if (x >= w->x && y >= w->y &&
            x < w->x + (int)frame_width(g_order[i]) &&
            y < w->y + (int)frame_height(g_order[i]))
            return g_order[i];
    }
    return -1;
}

static void raise_window(int slot)
{
    /* The desktop stays underneath, always. Raising it would put it over the
     * windows it is supposed to sit behind. */
    if (is_desktop(slot))
        return;
    int at = 0;
    while (at < g_count && g_order[at] != slot)
        ++at;
    if (at >= g_count || at == 0)
        return;
    const int was_focused = g_order[0];
    for (int i = at; i > 0; --i)
        g_order[i] = g_order[i - 1];
    g_order[0] = slot;
    /* Both change: the raised window comes forward, and the one it displaced
     * loses its active title bar. */
    damage_window(slot);
    damage_window(was_focused);
}

/* --- following what the clients are doing -------------------------------- */

/* Clients appear and vanish on their own; nothing tells the server. So each
 * pass looks at the slot states and reconciles: a slot that has gone live gets
 * its pixels mapped and joins the order, one that has gone free is dropped. */
static void reconcile(void)
{
    for (int slot = 0; slot < WS_MAX_WINDOWS; ++slot) {
        struct ws_window* w = &g_control->windows[slot];
        const uint32_t state = __atomic_load_n(&w->state, __ATOMIC_ACQUIRE);

        int known = 0;
        for (int i = 0; i < g_count; ++i)
            if (g_order[i] == slot) { known = 1; break; }

        /* Nothing tells a userland server that a client died - that is the one
         * thing being inside the kernel gave it for free. So ask: signal 0
         * delivers nothing and simply reports whether the process is still
         * there. A window whose owner has gone is released here, which is what
         * stops a crashed client leaving a corpse on the desktop. */
        if (state != WS_SLOT_FREE && w->owner_pid != 0 &&
            kill((int)w->owner_pid, 0) != 0) {
            __atomic_store_n(&w->state, WS_SLOT_FREE, __ATOMIC_RELEASE);
            continue;                   /* picked up as a removal next pass */
        }

        if (state == WS_SLOT_LIVE && !known) {
            /* Everything in this slot was written by a client, which is not
             * trusted to be correct - a wrong width here would have the
             * compositor read past the end of the segment and take the whole
             * desktop down with it. So the geometry is checked against the
             * screen, and then against the size of the memory actually backing
             * it. */
            const unsigned width = w->width, height = w->height;
            if (width == 0 || height == 0 ||
                width > g_fb.width || height > g_fb.height) {
                __atomic_store_n(&w->state, WS_SLOT_FREE, __ATOMIC_RELEASE);
                continue;
            }

            /* The generation matters even for a window's first appearance.
             * The key carries one so that a resize can replace the segment
             * without the two colliding, and a server that ignores it here
             * looks for a key no client ever created - which is silent, and
             * looks exactly like a window that refuses to appear until
             * something else forces a redraw. */
            const uint32_t gen = __atomic_load_n(&w->pixels_gen, __ATOMIC_ACQUIRE);
            const int id = shm_open(WS_PIXEL_KEY(slot, gen), 0, 0);
            if (id < 0)
                continue;               /* not ready yet; try again next pass */
            const unsigned long bytes = shm_size(id);
            if (bytes < (unsigned long)width * height * 4) {
                __atomic_store_n(&w->state, WS_SLOT_FREE, __ATOMIC_RELEASE);
                continue;
            }
            uint32_t* px = (uint32_t*)shm_map(id);
            if (px == 0)
                continue;

            /* Frozen here, so a client changing them later cannot move the
             * compositor's reads outside what it validated. */
            g_width[slot] = width;
            g_height[slot] = height;
            g_pixels[slot] = px;
            g_pixel_bytes[slot] = bytes;
            g_mapped_gen[slot] = w->present;
            if (is_desktop(slot)) {
                /* Behind everything, and it stays there. */
                g_order[g_count++] = slot;
            } else {
                /* Newest on top, which is also focused. */
                for (int i = g_count; i > 0; --i)
                    g_order[i] = g_order[i - 1];
                g_order[0] = slot;
                ++g_count;
            }
            g_pixel_gen[slot] = gen;
            damage_window(slot);
        } else if (state != WS_SLOT_LIVE && known) {
            damage_window(slot);        /* while its geometry is still known */
            for (int i = 0; i < g_count; ++i) {
                if (g_order[i] != slot)
                    continue;
                for (int j = i; j + 1 < g_count; ++j)
                    g_order[j] = g_order[j + 1];
                --g_count;
                break;
            }
            /* Let the pages go. The client has already dropped the segment's
             * own reference, so this is what actually frees them - and without
             * it the server would accumulate a mapping per window ever
             * opened. */
            if (g_pixels[slot] != 0 && g_pixel_bytes[slot] != 0)
                munmap(g_pixels[slot], g_pixel_bytes[slot]);
            g_pixels[slot] = 0;
            blur_cache_drop(slot);
            g_pixel_bytes[slot] = 0;
            g_pixel_gen[slot] = 0xFFFFFFFFu;
            if (g_dragging == slot)   g_dragging = -1;
            if (g_mouse_grab == slot) g_mouse_grab = -1;
            if (g_resizing == slot)   g_resizing = -1;
        } else if (state == WS_SLOT_LIVE && known) {
            /* A resize replaces the segment rather than growing it, so the
             * client publishes a new generation and the server swaps over. The
             * old pages stay alive until this unmaps them, which is what lets
             * the two overlap instead of needing a handshake. */
            const uint32_t gen = __atomic_load_n(&w->pixels_gen, __ATOMIC_ACQUIRE);
            if (gen != g_pixel_gen[slot]) {
                const unsigned width = w->width, height = w->height;
                const int fresh = (width != 0 && height != 0 &&
                                   width <= g_fb.width && height <= g_fb.height)
                                ? shm_open(WS_PIXEL_KEY(slot, gen), 0, 0) : -1;
                if (fresh >= 0) {
                    const unsigned long bytes = shm_size(fresh);
                    uint32_t* px = (bytes >= (unsigned long)width * height * 4)
                                 ? (uint32_t*)shm_map(fresh) : 0;
                    if (px != 0) {
                        damage_window(slot);    /* where it was */
                        if (g_pixels[slot] != 0 && g_pixel_bytes[slot] != 0)
                            munmap(g_pixels[slot], g_pixel_bytes[slot]);
                        g_pixels[slot] = px;
                        g_pixel_bytes[slot] = bytes;
                        g_width[slot] = width;
                        g_height[slot] = height;
                        g_pixel_gen[slot] = gen;
                        g_mapped_gen[slot] = w->present;
                        damage_window(slot);    /* and where it now is */
                    }
                }
            }

            /* Redraw only when the client says it drew something, and only the
             * content - the frame around it has not changed. */
            const uint32_t present = __atomic_load_n(&w->present, __ATOMIC_ACQUIRE);
            if (present != g_mapped_gen[slot]) {
                g_mapped_gen[slot] = present;
                w->drawn = present;
                damage_rect(w->x + BORDER, w->y + BORDER + TITLE_HEIGHT,
                            (int)g_width[slot], (int)g_height[slot]);
            }
        }
    }
}

static void handle_input(void)
{
    struct input_state in;
    if (input_poll(&in) != 0)
        return;

    g_mods = (uint32_t)in.modifiers;
    const int before_x = g_cursor_x, before_y = g_cursor_y;

    int x = in.mouse_x, y = in.mouse_y;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if ((unsigned)x >= g_fb.width)  x = (int)g_fb.width - 1;
    if ((unsigned)y >= g_fb.height) y = (int)g_fb.height - 1;
    g_cursor_x = x;
    g_cursor_y = y;

    /* Whoever is on top has the keyboard - except for the one chord the window
     * manager keeps for itself, which becomes a close request rather than a
     * keystroke the client has to know about. */
    if (in.key != 0 && g_count > 0) {
        if (in.key == WIN_KEY_CLOSE)
            push_event(g_order[0], WIN_EVENT_CLOSE, 0, 0, 0, 0);
        else
            push_event(g_order[0], WIN_EVENT_KEY, 0, 0, 0, (uint32_t)in.key);
    }

    const int left = (in.buttons & 1) != 0;
    const int pressed = left && !g_last_left;
    const int released = !left && g_last_left;

    /* A live drag follows the pointer wherever it goes, regardless of which
     * window the press started in: the whole point is that it can cross from
     * one process's window into another's. */
    if (g_control->drag.phase == WS_DRAG_LIVE &&
        (x != before_x || y != before_y)) {
        damage_rect(g_control->drag.x, g_control->drag.y, WS_DRAG_W, WS_DRAG_H);
        g_control->drag.x = x - g_control->drag.grab_x;
        g_control->drag.y = y - g_control->drag.grab_y;
        damage_rect(g_control->drag.x, g_control->drag.y, WS_DRAG_W, WS_DRAG_H);
    }

    /* The right button raises a context menu, which is the client's to draw -
     * the server only says where it was asked for. It does not raise or focus
     * the window: a right-click is a question about something, not a decision
     * to work in it. */
    const int right = (in.buttons & 2) != 0;
    if (right && !g_last_right) {
        const int slot = window_at(x, y);
        if (slot >= 0) {
            struct ws_window* w = &g_control->windows[slot];
            const int ox = is_desktop(slot) ? w->x : w->x + BORDER;
            const int oy = is_desktop(slot) ? w->y
                                            : w->y + BORDER + TITLE_HEIGHT;
            push_event(slot, WIN_EVENT_MOUSE_DOWN, x - ox, y - oy, 2, 0);
        }
    }
    g_last_right = right;

    if (pressed) {
        const int slot = window_at(x, y);
        if (slot >= 0) {
            struct ws_window* w = &g_control->windows[slot];
            raise_window(slot);

            int cx, cy;
            close_box(w, &cx, &cy);
            const int on_close = x >= cx && y >= cy &&
                                 x < cx + CLOSE_SIZE && y < cy + CLOSE_SIZE;
            const int on_title = y < w->y + BORDER + TITLE_HEIGHT;

            int gx, gy;
            grow_box(slot, &gx, &gy);
            const int on_grip = x >= gx && y >= gy &&
                                x < gx + GRIP_W && y < gy + GRIP_H;

            if (is_desktop(slot)) {
                /* Everything on it is content, so a press is the client's. */
                push_event(slot, WIN_EVENT_MOUSE_DOWN, x - w->x, y - w->y, 1, 0);
                g_mouse_grab = slot;
            } else if (on_grip) {
                g_resizing = slot;
                g_resize_start_w = (int)g_width[slot];
                g_resize_start_h = (int)g_height[slot];
                g_resize_from_x = x;
                g_resize_from_y = y;
                g_resize_w = g_resize_start_w;
                g_resize_h = g_resize_start_h;
            } else if (on_close) {
                push_event(slot, WIN_EVENT_CLOSE, 0, 0, 1, 0);
            } else if (on_title) {
                g_dragging = slot;
                g_drag_dx = x - w->x;
                g_drag_dy = y - w->y;
            } else {
                push_event(slot, WIN_EVENT_MOUSE_DOWN,
                           x - (w->x + BORDER),
                           y - (w->y + BORDER + TITLE_HEIGHT), 1, 0);
                g_mouse_grab = slot;
            }
        }
    }

    /* Motion goes to whoever holds the pointer, in that window's coordinates,
     * so a stroke that leaves the window stops rather than carrying on into
     * whatever is underneath. */
    if (g_mouse_grab >= 0 && (x != before_x || y != before_y)) {
        struct ws_window* w = &g_control->windows[g_mouse_grab];
        push_event(g_mouse_grab, WIN_EVENT_MOUSE_MOVE,
                   x - (w->x + BORDER),
                   y - (w->y + BORDER + TITLE_HEIGHT), 1, 0);
    }

    if (released && g_resizing >= 0) {
        /* Ask the client for the size the band ended at. It answers by
         * replacing its segment, which reconcile picks up. */
        struct ws_window* w = &g_control->windows[g_resizing];
        if (g_resize_w != (int)g_width[g_resizing] ||
            g_resize_h != (int)g_height[g_resizing]) {
            w->req_width = (uint32_t)g_resize_w;
            w->req_height = (uint32_t)g_resize_h;
            __atomic_add_fetch(&w->resize_seq, 1, __ATOMIC_RELEASE);
        }
        g_resizing = -1;
        g_mouse_grab = -1;
        g_dragging = -1;
        g_last_left = left;
        return;
    }

    if (released) {
        g_dragging = -1;

        /* A drop goes to whatever is *under the cursor*, not to whoever the
         * pointer was grabbed by. Those are the same window for an ordinary
         * click and different ones for every drag worth making. */
        if (g_control->drag.phase == WS_DRAG_LIVE) {
            const int onto = window_at(x, y);
            if (onto >= 0) {
                struct ws_window* w = &g_control->windows[onto];
                const int ox = is_desktop(onto) ? w->x : w->x + BORDER;
                const int oy = is_desktop(onto) ? w->y
                                                : w->y + BORDER + TITLE_HEIGHT;
                push_event(onto, WIN_EVENT_DROP, x - ox, y - oy, 1, 0);
            } else {
                /* Dropped on nothing. Send it home rather than leaving the
                 * ghost stranded waiting for an answer nobody will give. */
                g_control->drag.to_x = g_control->drag.home_x;
                g_control->drag.to_y = g_control->drag.home_y;
                g_control->drag.step = 0;
                g_control->drag.steps = 16;
                g_control->drag.phase = WS_DRAG_SNAP;
            }
        }

        const int slot = g_mouse_grab >= 0 ? g_mouse_grab : window_at(x, y);
        g_mouse_grab = -1;
        if (slot >= 0) {
            struct ws_window* w = &g_control->windows[slot];
            push_event(slot, WIN_EVENT_MOUSE_UP,
                       x - (w->x + BORDER),
                       y - (w->y + BORDER + TITLE_HEIGHT), 1, 0);
        }
    }

    if (g_dragging >= 0) {
        struct ws_window* w = &g_control->windows[g_dragging];
        int nx = x - g_drag_dx, ny = y - g_drag_dy;

        /* Keep the title bar reachable. A window dragged clean off an edge
         * would take its close box with it and could never be shut again. */
        const int keep = BORDER + CLOSE_SIZE + 8;
        const int max_x = (int)g_fb.width - keep;
        const int max_y = (int)g_fb.height - (BORDER + TITLE_HEIGHT);
        if (nx < 0) nx = 0;
        if (ny < 0) ny = 0;
        if (nx > max_x) nx = max_x;
        if (ny > max_y) ny = max_y;

        if (nx != w->x || ny != w->y) {
            damage_window(g_dragging);      /* vacated */
            w->x = nx;
            w->y = ny;
            damage_window(g_dragging);      /* occupied */
        }
    }

    if (g_resizing >= 0) {
        struct ws_window* w = &g_control->windows[g_resizing];
        int nw = g_resize_start_w + (x - g_resize_from_x);
        int nh = g_resize_start_h + (y - g_resize_from_y);
        const int min_w = (int)(w->min_width  != 0 ? w->min_width  : 64u);
        const int min_h = (int)(w->min_height != 0 ? w->min_height : 32u);
        if (nw < min_w) nw = min_w;
        if (nh < min_h) nh = min_h;
        /* Bounded by the screen: a window bigger than the framebuffer is one
         * the server would refuse to map anyway. */
        const int max_w = (int)g_fb.width  - BORDER * 2;
        const int max_h = (int)g_fb.height - (BORDER * 2 + TITLE_HEIGHT + GRIP_H);
        if (nw > max_w) nw = max_w;
        if (nh > max_h) nh = max_h;
        g_resize_w = nw;
        g_resize_h = nh;
    }

    g_last_left = left;
}

/* --- the rubber band ------------------------------------------------------
 *
 * Drawn straight to the screen while a resize is in progress and rubbed out
 * from the backbuffer afterwards, exactly as the cursor is. Committing the size
 * on every pixel of movement would mean the client allocating and the server
 * mapping a new segment per frame, which is a great deal of work to show
 * something that is not final yet. */
static void band_plot(int x, int y)
{
    if (x < 0 || y < 0 || (unsigned)x >= g_fb.width || (unsigned)y >= g_fb.height)
        return;
    uint32_t* p = (uint32_t*)(g_screen + (unsigned long)y * g_fb.pitch
                              + (unsigned long)x * 4);
    *p = OUTLINE;
}

static void draw_band(const struct rect* r)
{
    for (int i = 0; i < r->w; ++i) {
        band_plot(r->x + i, r->y);
        band_plot(r->x + i, r->y + r->h - 1);
    }
    for (int i = 0; i < r->h; ++i) {
        band_plot(r->x, r->y + i);
        band_plot(r->x + r->w - 1, r->y + i);
    }
}

static void erase_band(const struct rect* r)
{
    present_region(r->x, r->y, (unsigned)r->w, 1);
    present_region(r->x, r->y + r->h - 1, (unsigned)r->w, 1);
    present_region(r->x, r->y, 1, (unsigned)r->h);
    present_region(r->x + r->w - 1, r->y, 1, (unsigned)r->h);
}

int main(void)
{
    if (fb_info(&g_fb) != 0 || g_fb.bits_per_pixel != 32) {
        printf("wserver: no 32-bit framebuffer\n");
        return 1;
    }
    g_screen = (unsigned char*)fb_map();
    if (g_screen == 0) {
        printf("wserver: cannot map the framebuffer (root only)\n");
        return 1;
    }
    if (fb_font(g_font) != 0) {
        printf("wserver: cannot read the console font\n");
        return 1;
    }

    /* The proportional font, and the one vector glyph the frame draws.
     *
     * Neither is fatal. A window server that refuses to start because a font
     * is missing is a machine with no screen at all, which is a far worse
     * failure than a title bar with no title - so both are attempted, both are
     * reported, and the frame checks before it uses either. */
    g_typeface = font_open(PATH_FONTS "/sans.ttf");
    if (g_typeface == 0)
        printf("wserver: no proportional font (%s); titles will be blank\n",
               font_error());

    g_close_ready = svg_render(PATH_ICONS "/glyphs/close.svg",
                               CONTROL_SIZE, &g_close_glyph) == 0;
    if (!g_close_ready)
        printf("wserver: no close glyph; drawing one instead\n");

    g_back = (uint32_t*)malloc((unsigned long)g_fb.width * g_fb.height * 4);
    if (g_back == 0) {
        printf("wserver: cannot allocate a backbuffer\n");
        return 1;
    }

    /* Public, because a client is not root and this is how it finds us. Only
     * the rendezvous is shared this widely - a window's pixels stay owned by
     * the client that created them. */
    const int id = shm_open(WS_CONTROL_KEY, sizeof(struct ws_shared), SHM_PUBLIC);
    if (id < 0) {
        printf("wserver: cannot create the control block\n");
        return 1;
    }
    g_control = (struct ws_shared*)shm_map(id);
    if (g_control == 0) {
        printf("wserver: cannot map the control block\n");
        return 1;
    }
    memset(g_control, 0, sizeof(struct ws_shared));
    g_control->server_pid = (uint32_t)getpid();

    /* The palette the desktop starts with; settings may change any of it.
     *
     * Soft and warm rather than the grey-and-navy it was: an off-white face,
     * a title bar the same colour as the window under it so the chrome reads
     * as one surface, and dark text on both. The desktop colour is only seen
     * when there is no wallpaper, so it is a muted green that sits under the
     * photograph rather than fighting it. */
    /* Platinum. The face is the grey everything is made of; light and shadow
     * are the two shades a bevel needs either side of it, and they are far
     * enough apart to read as moulded rather than tinted. The title bar is the
     * same grey as the face - what marks it as a title bar is the pinstripes,
     * not a colour, which is the thing this era got right. */
    g_control->theme.desktop      = 0x8894A8;   /* the blue-grey behind it all */
    g_control->theme.face         = 0xDDDDDD;
    g_control->theme.light        = 0xFFFFFF;
    g_control->theme.shadow       = 0x888888;
    g_control->theme.title_active = 0xDDDDDD;
    g_control->theme.title_idle   = 0xDDDDDD;
    g_control->theme.title_text   = 0x000000;
    g_control->theme.cursor       = 0xFFFFFF;
    g_control->theme.selection    = 0x000080;   /* the classic selection navy */
    g_control->theme.body         = 0xFFFFFF;
    g_control->theme.text         = 0x000000;
    g_control->theme.text_scale   = 1;
    g_control->theme.contrast     = 0;
    g_control->theme.pattern      = WS_PATTERN_DITHER;
    g_control->theme.blur         = 0;

    /* No wallpaper. The desktop is the dither above, which is what this
     * interface looked like - a photograph behind it belongs to a later era
     * and fought with the grey. Settings can still set one; nothing here
     * stops it. */
    g_control->theme.wallpaper[0] = '\0';

    g_control->theme.generation   = 1;
    for (int i = 0; i < WS_MAX_WINDOWS; ++i)
        g_order[i] = -1;

    g_cursor_x = (int)g_fb.width / 2;
    g_cursor_y = (int)g_fb.height / 2;
    g_last_cursor_x = g_last_cursor_y = -1;
    for (int i = 0; i < WS_MAX_WINDOWS; ++i)
        g_pixel_gen[i] = 0xFFFFFFFFu;
    g_clip.x = 0; g_clip.y = 0;
    g_clip.w = (int)g_fb.width; g_clip.h = (int)g_fb.height;
    damage_all();               /* the desktop has to be painted once */

    /* Published last: a client that sees the magic can rely on everything else
     * already being in place. */
    __atomic_store_n(&g_control->magic, WS_MAGIC, __ATOMIC_RELEASE);

    /* The session is over when windows have existed and then all gone. There is
     * deliberately no timeout for "nobody ever connected": the server comes up
     * before its first client and polling each other with two independent
     * timeouts is a race - an impatient server exits while the thing waiting
     * for it is still looking. Whoever started this owns its lifetime and can
     * signal it; see login. */
    int seen_any = 0;
    unsigned long empty_passes = 0;

    for (;;) {
        reload_theme();
        reconcile();
        handle_input();

        if (g_count > 0) {
            seen_any = 1;
            empty_passes = 0;
        } else if (seen_any && ++empty_passes > 200) {
            break;                      /* every window has gone */
        }

        const int cursor_moved = (g_cursor_x != g_last_cursor_x ||
                                  g_cursor_y != g_last_cursor_y);

        if (g_damage_count == 0 && !cursor_moved && g_resizing < 0) {
            /* Nothing changed. Sleep rather than spin: this loop polls input
             * and client state, and polling flat out means a syscall per pass
             * on a CPU that never blocks - which is enough to keep another
             * processor out of the kernel entirely. */
            msleep(kIdleSleepMs);
            continue;
        }

        /* The rubber band still lives on the screen rather than in the
         * backbuffer, so it has to come off before anything composes over it.
         * The cursor no longer does - it is composed like a window, which is
         * what stopped it blinking. */
        if (g_band_shown) {
            erase_band(&g_band);
            g_band_shown = 0;
        }

        /* A pointer move is damage like anything else: where it was, and where
         * it is. Nothing draws to the screen outside the blit below, so the
         * screen only ever shows finished frames. */
        /* The snap home. One frame per step, which is the only clock the
         * server has and the right one: the animation is a property of what
         * is on screen, so it should advance when the screen does. */
        if (g_control->drag.phase == WS_DRAG_SNAP) {
            struct ws_drag* d = &g_control->drag;
            damage_rect(d->x, d->y, WS_DRAG_W, WS_DRAG_H);
            ++d->step;
            if (d->step >= d->steps) {
                d->phase = WS_DRAG_NONE;
                /* Where it finished and where it was aiming, because integer
                 * easing stops short and the two are not always the same
                 * rectangle. Half-transparent pixels that never get recomposed
                 * stay on the screen as a pale rectangle - the one thing a
                 * ghost must not leave behind is a ghost. */
                damage_rect(d->to_x, d->to_y, WS_DRAG_W, WS_DRAG_H);
                damage_rect(d->home_x, d->home_y, WS_DRAG_W, WS_DRAG_H);
            } else {
                /* Ease out: most of the distance early, so it reads as being
                 * pulled into place rather than sliding at a constant speed.
                 * The remaining gap is halved and a bit each frame. */
                /* Ease out: a third of the remaining gap each frame, so it
                 * moves fastest at the start and settles rather than
                 * arriving at speed. The last few pixels are covered by the
                 * step count running out, which is why the target does not
                 * need to be reached exactly. */
                d->x += (d->to_x - d->x) / 3;
                d->y += (d->to_y - d->y) / 3;
                damage_rect(d->x, d->y, WS_DRAG_W, WS_DRAG_H);
            }
        }

        if (g_cursor_x != g_last_cursor_x || g_cursor_y != g_last_cursor_y) {
            if (g_last_cursor_x >= 0)
                damage_cursor(g_last_cursor_x, g_last_cursor_y);
            damage_cursor(g_cursor_x, g_cursor_y);
            g_last_cursor_x = g_cursor_x;
            g_last_cursor_y = g_cursor_y;
        }

        for (int i = 0; i < g_damage_count; ++i) {
            compose_rect(&g_damage[i]);
            present_region(g_damage[i].x, g_damage[i].y,
                           (unsigned)g_damage[i].w, (unsigned)g_damage[i].h);
        }
        g_damage_count = 0;

        if (g_resizing >= 0) {
            struct ws_window* w = &g_control->windows[g_resizing];
            g_band.x = w->x;
            g_band.y = w->y;
            g_band.w = g_resize_w + BORDER * 2;
            g_band.h = g_resize_h + BORDER * 2 + TITLE_HEIGHT + GRIP_H;
            draw_band(&g_band);
            g_band_shown = 1;
        }

        msleep(kFrameSleepMs);
    }

    __atomic_store_n(&g_control->magic, 0, __ATOMIC_RELEASE);
    g_control->quit = 1;
    return 0;
}
