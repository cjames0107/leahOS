/* paint - a drawing program.
 *
 * Tools, a palette, brush sizes, and saving to PNG or GIF. The canvas is the
 * window's own pixel buffer below the toolbar, so what is saved is exactly what
 * is on screen - there is no second copy to keep in step.
 *
 * Saving is real: see user/libc/image.c. Neither format is compressed - PNG
 * uses stored deflate blocks and GIF clears its LZW table before it fills -
 * because this system has no compressor. Both are valid files that any reader
 * will open.
 */

#include <dialog.h>
#include <image.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <widget.h>
#include <window.h>

#define TOOLBAR_H 92
#define STATUS_H  18

#define T_PENCIL 0
#define T_LINE   1
#define T_RECT   2
#define T_FILL   3
#define T_ERASE  4
#define TOOLS    5

static uint32_t* g_px;
static unsigned  g_w = 460, g_h = 360;

static int g_tool = T_PENCIL;
static uint32_t g_colour = 0x000080;
static int g_size = 2;
static char g_note[96] = "click a tool, drag on the canvas";

static int g_drawing, g_ax, g_ay, g_lx, g_ly;

static const char* kToolName[TOOLS] = { "Pencil", "Line", "Rect", "Fill", "Eraser" };

/* The ink is mixed rather than chosen from a tray. Twelve fixed colours are
 * twelve pictures you can paint; three sliders are all of them. */
#define PICK_X 6
#define PICK_Y 28
#define PICK_W 288
static int g_pick_drag = -1;

struct box { int x, y, w, h; };
static struct box g_tool_box[TOOLS];
static struct box g_size_box[3];
static struct box g_png = { 306, 30, 64, 20 };
static struct box g_gif = { 306, 54, 64, 20 };
static struct box g_clr = { 376, 30, 64, 20 };

static int inside(const struct box* b, int x, int y)
{
    return x >= b->x && y >= b->y && x < b->x + b->w && y < b->y + b->h;
}

static void layout(void)
{
    for (int i = 0; i < TOOLS; ++i) {
        g_tool_box[i].x = 6 + i * 58; g_tool_box[i].y = 5;
        g_tool_box[i].w = 56; g_tool_box[i].h = 20;
    }
    for (int i = 0; i < 3; ++i) {
        g_size_box[i].x = 306 + i * 26; g_size_box[i].y = 5;
        g_size_box[i].w = 24; g_size_box[i].h = 20;
    }
}

/* The canvas is the window buffer below the toolbar - so saving is just a crop
 * of what the compositor already shows. */
static int canvas_top(void)    { return TOOLBAR_H; }
static int canvas_h(void)      { return (int)g_h - TOOLBAR_H - STATUS_H; }

static void dot(int x, int y, uint32_t colour)
{
    const int r = g_size;
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx) {
            const int px = x + dx, py = y + dy;
            if (py >= canvas_top() && py < canvas_top() + canvas_h() &&
                px >= 0 && px < (int)g_w)
                wg_plot(px, py, colour);
        }
}

/* Bresenham, so a fast drag leaves a line rather than a dotted trail. */
static void line(int x0, int y0, int x1, int y1, uint32_t colour)
{
    int dx = x1 - x0, dy = y1 - y0;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        dot(x0, y0, colour);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}

static void rect_outline(int x0, int y0, int x1, int y1, uint32_t colour)
{
    line(x0, y0, x1, y0, colour);
    line(x1, y0, x1, y1, colour);
    line(x1, y1, x0, y1, colour);
    line(x0, y1, x0, y0, colour);
}

/* Flood fill, iteratively over an explicit stack: the recursive version needs a
 * frame per pixel and overruns the stack on any real area. */
