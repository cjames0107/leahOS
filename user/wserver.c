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
#define DESKTOP      0x008080
#define FACE         0xC0C0C0
#define LIGHT        0xFFFFFF
#define SHADOW       0x606060
#define OUTLINE      0x000000
#define TITLE_ACTIVE 0x000080
#define TITLE_IDLE   0x808080
#define TITLE_TEXT   0xFFFFFF

#define TITLE_HEIGHT 18
#define BORDER       3
#define CLOSE_SIZE   12

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
static unsigned long g_pixel_bytes[WS_MAX_WINDOWS];
/* The server's own copy of each window's size, taken once and checked. The
 * client's fields stay writable by the client; these do not. */
static unsigned  g_width[WS_MAX_WINDOWS];
static unsigned  g_height[WS_MAX_WINDOWS];
static int       g_order[WS_MAX_WINDOWS];    /* front to back; [0] is focused */
static int       g_count;

static int g_damaged = 1;
static int g_cursor_x, g_cursor_y;
static int g_last_cursor_x = -1, g_last_cursor_y = -1;
static int g_dragging = -1, g_drag_dx, g_drag_dy;
static int g_mouse_grab = -1;
static int g_last_left;

/* --- drawing into the backbuffer ---------------------------------------- */

static void back_plot(int x, int y, uint32_t colour)
{
    if (x < 0 || y < 0 || (unsigned)x >= g_fb.width || (unsigned)y >= g_fb.height)
        return;
    g_back[(unsigned)y * g_fb.width + (unsigned)x] = colour;
}

static void fill(int x, int y, unsigned w, unsigned h, uint32_t colour)
{
    for (unsigned row = 0; row < h; ++row)
        for (unsigned col = 0; col < w; ++col)
            back_plot(x + (int)col, y + (int)row, colour);
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
static unsigned frame_width(int slot)  { return g_width[slot] + BORDER * 2; }
static unsigned frame_height(int slot)
{
    return g_height[slot] + BORDER * 2 + TITLE_HEIGHT;
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

    /* The client's own pixels. */
    const uint32_t* px = g_pixels[slot];
    if (px == 0)
        return;
    const int content_x = w->x + BORDER, content_y = ty + TITLE_HEIGHT;
    for (unsigned row = 0; row < g_height[slot]; ++row)
        for (unsigned col = 0; col < g_width[slot]; ++col)
            back_plot(content_x + (int)col, content_y + (int)row,
                      px[(unsigned long)row * g_width[slot] + col]);
}

/* Desktop, then windows back to front so the topmost is drawn last and wins. */
static void compose(void)
{
    for (unsigned long i = 0; i < (unsigned long)g_fb.width * g_fb.height; ++i)
        g_back[i] = DESKTOP;
    for (int i = g_count - 1; i >= 0; --i)
        draw_window(g_order[i], i == 0);
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

/* The cursor goes straight to the screen and is rubbed out from the backbuffer,
 * so moving the pointer never costs a recompose. */
static void draw_cursor(void)
{
    for (int row = 0; row < CURSOR_H; ++row) {
        for (int col = 0; col < CURSOR_W; ++col) {
            const unsigned char v = kCursor[row][col];
            if (v == 0)
                continue;
            const int x = g_cursor_x + col, y = g_cursor_y + row;
            if (x < 0 || y < 0 || (unsigned)x >= g_fb.width ||
                (unsigned)y >= g_fb.height)
                continue;
            uint32_t* p = (uint32_t*)(g_screen + (unsigned long)y * g_fb.pitch
                                      + (unsigned long)x * 4);
            *p = (v == 1) ? OUTLINE : LIGHT;
        }
    }
}

static void erase_cursor(void)
{
    if (g_last_cursor_x < 0)
        return;
    present_region(g_last_cursor_x, g_last_cursor_y, CURSOR_W, CURSOR_H);
}

/* --- events -------------------------------------------------------------- */

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
    int at = 0;
    while (at < g_count && g_order[at] != slot)
        ++at;
    if (at >= g_count || at == 0)
        return;
    for (int i = at; i > 0; --i)
        g_order[i] = g_order[i - 1];
    g_order[0] = slot;
    g_damaged = 1;
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

            const int id = shm_open(WS_PIXEL_KEY_BASE + (unsigned)slot, 0, 0);
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
            /* Newest on top, which is also focused. */
            for (int i = g_count; i > 0; --i)
                g_order[i] = g_order[i - 1];
            g_order[0] = slot;
            ++g_count;
            g_damaged = 1;
        } else if (state != WS_SLOT_LIVE && known) {
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
            if (g_dragging == slot)   g_dragging = -1;
            if (g_mouse_grab == slot) g_mouse_grab = -1;
            g_damaged = 1;
        } else if (state == WS_SLOT_LIVE && known) {
            /* Redraw only when the client says it drew something. */
            const uint32_t present = __atomic_load_n(&w->present, __ATOMIC_ACQUIRE);
            if (present != g_mapped_gen[slot]) {
                g_mapped_gen[slot] = present;
                w->drawn = present;
                g_damaged = 1;
            }
        }
    }
}

static void handle_input(void)
{
    struct input_state in;
    if (input_poll(&in) != 0)
        return;

    const int before_x = g_cursor_x, before_y = g_cursor_y;

    int x = in.mouse_x, y = in.mouse_y;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if ((unsigned)x >= g_fb.width)  x = (int)g_fb.width - 1;
    if ((unsigned)y >= g_fb.height) y = (int)g_fb.height - 1;
    g_cursor_x = x;
    g_cursor_y = y;

    /* Whoever is on top has the keyboard. */
    if (in.key != 0 && g_count > 0)
        push_event(g_order[0], WIN_EVENT_KEY, 0, 0, 0, (uint32_t)in.key);

    const int left = (in.buttons & 1) != 0;
    const int pressed = left && !g_last_left;
    const int released = !left && g_last_left;

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

            if (on_close) {
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
            w->x = nx;
            w->y = ny;
            g_damaged = 1;
        }
    }

    g_last_left = left;
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
    for (int i = 0; i < WS_MAX_WINDOWS; ++i)
        g_order[i] = -1;

    g_cursor_x = (int)g_fb.width / 2;
    g_cursor_y = (int)g_fb.height / 2;

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
        reconcile();
        handle_input();

        if (g_count > 0) {
            seen_any = 1;
            empty_passes = 0;
        } else if (seen_any && ++empty_passes > 200) {
            break;                      /* every window has gone */
        }

        if (g_damaged) {
            compose();
            present_region(0, 0, g_fb.width, g_fb.height);
            g_last_cursor_x = -1;       /* the whole screen was repainted */
            g_damaged = 0;
        } else if (g_cursor_x != g_last_cursor_x || g_cursor_y != g_last_cursor_y) {
            erase_cursor();
        } else {
            /* Nothing changed. Sleep rather than spin: this loop polls input
             * and client state, and polling flat out means a syscall per pass
             * on a CPU that never blocks - which is enough to keep another
             * processor out of the kernel entirely. */
            msleep(kIdleSleepMs);
            continue;
        }

        draw_cursor();
        g_last_cursor_x = g_cursor_x;
        g_last_cursor_y = g_cursor_y;
        msleep(kFrameSleepMs);
    }

    __atomic_store_n(&g_control->magic, 0, __ATOMIC_RELEASE);
    g_control->quit = 1;
    return 0;
}
