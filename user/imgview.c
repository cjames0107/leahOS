/* imgview - shows a PNG, at whatever size you want to see it.
 *
 * The decoding is libc's, in img_read_png, which reads real deflate and every
 * row filter and colour type at 8 bits. What is left here is the part that is
 * actually the viewer's: a window, a scale, somewhere to look, and saying
 * plainly when a file could not be read.
 *
 * A picture opens fitted to the window, because the first thing anyone wants
 * of a photograph is to see all of it. Fitting never magnifies: a 32-pixel
 * icon in a 500-pixel window is shown at its own size rather than blown up to
 * a wall of blocks, and it does already fit inside the window, which is what
 * was asked for.
 *
 * Sampling is nearest-neighbour, the same as the wallpaper and the icons. It
 * is honest about being a stretch, and for a magnified pixel drawing it is
 * what you want to see anyway - the pixels.
 */

#include <app.h>
#include <image.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ui.h>
#include <widget.h>
#include <window.h>

static uint32_t* g_px;
static unsigned  g_w = 520, g_h = 400;

static uint32_t* g_image;
static unsigned g_iw, g_ih;
static char g_note[128] = "";
static char g_path[256];

/* Where in the image the middle of the viewport is looking, in image pixels.
 * Held as a point rather than as a corner offset so that zooming keeps what
 * you were looking at in the middle, which is what makes zoom feel attached to
 * the picture rather than to the window. */
static double g_cx, g_cy;
static struct ui_rect g_view;   /* the viewport, handed over by the layout */
static double g_zoom = 1.0;
static int    g_fitting = 1;    /* follow the window until the user says not to */

#define TOOLBAR_H 26
#define STATUS_H  20

/* The steps zoom moves in. Not a constant factor: the useful sizes are the
 * simple fractions and the small whole multiples, and a plain doubling skips
 * every one of them between 1/2 and 2. */
static const double kSteps[] = {
    0.03125, 0.0625, 0.125, 0.25, 1.0/3.0, 0.5, 2.0/3.0, 1.0,
    1.5, 2.0, 3.0, 4.0, 6.0, 8.0, 16.0, 32.0
};
#define STEP_COUNT ((int)(sizeof(kSteps) / sizeof(kSteps[0])))

static int view_w(void) { return g_view.w; }
static int view_h(void) { return g_view.h; }

/* The scale at which the whole picture is visible. Capped at 1: fitting is
 * about bringing something too large down, not about magnifying. */
static double fit_zoom(void)
{
    if (g_iw == 0 || g_ih == 0 || view_w() <= 0 || view_h() <= 0)
        return 1.0;
    const double sx = (double)view_w() / (double)g_iw;
    const double sy = (double)view_h() / (double)g_ih;
    double z = (sx < sy) ? sx : sy;
    if (z > 1.0)
        z = 1.0;
    if (z < kSteps[0])
        z = kSteps[0];
    return z;
}

/* Keep the centre inside the picture, so it can never be scrolled away
 * entirely. When the picture is smaller than the viewport on an axis it is
 * pinned to the middle, which is where a small image belongs. */
static void clamp_centre(void)
{
    if (g_iw == 0)
        return;
    const double half_w = (double)view_w() / (2.0 * g_zoom);
    const double half_h = (double)view_h() / (2.0 * g_zoom);

    if ((double)g_iw <= half_w * 2.0) {
        g_cx = (double)g_iw / 2.0;
    } else {
        if (g_cx < half_w) g_cx = half_w;
        if (g_cx > (double)g_iw - half_w) g_cx = (double)g_iw - half_w;
    }
    if ((double)g_ih <= half_h * 2.0) {
        g_cy = (double)g_ih / 2.0;
    } else {
        if (g_cy < half_h) g_cy = half_h;
        if (g_cy > (double)g_ih - half_h) g_cy = (double)g_ih - half_h;
    }
}

static void set_zoom(double z, int from_fit)
{
    if (z < kSteps[0]) z = kSteps[0];
    if (z > kSteps[STEP_COUNT - 1]) z = kSteps[STEP_COUNT - 1];
    g_zoom = z;
    g_fitting = from_fit;
    clamp_centre();
}

static void zoom_step(int direction)
{
    /* To the next step past where we are, rather than to the next entry in a
     * table position we might not be sitting on - a fitted zoom is almost
     * never exactly one of these. */
    if (direction > 0) {
        for (int i = 0; i < STEP_COUNT; ++i)
            if (kSteps[i] > g_zoom * 1.0001) { set_zoom(kSteps[i], 0); return; }
        set_zoom(kSteps[STEP_COUNT - 1], 0);
    } else {
        for (int i = STEP_COUNT - 1; i >= 0; --i)
            if (kSteps[i] < g_zoom * 0.9999) { set_zoom(kSteps[i], 0); return; }
        set_zoom(kSteps[0], 0);
    }
}

static void fit(void)         { set_zoom(fit_zoom(), 1); }
static void actual_size(void) { set_zoom(1.0, 0); }

static int load(const char* path)
{
    g_iw = g_ih = 0;
    g_image = img_read_png(path, &g_iw, &g_ih);
    if (g_image == 0) {
        /* One message for every reason, because from out here they are the
         * same reason: this file did not become an image. Interlaced PNGs and
         * bit depths other than 8 are the cases libc turns down. */
        snprintf(g_note, sizeof(g_note), "cannot read %s as a PNG", path);
        return -1;
    }
    g_cx = (double)g_iw / 2.0;
    g_cy = (double)g_ih / 2.0;
    fit();
    return 0;
}

