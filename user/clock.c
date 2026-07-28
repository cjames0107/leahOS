/* clock - a window that redraws itself, to show the server keeps up. */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <window.h>

#define W 200
#define H 60

int main(void)
{
    const int id = win_create(430, 300, W, H, "Uptime");
    if (id < 0) {
        printf("clock: no window server\n");
        return 1;
    }
    uint32_t* px = win_map(id);
    if (px == 0)
        return 1;

    for (unsigned i = 0; i < W * H; ++i)
        px[i] = 0xC0C0C0;
    win_present(id);

    unsigned tick = 0;
    for (;;) {
        struct win_event event;
        while (win_poll(id, &event)) {
            if (event.type == WIN_EVENT_CLOSE ||
                (event.type == WIN_EVENT_KEY && event.key == 'q')) {
                win_destroy(id);
                return 0;
            }
        }

        /* A bar that grows and wraps: motion is the point, not precision. */
        const unsigned width = (tick++ % (W - 20)) + 1;
        for (unsigned yy = 20; yy < 40; ++yy) {
            for (unsigned xx = 10; xx < W - 10; ++xx)
                px[yy * W + xx] = xx - 10 < width ? 0x000080 : 0xFFFFFF;
        }
        win_present(id);
        for (volatile int spin = 0; spin < 2000000; ++spin) { }
    }
}