#define FILL_MAX 65536
static int g_stack[FILL_MAX];
static void flood(int x, int y, uint32_t to)
{
    const int top = canvas_top(), bottom = top + canvas_h();
    if (x < 0 || y < top || x >= (int)g_w || y >= bottom)
        return;
    const uint32_t from = g_px[(unsigned)y * g_w + (unsigned)x];
    if (from == to)
        return;
    int sp = 0;
    g_stack[sp++] = y * (int)g_w + x;
    while (sp > 0) {
        const int at = g_stack[--sp];
        const int cx = at % (int)g_w, cy = at / (int)g_w;
        if (cx < 0 || cy < top || cx >= (int)g_w || cy >= bottom)
            continue;
        if (g_px[(unsigned)cy * g_w + (unsigned)cx] != from)
            continue;
        g_px[(unsigned)cy * g_w + (unsigned)cx] = to;
        if (sp + 4 >= FILL_MAX)
            continue;               /* give up neatly rather than overrun */
        g_stack[sp++] = at - 1;
        g_stack[sp++] = at + 1;
        g_stack[sp++] = at - (int)g_w;
        g_stack[sp++] = at + (int)g_w;
    }
}

static void clear_canvas(void)
{
    wg_fill(0, canvas_top(), (int)g_w, canvas_h(), WG_PAPER);
}

static int g_pending_png = 1;   /* which format the open dialogue is for */

/* Only the canvas, not the chrome: the toolbar is not part of the picture. */
static void save_to(const char* path, int png)
{
    const unsigned h = (unsigned)canvas_h();
    const uint32_t* start = &g_px[(unsigned long)canvas_top() * g_w];
    const int rc = png ? img_write_png(path, start, g_w, h)
                       : img_write_gif(path, start, g_w, h);
    if (rc == 0)
        snprintf(g_note, sizeof(g_note), "saved %s (%ux%u)", path, g_w, h);
    else
        snprintf(g_note, sizeof(g_note), "could not write %s", path);
}

/* Ask where, rather than choosing for the user. */
static void save(int png)
{
    g_pending_png = png;
    dlg_save("/", png ? "picture.png" : "picture.gif");
    snprintf(g_note, sizeof(g_note), "choose where to save");
}

static void draw_chrome(void)
{
    wg_fill(0, 0, (int)g_w, TOOLBAR_H, WG_FACE);
    for (int i = 0; i < TOOLS; ++i)
        wg_button(g_tool_box[i].x, g_tool_box[i].y, g_tool_box[i].w,
                  g_tool_box[i].h, kToolName[i], g_tool == i);
    wg_rgb_draw(PICK_X, PICK_Y, PICK_W, g_colour);
    for (int i = 0; i < 3; ++i) {
        char s[4] = { (char)('1' + i), 0 };
        wg_button(g_size_box[i].x, g_size_box[i].y, g_size_box[i].w,
                  g_size_box[i].h, s, g_size == i + 1);
    }
    wg_button(g_clr.x, g_clr.y, g_clr.w, g_clr.h, "Clear", 0);
    wg_button(g_png.x, g_png.y, g_png.w, g_png.h, "PNG", 0);
    wg_button(g_gif.x, g_gif.y, g_gif.w, g_gif.h, "GIF", 0);

    wg_fill(0, (int)g_h - STATUS_H, (int)g_w, STATUS_H, WG_FACE);
    wg_text_clipped(6, (int)g_h - STATUS_H + 1, g_note, WG_INK, (int)g_w - 12);
}