/* --- the toolbar ---------------------------------------------------------- */



/* --- the interface ---------------------------------------------------------
 *
 * A toolbar of components and the picture as a custom view. The picture stays
 * hand-drawn on purpose: it is every pixel of the viewport on every repaint,
 * sampled at whatever zoom is set, which is not a thing any component does.
 */

static struct app g_app;
static struct ui_view* g_status;
static struct ui_view* g_where;
static char g_status_text[160];

static void sync_status(void)
{
    if (g_iw != 0)
        snprintf(g_status_text, sizeof(g_status_text),
                 "%ux%u   %d%%%s   drag to pan, +/- to zoom", g_iw, g_ih,
                 (int)(g_zoom * 100.0 + 0.5), g_fitting ? " (fit)" : "");
    else
        snprintf(g_status_text, sizeof(g_status_text), "%s", g_note);
    ui_set_text(g_status, g_status_text);
}

static void draw_picture(struct ui_view* v, void* user)
{
    (void)user;
    g_view = v->frame;
    const int vx = g_view.x, vy = g_view.y, vw = g_view.w, vh = g_view.h;
    wg_fill(vx, vy, vw, vh, 0x404040);
    if (g_iw == 0 || g_px == 0 || vw <= 2 || vh <= 2)
        return;

    /* Where the top-left of the viewport lands in the image. Written straight
     * into the window buffer rather than through wg_plot: this is every pixel
     * of the view on every repaint, and a bounds check per pixel is visible at
     * screen size. */
    const double left = g_cx - (double)vw / (2.0 * g_zoom);
    const double top  = g_cy - (double)vh / (2.0 * g_zoom);
    const double step = 1.0 / g_zoom;

    for (int y = 1; y < vh - 1; ++y) {
        const double sy = top + (double)y * step;
        if (sy < 0.0 || sy >= (double)g_ih)
            continue;
        const uint32_t* src = &g_image[(unsigned long)sy * g_iw];
        uint32_t* dst = &g_px[(unsigned long)(vy + y) * g_w + vx];
        for (int x = 1; x < vw - 1; ++x) {
            const double sx = left + (double)x * step;
            if (sx < 0.0 || sx >= (double)g_iw)
                continue;
            /* Opaque, explicitly: stripping the alpha byte means "not there"
             * to the server, and every picture became a hole the shape of
             * itself when this said otherwise. */
            dst[x] = 0xFF000000u | (src[(unsigned)sx] & 0xFFFFFF);
        }
    }
}

static void on_out(struct ui_view* v, void* u)  { (void)v; (void)u; zoom_step(-1); sync_status(); }
static void on_in(struct ui_view* v, void* u)   { (void)v; (void)u; zoom_step(1);  sync_status(); }
static void on_fit(struct ui_view* v, void* u)  { (void)v; (void)u; fit();         sync_status(); }
static void on_full(struct ui_view* v, void* u) { (void)v; (void)u; actual_size(); sync_status(); }

static int g_dragging, g_drag_x, g_drag_y;

static int on_event(struct app* a, const struct win_event* e)
{
    (void)a;
    if (e->type == WIN_EVENT_MOUSE_DOWN &&
        e->x >= g_view.x && e->y >= g_view.y &&
        e->x < g_view.x + g_view.w && e->y < g_view.y + g_view.h) {
        g_dragging = 1;
        g_drag_x = e->x;
        g_drag_y = e->y;
        return 0;
    }
    if (e->type == WIN_EVENT_MOUSE_UP) {
        g_dragging = 0;
        return 0;
    }
    if (e->type == WIN_EVENT_MOUSE_MOVE && g_dragging) {
        /* Panning moves the picture under the pointer, so the image follows
         * the hand rather than running away from it. */
        g_cx -= (double)(e->x - g_drag_x) / g_zoom;
        g_cy -= (double)(e->y - g_drag_y) / g_zoom;
        g_drag_x = e->x;
        g_drag_y = e->y;
        clamp_centre();
        return 1;
    }
    if (e->type == WIN_EVENT_KEY) {
        if (e->key == '+' || e->key == '=') zoom_step(1);
        else if (e->key == '-')             zoom_step(-1);
        else if (e->key == 'f')             fit();
        else if (e->key == '1')             actual_size();
        else return 0;
        sync_status();
        return 1;
    }
    return 0;
}

int main(int argc, char** argv)
{
    struct ui_view* root = ui_box(0, UI_STACK_V, 8, 6);

    struct ui_view* bar = ui_box(root, UI_STACK_H, 0, 6);
    ui_size(bar, 0, 24);
    ui_grow(bar, 0);
    ui_grow(ui_button(bar, "-", on_out, 0), 0);
    ui_grow(ui_button(bar, "+", on_in, 0), 0);
    ui_grow(ui_button(bar, "Fit", on_fit, 0), 0);
    ui_grow(ui_button(bar, "100%", on_full, 0), 0);
    g_where = ui_label(bar, "");
    ui_grow(g_where, 1);

    ui_custom(root, draw_picture, 0);
    g_status = ui_label(root, "");
    ui_grow(g_status, 0);

    if (argc > 1 && argv[1][0] != '\0' && argv[1][0] != '-')
        load(argv[1]);
    ui_set_text(g_where, g_path);
    sync_status();

    g_app.title = "Image";
    g_app.width = g_w; g_app.height = g_h;
    g_app.min_width = 320; g_app.min_height = 240;
    g_app.event = on_event;
    g_app.root = root;
    return app_run(&g_app, argc, argv);
}
