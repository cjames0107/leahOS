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
#include <image.h>
#include <shm.h>
#include <sys/mman.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

#define TITLE_HEIGHT 18
#define BORDER       3
#define CLOSE_SIZE   12
/* The grip bar along the bottom, and the grow box sitting at its right end.
 * A strip of its own rather than a corner overlapping the content, so a click
 * near the bottom-right of a window is unambiguously a resize and never a
 * stroke the client was expecting. */
#define GRIP_H       14
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

    for (int i = 0; i < g_damage_count; ++i) {
        if (!rects_overlap(&r, &g_damage[i]))
            continue;
        const int nx = imin(r.x, g_damage[i].x), ny = imin(r.y, g_damage[i].y);
        const int mx = imax(r.x + r.w, g_damage[i].x + g_damage[i].w);
        const int my = imax(r.y + r.h, g_damage[i].y + g_damage[i].h);
        g_damage[i].x = nx; g_damage[i].y = ny;
        g_damage[i].w = mx - nx; g_damage[i].h = my - ny;
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

static void back_plot(int x, int y, uint32_t colour)
{
    if (x < g_clip.x || y < g_clip.y ||
        x >= g_clip.x + g_clip.w || y >= g_clip.y + g_clip.h)
        return;
    g_back[(unsigned)y * g_fb.width + (unsigned)x] = colour;
}

/* Clipped up front rather than per pixel: a window's face is most of its area,
 * and a pass that only has to repaint a title bar should not walk the rest. */
static void fill(int x, int y, unsigned w, unsigned h, uint32_t colour)
{
    const int x0 = imax(x, g_clip.x), y0 = imax(y, g_clip.y);
    const int x1 = imin(x + (int)w, g_clip.x + g_clip.w);
    const int y1 = imin(y + (int)h, g_clip.y + g_clip.h);
    for (int row = y0; row < y1; ++row)
        for (int col = x0; col < x1; ++col)
            g_back[(unsigned)row * g_fb.width + (unsigned)col] = colour;
}

/* Raised (or, inverted, sunken): light on the top and left edges, shadow on the
 * bottom and right. */
static void bevel(int x, int y, unsigned w, unsigned h, int raised)
{
    const uint32_t tl = raised ? LIGHT : SHADOW;
    const uint32_t br = raised ? SHADOW : LIGHT;
    for (unsigned i = 0; i < w; ++i) {
        back_plot(x + (int)i, y, tl);
        back_plot(x + (int)i, y + (int)h - 1, br);
    }
    for (unsigned i = 0; i < h; ++i) {
        back_plot(x, y + (int)i, tl);
        back_plot(x + (int)w - 1, y + (int)i, br);
    }
}

static void draw_text(int x, int y, const char* text, uint32_t colour)
{
    for (unsigned i = 0; text[i] != '\0'; ++i) {
        const unsigned char* glyph = &g_font[(unsigned char)text[i] * 16];
        for (unsigned row = 0; row < 16; ++row) {
            for (unsigned col = 0; col < 8; ++col) {
                if ((glyph[row] & (0x80 >> col)) != 0)
                    back_plot(x + (int)(i * 8 + col), y + (int)row, colour);
            }
        }
    }
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

static void damage_window(int slot)
{
    const struct rect r = frame_rect(slot);
    damage_rect(r.x, r.y, r.w, r.h);
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
        for (int y = y0; y < y1; ++y) {
            const uint32_t* row = &dp[(unsigned long)(y - w->y) * g_width[slot]];
            uint32_t* dst = &g_back[(unsigned)y * g_fb.width + (unsigned)x0];
            for (int x = x0; x < x1; ++x)
                *dst++ = row[x - w->x];
        }
        return;
    }

    fill(w->x, w->y, fw, fh, FACE);
    bevel(w->x, w->y, fw, fh, 1);
    bevel(w->x + 1, w->y + 1, fw - 2, fh - 2, 1);   /* thickness */

    const int tx = w->x + BORDER, ty = w->y + BORDER;
    fill(tx, ty, fw - BORDER * 2, TITLE_HEIGHT,
         focused ? TITLE_ACTIVE : TITLE_IDLE);

    int cx, cy;
    close_box(w, &cx, &cy);
    fill(cx, cy, CLOSE_SIZE, CLOSE_SIZE, FACE);
    bevel(cx, cy, CLOSE_SIZE, CLOSE_SIZE, 1);
    for (unsigned i = 3; i < CLOSE_SIZE - 3; ++i)
        back_plot(cx + (int)i, cy + CLOSE_SIZE / 2, OUTLINE);

    char title[WS_TITLE_LEN];
    memcpy(title, (const void*)w->title, WS_TITLE_LEN);
    title[WS_TITLE_LEN - 1] = '\0';
    draw_text(cx + CLOSE_SIZE + 6, ty + (TITLE_HEIGHT - 16) / 2 + 1,
              title, TITLE_TEXT);

    /* The grip bar, and the grow box at its right end: three diagonal lines,
     * which is the whole of the idiom. */
    const int gy = w->y + (int)frame_height(slot) - BORDER - GRIP_H;
    fill(tx, gy, fw - BORDER * 2, GRIP_H, FACE);
    int bx, by;
    grow_box(slot, &bx, &by);
    bevel(bx, by, GRIP_W, GRIP_H, 1);
    for (int line = 0; line < 3; ++line) {
        const int off = 3 + line * 4;
        for (int i = 0; i < GRIP_W - 4 - off; ++i)
            back_plot(bx + GRIP_W - 2 - i, by + GRIP_H - 2 - (GRIP_W - 4 - off) + i,
                      SHADOW);
    }

    /* The client's own pixels. Bounded by the clip rather than the window, so
     * a pass repainting one corner copies one corner. */
    const uint32_t* px = g_pixels[slot];
    if (px == 0)
        return;
    const int content_x = w->x + BORDER, content_y = ty + TITLE_HEIGHT;
    const int x0 = imax(content_x, g_clip.x), y0 = imax(content_y, g_clip.y);
    const int x1 = imin(content_x + (int)g_width[slot], g_clip.x + g_clip.w);
    const int y1 = imin(content_y + (int)g_height[slot], g_clip.y + g_clip.h);
    for (int y = y0; y < y1; ++y) {
        const uint32_t* row = &px[(unsigned long)(y - content_y) * g_width[slot]];
        uint32_t* dst = &g_back[(unsigned)y * g_fb.width + (unsigned)x0];
        for (int x = x0; x < x1; ++x)
            *dst++ = row[x - content_x];
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

/* Desktop, then windows back to front so the topmost is drawn last and wins -
 * within one rectangle, and skipping the windows that do not touch it. */
static void compose_rect(const struct rect* r)
{
    g_clip = *r;
    for (int y = r->y; y < r->y + r->h; ++y) {
        uint32_t* row = &g_back[(unsigned)y * g_fb.width + (unsigned)r->x];
        if (g_paper != 0) {
            const unsigned sy = (unsigned)y * g_paper_h / g_fb.height;
            const uint32_t* src = &g_paper[(unsigned long)sy * g_paper_w];
            for (int x = 0; x < r->w; ++x)
                row[x] = src[(unsigned)(r->x + x) * g_paper_w / g_fb.width];
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
                default:
                    break;
                }
                row[x] = c;
            }
        }
    }
    for (int i = g_count - 1; i >= 0; --i) {
        const struct rect f = frame_rect(g_order[i]);
        if (rects_overlap(&f, r))
            draw_window(g_order[i], i == 0);
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

    /* The palette the desktop starts with; settings may change any of it. */
    g_control->theme.desktop      = 0x008080;
    g_control->theme.face         = 0xC0C0C0;
    g_control->theme.light        = 0xFFFFFF;
    g_control->theme.shadow       = 0x606060;
    g_control->theme.title_active = 0x000080;
    g_control->theme.title_idle   = 0x808080;
    g_control->theme.title_text   = 0xFFFFFF;
    g_control->theme.cursor       = 0xFFFFFF;
    g_control->theme.selection    = 0xB0C4DE;
    g_control->theme.body         = 0xFFFFFF;
    g_control->theme.text         = 0x000000;
    g_control->theme.text_scale   = 1;
    g_control->theme.contrast     = 0;
    g_control->theme.pattern      = WS_PATTERN_FLAT;
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