int main(int argc, char** argv)
{
    const int wx = argc > 1 ? atoi_simple(argv[1]) : 120;
    const int wy = argc > 2 ? atoi_simple(argv[2]) : 90;
    if (wg_font() != 0)
        return 1;
    const int id = win_create(wx, wy, g_w, g_h, "Paint");
    if (id < 0) {
        printf("paint: no window server\n");
        return 1;
    }
    g_px = win_map(id);
    if (g_px == 0)
        return 1;
    win_set_min_size(id, 450, 260);
    wg_target(g_px, g_w, g_h);

    layout();
    clear_canvas();
    draw_chrome();
    win_present(id);

    for (;;) {
        struct win_event e;
        while (win_poll(id, &e)) {
            if (e.type == WIN_EVENT_CLOSE) { win_destroy(id); return 0; }

            /* While a dialogue is up it takes the input; the drawing tools must
             * not also act on a click meant for it. */
            if (dlg_active() && e.type != WIN_EVENT_RESIZE) {
                if (dlg_event(&e) == DLG_ACCEPT)
                    save_to(dlg_path(), g_pending_png);
                draw_chrome();
                dlg_draw((int)g_w, (int)g_h);
                win_present(id);
                continue;
            }

            if (e.type == WIN_EVENT_RESIZE) {
                g_w = (unsigned)e.x; g_h = (unsigned)e.y;
                g_px = win_map(id);
                if (g_px == 0) return 1;
                wg_target(g_px, g_w, g_h);
                layout();
                clear_canvas();     /* the buffer is new and starts blank */
            } else if (e.type == WIN_EVENT_MOUSE_DOWN) {
                int handled = 0;
                for (int i = 0; i < TOOLS; ++i)
                    if (inside(&g_tool_box[i], e.x, e.y)) { g_tool = i; handled = 1; }
                g_pick_drag = wg_rgb_hit(PICK_X, PICK_Y, PICK_W, e.x, e.y);
                if (g_pick_drag >= 0) {
                    g_colour = wg_rgb_move(g_colour, g_pick_drag, PICK_X,
                                           PICK_W, e.x);
                    handled = 1;
                }
                for (int i = 0; i < 3; ++i)
                    if (inside(&g_size_box[i], e.x, e.y)) { g_size = i + 1; handled = 1; }
                if (inside(&g_clr, e.x, e.y)) { clear_canvas(); handled = 1; }
                else if (inside(&g_png, e.x, e.y)) { save(1); handled = 1; }
                else if (inside(&g_gif, e.x, e.y)) { save(0); handled = 1; }

                if (!handled && e.y >= canvas_top()) {
                    const uint32_t ink = (g_tool == T_ERASE) ? WG_PAPER : g_colour;
                    g_ax = g_lx = e.x; g_ay = g_ly = e.y;
                    if (g_tool == T_FILL) {
                        flood(e.x, e.y, ink);
                    } else if (g_tool == T_PENCIL || g_tool == T_ERASE) {
                        dot(e.x, e.y, ink);
                        g_drawing = 1;
                    } else {
                        g_drawing = 1;      /* line and rect commit on release */
                    }
                }
            } else if (e.type == WIN_EVENT_MOUSE_MOVE && g_pick_drag >= 0) {
                g_colour = wg_rgb_move(g_colour, g_pick_drag, PICK_X, PICK_W,
                                       e.x);
            } else if (e.type == WIN_EVENT_MOUSE_MOVE && g_drawing) {
                if (g_tool == T_PENCIL || g_tool == T_ERASE) {
                    const uint32_t ink = (g_tool == T_ERASE) ? WG_PAPER : g_colour;
                    line(g_lx, g_ly, e.x, e.y, ink);
                    g_lx = e.x; g_ly = e.y;
                }
            } else if (e.type == WIN_EVENT_MOUSE_UP) {
                g_pick_drag = -1;
                if (g_drawing && g_tool == T_LINE)
                    line(g_ax, g_ay, e.x, e.y, g_colour);
                else if (g_drawing && g_tool == T_RECT)
                    rect_outline(g_ax, g_ay, e.x, e.y, g_colour);
                g_drawing = 0;
            } else if (e.type == WIN_EVENT_KEY) {
                if (e.key == 'c') clear_canvas();
                else if (e.key == 'p') save(1);
                else if (e.key == 'g') save(0);
                else if (e.key >= '1' && e.key <= '3') g_size = (int)e.key - '0';
            } else {
                continue;
            }
            draw_chrome();
            dlg_draw((int)g_w, (int)g_h);
            win_present(id);
        }
        msleep(15);
    }
}
