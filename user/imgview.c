/* imgview - shows a PNG.
 *
 * The decoding is libc's, in img_read_png, which reads real deflate and every
 * row filter and colour type at 8 bits. This used to carry its own decoder for
 * the narrow subset paint writes, and there is no reason for a viewer to know
 * anything about deflate that the library does not already know.
 *
 * What is left here is the part that is actually the viewer's: a window, a pan
 * offset, and saying plainly when a file could not be read.
 */

#include <image.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <widget.h>
#include <window.h>

static uint32_t* g_px;
static unsigned  g_w = 480, g_h = 380;

static uint32_t* g_image;
static unsigned g_iw, g_ih;
static char g_note[128] = "";
static char g_path[256];
static int  g_ox, g_oy;         /* pan offset, for images larger than the window */

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
    snprintf(g_note, sizeof(g_note), "%ux%u", g_iw, g_ih);
    return 0;
}

static void draw(void)
{
    wg_fill(0, 0, (int)g_w, (int)g_h, WG_FACE);
    const int top = 24;
    wg_text_clipped(8, 4, g_path, WG_INK, (int)g_w - 16);

    wg_fill(4, top, (int)g_w - 8, (int)g_h - top - 20, 0x404040);
    wg_bevel(4, top, (int)g_w - 8, (int)g_h - top - 20, 0);

    if (g_iw != 0) {
        const int vw = (int)g_w - 12, vh = (int)g_h - top - 28;
        for (int y = 0; y < vh; ++y) {
            const int sy = y + g_oy;
            if (sy < 0 || sy >= (int)g_ih)
                continue;
            for (int x = 0; x < vw; ++x) {
                const int sx = x + g_ox;
                if (sx < 0 || sx >= (int)g_iw)
                    continue;
                wg_plot(8 + x, top + 4 + y,
                        g_image[(unsigned long)sy * g_iw + sx] & 0xFFFFFF);
            }
        }
    }
    wg_fill(0, (int)g_h - 20, (int)g_w, 20, WG_FACE);
    wg_text_clipped(8, (int)g_h - 18, g_note, WG_DIM, (int)g_w - 16);
}

int main(int argc, char** argv)
{
    if (argc > 1) {
        int n = 0;
        while (argv[1][n] != '\0' && n < 255) { g_path[n] = argv[1][n]; ++n; }
        g_path[n] = '\0';
    } else {
        snprintf(g_path, sizeof(g_path), "/PAINT.PNG");
    }
    if (wg_font() != 0)
        return 1;
    const int id = win_create(200, 90, g_w, g_h, "Image");
    if (id < 0) {
        printf("imgview: no window server\n");
        return 1;
    }
    g_px = win_map(id);
    if (g_px == 0)
        return 1;
    win_set_min_size(id, 260, 200);
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
            } else if (e.type == WIN_EVENT_MOUSE_DOWN) {
                dragging = 1; last_x = e.x; last_y = e.y;
            } else if (e.type == WIN_EVENT_MOUSE_UP) {
                dragging = 0;
            } else if (e.type == WIN_EVENT_MOUSE_MOVE && dragging) {
                /* Drag to pan, which is the only navigation an image this
                 * simple needs. */
                g_ox -= e.x - last_x;
                g_oy -= e.y - last_y;
                last_x = e.x; last_y = e.y;
                if (g_ox < 0) g_ox = 0;
                if (g_oy < 0) g_oy = 0;
            } else if (e.type == WIN_EVENT_KEY && e.key == 'r') {
                load(g_path);
                g_ox = g_oy = 0;
            } else {
                continue;
            }
            draw();
            win_present(id);
        }
        msleep(15);
    }
}
