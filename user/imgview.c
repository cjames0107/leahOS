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

#include <image.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
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

static int view_w(void) { return (int)g_w - 8; }
static int view_h(void) { return (int)g_h - TOOLBAR_H - STATUS_H - 8; }
static int view_x(void) { return 4; }
static int view_y(void) { return TOOLBAR_H + 4; }

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

struct box { int x, y, w, h; };
static struct box g_b_out  = {   6, 3, 26, 20 };
static struct box g_b_in   = {  34, 3, 26, 20 };
static struct box g_b_fit  = {  66, 3, 40, 20 };
static struct box g_b_full = { 110, 3, 48, 20 };

static int inside(const struct box* b, int x, int y)
{
    return x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h;
}

static void draw(void)
{
    wg_glass_clear();

    wg_button(g_b_out.x, g_b_out.y, g_b_out.w, g_b_out.h, "-", 0);
    wg_button(g_b_in.x, g_b_in.y, g_b_in.w, g_b_in.h, "+", 0);
    wg_button(g_b_fit.x, g_b_fit.y, g_b_fit.w, g_b_fit.h, "Fit", g_fitting);
    wg_button(g_b_full.x, g_b_full.y, g_b_full.w, g_b_full.h, "100%",
              !g_fitting && g_zoom == 1.0);
    wg_text_clipped(166, 8, g_path, WG_DIM, (int)g_w - 174);

    const int vx = view_x(), vy = view_y();
    const int vw = view_w(), vh = view_h();
    wg_fill(vx, vy, vw, vh, 0x404040);
    wg_bevel(vx, vy, vw, vh, 0);

    if (g_iw != 0 && g_px != 0 && vw > 2 && vh > 2) {
        /* Where the top-left of the viewport lands in the image. Written
         * straight into the window buffer rather than through wg_plot: this is
         * every pixel of the view on every repaint, and a bounds check per
         * pixel is visible at screen size. */
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
                /* Opaque, explicitly. Stripping the alpha byte used to mean
                 * "there is no alpha here"; since the server began reading
                 * that byte it means "this pixel is not there", so every
                 * picture became a hole the shape of itself. */
                dst[x] = 0xFF000000u | (src[(unsigned)sx] & 0xFFFFFF);
            }
        }
    }

    wg_fill(0, (int)g_h - STATUS_H, (int)g_w, STATUS_H, WG_FACE);
    char line[160];
    if (g_iw != 0) {
        /* The percentage is what a person thinks in; the pixel size is what
         * they need when it matters. */
        snprintf(line, sizeof(line), "%ux%u   %d%%%s   drag to pan, +/- to zoom",
                 g_iw, g_ih, (int)(g_zoom * 100.0 + 0.5),
                 g_fitting ? " (fit)" : "");
    } else {
        snprintf(line, sizeof(line), "%s", g_note);
    }
    wg_text_clipped(8, (int)g_h - STATUS_H + 2, line, WG_DIM, (int)g_w - 16);
}

int main(int argc, char** argv)
{
    if (argc > 1)
        snprintf(g_path, sizeof(g_path), "%s", argv[1]);
    else
        snprintf(g_path, sizeof(g_path), "/PAINT.PNG");

    if (wg_font() != 0)
        return 1;
    const int id = win_create(200, 90, g_w, g_h, "Image");
    /* Its pixels carry alpha, so the glass reaches into it. */
    if (id >= 0)
        win_set_alpha(id);
    if (id < 0) {
        printf("imgview: no window server\n");
        return 1;
    }
    g_px = win_map(id);
    if (g_px == 0)
        return 1;
    win_set_min_size(id, 300, 220);
    wg_target(g_px, g_w, g_h);

    load(g_path);
    draw();
    win_present(id);

    int dragging = 0, last_x = 0, last_y = 0;
    for (;;) {
        struct win_event e;
        while (win_poll(id, &e)) {
            if (e.type == WIN_EVENT_CLOSE) { win_destroy(id); return 0; }

            if (e.type == WIN_EVENT_RESIZE) {
                g_w = (unsigned)e.x; g_h = (unsigned)e.y;
                g_px = win_map(id);
                if (g_px == 0) return 1;
                wg_target(g_px, g_w, g_h);
                /* A resize re-fits only if it was fitted. Once somebody has
                 * chosen a zoom, the window changing shape is not a reason to
                 * overrule them. */
                if (g_fitting)
                    fit();
                else
                    clamp_centre();
            } else if (e.type == WIN_EVENT_MOUSE_DOWN) {
                if (inside(&g_b_out, e.x, e.y))       zoom_step(-1);
                else if (inside(&g_b_in, e.x, e.y))   zoom_step(1);
                else if (inside(&g_b_fit, e.x, e.y))  fit();
                else if (inside(&g_b_full, e.x, e.y)) actual_size();
                else { dragging = 1; last_x = e.x; last_y = e.y; }
            } else if (e.type == WIN_EVENT_MOUSE_UP) {
                dragging = 0;
            } else if (e.type == WIN_EVENT_MOUSE_MOVE && dragging) {
                /* A drag of one screen pixel moves the picture one screen
                 * pixel, whatever the zoom - so the image follows the cursor
                 * rather than racing ahead of it when magnified. */
                g_cx -= (double)(e.x - last_x) / g_zoom;
                g_cy -= (double)(e.y - last_y) / g_zoom;
                last_x = e.x; last_y = e.y;
                clamp_centre();
            } else if (e.type == WIN_EVENT_KEY) {
                switch (e.key) {
                case '+': case '=': zoom_step(1); break;
                case '-': case '_': zoom_step(-1); break;
                case 'f': case 'F': fit(); break;
                case '1':           actual_size(); break;
                case 'r': case 'R': load(g_path); break;
                default: continue;
                }
            } else {
                continue;
            }
            draw();
            win_present(id);
        }
        msleep(15);
    }
}
