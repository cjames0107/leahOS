/* paint - a window you can scribble in.
 *
 * Small on purpose: it exists to show that a client owns its own pixels, gets
 * its own input events in its own coordinates, and never sees the screen.
 *
 * It is also the simplest thing that has to cope with being resized. The buffer
 * is replaced rather than grown - there is no realloc for shared memory - so
 * the width, the height and the pointer are all re-read after a resize, and
 * nothing is cached across one.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <window.h>

static uint32_t* g_px;
static unsigned  g_w, g_h;

static void fill(uint32_t colour)
{
    for (unsigned i = 0; i < g_w * g_h; ++i)
        g_px[i] = colour;
}

/* A border, so the content area is visibly the client's and not the frame's. */
static void border(void)
{
    for (unsigned i = 0; i < g_w; ++i) {
        g_px[i] = 0xA0A0A0;
        g_px[(g_h - 1) * g_w + i] = 0xA0A0A0;
    }
    for (unsigned i = 0; i < g_h; ++i) {
        g_px[i * g_w] = 0xA0A0A0;
        g_px[i * g_w + g_w - 1] = 0xA0A0A0;
    }
}

static void blot(int cx, int cy, uint32_t colour)
{
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            const int x = cx + dx, y = cy + dy;
            if (x >= 0 && y >= 0 && x < (int)g_w && y < (int)g_h)
                g_px[y * (int)g_w + x] = colour;
        }
    }
}

int main(int argc, char** argv)
{
    const int x = argc > 1 ? atoi_simple(argv[1]) : 120;
    const int y = argc > 2 ? atoi_simple(argv[2]) : 90;

    g_w = 260;
    g_h = 160;
    const int id = win_create(x, y, g_w, g_h, "Paint");
    if (id < 0) {
        printf("paint: no window server\n");
        return 1;
    }
    g_px = win_map(id);
    if (g_px == 0) {
        printf("paint: could not map the window\n");
        return 1;
    }

    fill(0xFFFFFF);
    border();
    win_present(id);

    int drawing = 0;
    uint32_t colour = 0x000080;
    for (;;) {
        struct win_event event;
        while (win_poll(id, &event)) {
            if (event.type == WIN_EVENT_RESIZE) {
                /* The buffer is a new one and starts blank; the old pointer is
                 * gone. Everything that described the old size is re-read. */
                g_w = (unsigned)event.x;
                g_h = (unsigned)event.y;
                g_px = win_map(id);
                if (g_px == 0)
                    return 1;
                fill(0xFFFFFF);
                border();
                win_present(id);
                drawing = 0;
                continue;
            }
            if (event.type == WIN_EVENT_CLOSE) {
                win_destroy(id);
                return 0;
            }
            if (event.type == WIN_EVENT_MOUSE_DOWN) {
                drawing = 1;
                blot(event.x, event.y, colour);
                win_present(id);
            }
            if (event.type == WIN_EVENT_MOUSE_MOVE && drawing) {
                blot(event.x, event.y, colour);
                win_present(id);
            }
            if (event.type == WIN_EVENT_MOUSE_UP)
                drawing = 0;
            if (event.type == WIN_EVENT_KEY) {
                /* c clears, q quits, and the digits pick a colour. */
                if (event.key == 'q') {
                    win_destroy(id);
                    return 0;
                }
                if (event.key == 'c') {
                    fill(0xFFFFFF);
                    border();
                    win_present(id);
                }
                if (event.key >= '1' && event.key <= '4') {
                    static const uint32_t palette[] = {
                        0x000080, 0x800000, 0x008000, 0x000000
                    };
                    colour = palette[event.key - '1'];
                }
            }
        }
        msleep(10);
    }
}
