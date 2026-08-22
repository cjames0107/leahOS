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

#include <app.h>
#include <ui.h>
#include <image.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <widget.h>
#include <window.h>

#define TOOLBAR_H 40
#define STATUS_H  22

#define T_PENCIL 0
#define T_LINE   1
#define T_RECT   2
#define T_FILL   3
#define T_ERASE  4
#define TOOLS    5

static uint32_t* g_px;
static unsigned  g_w = 640, g_h = 420;

static int g_tool = T_PENCIL;
static uint32_t g_colour = 0x000080;
static int g_size = 2;
static char g_note[96] = "click a tool, drag on the canvas";

static int g_drawing, g_ax, g_ay, g_lx, g_ly;

static const char* kToolName[TOOLS] = { "Pencil", "Line", "Rect", "Fill", "Eraser" };

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
    if (rc == 0) {
        snprintf(g_note, sizeof(g_note), "saved %s (%ux%u)", path, g_w, h);
    } else {
        /* Said out loud as well as in the toolbar. A picture that was not
         * written is the one thing here that loses work, and a note in a
         * corner of a window the person has already stopped looking at is not
         * telling them. */
        snprintf(g_note, sizeof(g_note), "could not write %s", path);
        FILE* c = fopen("/dev/console", "w");
        if (c != 0) {
            fprintf(c, "paint: could not write %s\n", path);
            fclose(c);
        }
    }
}

static void say(const char* text);

/* Ask where, rather than choosing for the user. */
static struct app g_app;
/* Ask where, rather than choosing for the user. */
static struct app g_app;

/* Where the picture goes, once the sheet has been answered. */
static void saved(struct app* a, int result)
{
    if (result)
        save_to(app_sheet_path(a), g_pending_png);
    /* The canvas is untouched either way: the sheet was its own window and
     * never drew a pixel into this one. That is the whole reason it is a
     * window - what a dialogue covers here is the picture, and there is no
     * copy of the picture to put back. */
    say(g_note);
}

static void save(int png)
{
    g_pending_png = png;
    g_app.sheet_done = saved;
    app_sheet_save(&g_app, "/", png ? "picture.png" : "picture.gif");
    say("choose where to save");
}

static void on_png(struct ui_view* v, void* user);
static void on_gif(struct ui_view* v, void* user);

/* The picture, as a document the framework knows about.
 *
 * Paint had no dirty flag at all: closing it threw the picture away without a
 * word, and tracking a flag is only worth doing when something acts on it.
 * Something does now. */
static int save_document(struct app* a, const char* path)
{
    (void)a;
    const unsigned h = (unsigned)canvas_h();
    const uint32_t* start = &g_px[(unsigned long)canvas_top() * g_w];
    /* By what it is called, so "picture.gif" is a GIF. The two buttons still
     * choose directly; this is for Save, which has only a path to go on. */
    const int n = (int)strlen(path);
    const int gif = n > 4 && (path[n - 3] == 'g' || path[n - 3] == 'G') &&
                             (path[n - 2] == 'i' || path[n - 2] == 'I');
    return gif ? img_write_gif(path, start, g_w, h)
               : img_write_png(path, start, g_w, h);
}

/* --- the toolbar, as components -------------------------------------------
 *
 * Five tools as one segmented control, a colour well that opens a mixer, the
 * brush size as a stepper, and three buttons. It was five buttons, three
 * buttons, three more buttons and an RGB picker, each at a rectangle written
 * down twice - once to draw it and once to decide whether it had been pressed
 * - in ninety-two pixels of chrome. The tree places them now, and the mixer
 * comes out of the well instead of sitting open all the time.
 */

static struct ui_view* g_note_label;

static const char* tool_name(void* user, int i)
{
    (void)user;
    return (i >= 0 && i < TOOLS) ? kToolName[i] : "";
}

static void on_tool(struct ui_view* v, void* user)
{
    (void)user;
    g_tool = v->on;
}

static void on_ink(struct ui_view* v, void* user)
{
    (void)user;
    g_colour = (uint32_t)v->value;
}

static void on_size(struct ui_view* v, void* user)
{
    (void)user;
    if (v->value < 1) v->value = 1;
    g_size = v->value;
}

static void say(const char* text)
{
    snprintf(g_note, sizeof(g_note), "%s", text);
    if (g_note_label != 0)
        ui_set_text(g_note_label, g_note);
}

static void on_clear(struct ui_view* v, void* user)
{
    (void)v; (void)user;
    clear_canvas();
    app_doc_touched(&g_app);
    say("cleared");
}

static int on_event(struct app* a, const struct win_event* e);
static void on_draw(struct app* a);

