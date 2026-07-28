/* paint - a window you can scribble in.
 *
 * Small on purpose: it exists to show that a client owns its own pixels, gets
 * its own input events in its own coordinates, and never sees the screen.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <window.h>

#define W 260
#define H 160

static void fill(uint32_t* px, unsigned w, unsigned h, uint32_t colour)
{
    for (unsigned i = 0; i < w * h; ++i)
        px[i] = colour;
}

static void blot(uint32_t* px, int cx, int cy, uint32_t colour)
{
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            const int x = cx + dx, y = cy + dy;
            if (x >= 0 && y >= 0 && x < W && y < H)
                px[y * W + x] = colour;
        }
    }
}

/* A border, so the content area is visibly the client's and not the frame's. */
static void border(uint32_t* px)
{
    for (int i = 0; i < W; ++i) { px[i] = 0xA0A0A0; px[(H - 1) * W + i] = 0xA0A0A0; }
    for (int i = 0; i < H; ++i) { px[i * W] = 0xA0A0A0; px[i * W + W - 1] = 0xA0A0A0; }
}

int main(int argc, char** argv)
{
    const int x = argc > 1 ? atoi_simple(argv[1]) : 120;
    const int y = argc > 2 ? atoi_simple(argv[2]) : 90;

    const int id = win_create(x, y, W, H, "Paint");
    if (id < 0) {
        printf("paint: no window server\n");
        return 1;
    }
    uint32_t* px = win_map(id);
    if (px == 0) {
        printf("paint: could not map the window\n");
        return 1;
    }

    fill(px, W, H, 0xFFFFFF);
    border(px);
    win_present(id);

    int drawing = 0;
    uint32_t colour = 0x000080;
    for (;;) {
        struct win_event event;
        while (win_poll(id, &event)) {
            if (event.type == WIN_EVENT_CLOSE) {
                win_destroy(id);
                return 0;
            }
            if (event.type == WIN_EVENT_MOUSE_DOWN) {
                drawing = 1;
                blot(px, event.x, event.y, colour);
                win_present(id);
            }
            if (event.type == WIN_EVENT_MOUSE_MOVE && drawing) {
                blot(px, event.x, event.y, colour);
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
                    fill(px, W, H, 0xFFFFFF);
                    border(px);
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
        yield();
    }
}