int main(int argc, char** argv)
{
    struct ui_view* root = ui_box(0, UI_STACK_V, 0, 0);

    struct ui_view* bar = ui_box(root, UI_STACK_H, 8, 8);
    ui_grow(ui_size(bar, 0, TOOLBAR_H), 0);

    struct ui_view* tools = ui_segmented(bar, tool_name, TOOLS, 0);
    tools->on = g_tool;
    ui_grow(ui_size(tools, 250, 24), 0);
    ui_on(tools, on_tool, 0);

    ui_on(ui_grow(ui_size(ui_colour(bar, g_colour), 54, 24), 0), on_ink, 0);

    struct ui_view* size = ui_stepper(bar, g_size, 8);
    ui_grow(ui_size(size, 72, 24), 0);
    ui_on(size, on_size, 0);

    ui_spacer(bar);
    ui_grow(ui_size(ui_button(bar, "Clear", on_clear, 0), 64, 24), 0);
    ui_grow(ui_size(ui_button(bar, "PNG", on_png, 0), 56, 24), 0);
    ui_grow(ui_size(ui_button(bar, "GIF", on_gif, 0), 56, 24), 0);
    ui_overflow(bar);

    /* The canvas: nothing at all, so that nothing draws over the picture. */
    ui_grow(ui_spacer(root), 1);

    struct ui_view* foot = ui_box(root, UI_STACK_H, 6, 0);
    ui_grow(ui_size(foot, 0, STATUS_H), 0);
    g_note_label = ui_grow(ui_label(foot, g_note), 1);

    g_app.root = root;
    g_app.title = "Paint";
    g_app.width = g_w; g_app.height = g_h;
    /* The toolbar folds, so the window is only as wide as the canvas needs. */
    g_app.min_width = 360; g_app.min_height = 260;
    /* What it is editing, so the framework can ask before it is lost and put
     * the name in the title bar. */
    g_app.doc_kind = "picture";
    g_app.doc_dir = "/root";
    g_app.doc_suggested = "picture.png";
    g_app.doc_save = save_document;
    g_app.draw = on_draw;
    g_app.event = on_event;
    /* Alpha, so the glass reaches into it like every other window. The canvas
     * is opaque where it has been drawn on, which is what clear_canvas does. */
    return app_run(&g_app, argc, argv);
}

/* Only the chrome. The canvas is the buffer itself - there is no model behind
 * it - so anything that repainted the whole window would erase the picture.
 * That is also why the framework is given a `draw` at all: without one it
 * would clear the window on every pass. */
/* Paint reaches into the window's pixels directly - the canvas *is* the buffer
 * - so it has to take the framework's, and take it again after every resize.
 * The buffer is a different buffer then, and holding the old pointer means
 * drawing into a mapping nobody is showing. */
static void adopt(struct app* a)
{
    g_px = a->px;
    g_w = a->w;
    g_h = a->h;
}

static void on_draw(struct app* a)
{
    static int ready;
    adopt(a);
    if (!ready) {
        clear_canvas();
        ready = 1;
    }
    /* The bands the components sit in. Nothing paints the canvas between them,
     * which is what keeps the picture: it is the buffer itself. */
    wg_fill(0, 0, (int)g_w, TOOLBAR_H, WG_FACE);
    wg_fill(0, (int)g_h - STATUS_H, (int)g_w, STATUS_H, WG_FACE);
}

static int on_event(struct app* a, const struct win_event* e)
{
    adopt(a);
    if (e->type == WIN_EVENT_RESIZE) {
        clear_canvas();         /* the buffer is new and starts blank */
        return 1;
    }
    if (e->type == WIN_EVENT_MOUSE_DOWN) {
        if (e->y >= canvas_top() && e->y < canvas_top() + canvas_h()) {
            const uint32_t ink = (g_tool == T_ERASE) ? WG_PAPER : g_colour;
            g_ax = g_lx = e->x; g_ay = g_ly = e->y;
            app_doc_touched(a);
            if (g_tool == T_FILL) {
                flood(e->x, e->y, ink);
            } else if (g_tool == T_PENCIL || g_tool == T_ERASE) {
                dot(e->x, e->y, ink);
                g_drawing = 1;
            } else {
                g_drawing = 1;      /* line and rect commit on release */
            }
        }
        return 1;
    }
    if (e->type == WIN_EVENT_MOUSE_MOVE && g_drawing) {
        if (g_tool == T_PENCIL || g_tool == T_ERASE) {
            const uint32_t ink = (g_tool == T_ERASE) ? WG_PAPER : g_colour;
            line(g_lx, g_ly, e->x, e->y, ink);
            g_lx = e->x; g_ly = e->y;
        }
        return 1;
    }
    if (e->type == WIN_EVENT_MOUSE_UP) {
        if (g_drawing && g_tool == T_LINE)
            line(g_ax, g_ay, e->x, e->y, g_colour);
        else if (g_drawing && g_tool == T_RECT)
            rect_outline(g_ax, g_ay, e->x, e->y, g_colour);
        g_drawing = 0;
        return 1;
    }
    if (e->type == WIN_EVENT_KEY) {
        if (e->key == 'c') clear_canvas();
        else if (e->key == 'p') save(1);
        else if (e->key == 'g') save(0);
        else if (e->key >= '1' && e->key <= '3') g_size = (int)e->key - '0';
        else return 0;
        return 1;
    }
    return 0;
}

static void on_png(struct ui_view* v, void* user)
{
    (void)v; (void)user;
    save(1);
}

static void on_gif(struct ui_view* v, void* user)
{
    (void)v; (void)user;
    save(0);
}
