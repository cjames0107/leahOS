/* wintest - how many windows the server will actually give out.
 *
 * The window table used to be one fixed array, and the failure when it ran out
 * was the worst kind: win_create returned -1, the application exited, and from
 * where the person was sitting nothing at all happened. This asks the question
 * directly.
 *
 * It opens them from one process rather than from a shell loop because a shell
 * loop cannot be typed: the first window to open takes the keyboard, and every
 * command after it goes into that window instead of the terminal.
 */

#include <display.h>
#include <cli.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <window.h>
#include <wproto.h>

#define TILE 90

/* --- how fast the compositor actually is ------------------------------------
 *
 * Measured from the client's side of the shared block, which is the only place
 * the whole path can be seen at once: `present` is bumped when this process has
 * drawn, and the server copies it into `drawn` once that frame is on the
 * screen. Counting how often `drawn` moves counts finished frames - not frames
 * asked for, and not the time one function took.
 *
 * A full-screen window that carries alpha is deliberately the worst case: the
 * whole desktop is damaged every frame, so the wallpaper fill, the blend and
 * the blit are all in the measurement.
 */
static int bench(int ms)
{
    struct fb_info fb;
    if (fb_info(&fb) != 0) {
        cli_fail("no framebuffer");
        return 1;
    }
    const int id = win_create(0, 0, fb.width - 40, fb.height - 120, "bench");
    if (id < 0) {
        cli_fail("could not open a window");
        return 1;
    }
    win_set_alpha(id);
    uint32_t* px = win_map(id);
    struct ws_window* w = ws_slot(id);
    if (px == 0 || w == 0)
        return 1;

    unsigned width = 0, height = 0;
    win_size(id, &width, &height);
    /* The rows are further apart than they are wide, so the fill walks rows
     * rather than running straight through the buffer. The same number of
     * pixels either way, which is what is being measured; running straight
     * through put them in the wrong places, and the window came out part
     * filled and part whatever it was allocated with. */
    const unsigned stride = win_stride(id);

    const unsigned long start = uptime_ms();
    unsigned long frames = 0;
    uint32_t last = __atomic_load_n(&w->drawn, __ATOMIC_ACQUIRE);
    unsigned tick = 0;
    while (uptime_ms() - start < (unsigned long)ms) {
        /* Opaque, which is what an application's content actually is: the
         * toolkit clears to a solid colour and everything drawn on it is
         * solid. A window filled with translucent pixels measures the blend
         * path and nothing else, and no real window looks like that. */
        const uint32_t ink = 0xFF000000u | (0x101010u * (++tick & 0xF));
        for (unsigned y = 0; y < height; ++y) {
            uint32_t* row = &px[(unsigned long)y * stride];
            for (unsigned x = 0; x < width; ++x)
                row[x] = ink;
        }
        win_present(id);

        struct win_event e;
        while (win_poll(id, &e))
            ;                   /* the queue must not be left to fill */
        const uint32_t now = __atomic_load_n(&w->drawn, __ATOMIC_ACQUIRE);
        if (now != last) {
            last = now;
            ++frames;
        }
    }
    const unsigned long took = uptime_ms() - start;
    win_destroy(id);
    printf("wintest: %lu frames in %lu ms at %ux%u (%lu per second)\n",
           frames, took, width, height,
           took > 0 ? frames * 1000 / took : 0);
    return 0;
}

/* --- what the screen itself costs -------------------------------------------
 *
 * The compositor's last act is a copy of the finished frame into the
 * framebuffer, and how much that costs depends entirely on how the
 * framebuffer is mapped. Uncacheable memory takes every four-byte store
 * straight to the bus; ordinary memory does not. Measuring both tells us
 * which of the two the compositor is actually paying for, rather than leaving
 * it to be guessed from a frame rate.
 */
static int blitbench(int rounds)
{
    struct fb_info fb;
    if (fb_info(&fb) != 0) {
        cli_fail("no framebuffer");
        return 1;
    }
    unsigned char* screen = (unsigned char*)fb_map();
    if (screen == 0) {
        cli_fail("cannot map the framebuffer");
        return 1;
    }
    const unsigned long bytes = (unsigned long)fb.width * fb.height * 4;
    uint32_t* ram = (uint32_t*)malloc(bytes);
    uint32_t* ram2 = (uint32_t*)malloc(bytes);
    if (ram == 0 || ram2 == 0) {
        cli_fail("out of memory");
        return 1;
    }
    for (unsigned long i = 0; i < bytes / 4; ++i)
        ram[i] = 0x101820u;

    /* Ordinary memory first, as the baseline. */
    unsigned long t0 = uptime_ms();
    for (int i = 0; i < rounds; ++i)
        memcpy(ram2, ram, bytes);
    const unsigned long to_ram = uptime_ms() - t0;

    /* Then the same bytes to the screen, one row at a time, which is what the
     * compositor does. */
    t0 = uptime_ms();
    for (int i = 0; i < rounds; ++i)
        for (unsigned y = 0; y < fb.height; ++y)
            memcpy(screen + (unsigned long)y * fb.pitch,
                   &ram[(unsigned long)y * fb.width],
                   (unsigned long)fb.width * 4);
    const unsigned long to_screen = uptime_ms() - t0;

    printf("wintest: %lu KiB x %d - to memory %lu ms, to screen %lu ms\n",
           bytes / 1024, rounds, to_ram, to_screen);
    free(ram);
    free(ram2);
    return 0;
}

/* What the keyboard actually delivers: every key and the modifiers that came
 * with it, on the serial line where a check can read them. */
static int keys(int ms)
{
    const int id = win_create(60, 60, 320, 120, "keys");
    if (id < 0)
        return 1;
    uint32_t* px = win_map(id);
    if (px != 0) {
        const unsigned stride = win_stride(id);
        for (int y = 0; y < 120; ++y)
            for (int x = 0; x < 320; ++x)
                px[(unsigned long)y * stride + (unsigned)x] = 0xFF202830u;
        win_present(id);
    }
    FILE* out = fopen("/dev/console", "w");
    const unsigned long start = uptime_ms();
    while (uptime_ms() - start < (unsigned long)ms) {
        struct win_event e;
        while (win_poll(id, &e)) {
            if (e.type == WIN_EVENT_KEY && out != 0)
                fprintf(out, "key %u mods %u\n", e.key, e.modifiers);
            if (e.type == WIN_EVENT_SCROLL && out != 0)
                fprintf(out, "wheel %d at %d,%d\n", (int)e.button, e.x, e.y);
        }
        msleep(10);
    }
    if (out != 0) { fprintf(out, "keys done\n"); fclose(out); }
    win_destroy(id);
    return 0;
}

int main(int argc, char** argv)
{
    cli_begin(argc, argv, "[keys|blit|bench] [n]   (or: [windows] [ms])", "");
    const char* what = cli_arg(0);

    if (what != 0 && strcmp(what, "keys") == 0)
        return keys(cli_argc() > 1 ? atoi_simple(cli_arg(1)) : 6000);
    if (what != 0 && strcmp(what, "blit") == 0)
        return blitbench(cli_argc() > 1 ? atoi_simple(cli_arg(1)) : 20);
    if (what != 0 && strcmp(what, "bench") == 0)
        return bench(cli_argc() > 1 ? atoi_simple(cli_arg(1)) : 4000);

    const int want = what != 0 ? atoi_simple(what) : 40;
    const int hold = cli_argc() > 1 ? atoi_simple(cli_arg(1)) : 3000;

    int* ids = (int*)malloc(sizeof(int) * (unsigned)want);
    if (ids == 0) {
        cli_fail("out of memory");
        return 1;
    }

    int got = 0;
    for (int i = 0; i < want; ++i) {
        /* Spread across the screen so that they are visibly separate windows
         * and not one window drawn `want` times. */
        const int x = 8 + (i % 10) * 100;
        const int y = 8 + (i / 10) * 96;
        char title[32];
        snprintf(title, sizeof(title), "w%d", i);
        const int id = win_create(x, y, TILE, 60, title);
        if (id < 0)
            break;
        uint32_t* px = win_map(id);
        if (px == 0)
            break;
        /* A different shade each, so a screenshot shows how many there are. */
        const uint32_t ink = 0xFF000000u | (uint32_t)(0x203040 + i * 0x050301);
        const unsigned stride = win_stride(id);
        for (int y = 0; y < 60; ++y)
            for (int x = 0; x < TILE; ++x)
                px[(unsigned long)y * stride + (unsigned)x] = ink;
        win_present(id);
        ids[got++] = id;
    }

    printf("wintest: %d of %d windows\n", got, want);
    fflush(stdout);

    /* Held open long enough to be looked at, then given back - a check that
     * leaves forty windows behind is a check that can only be run once. */
    msleep((unsigned)hold);
    for (int i = 0; i < got; ++i)
        win_destroy(ids[i]);
    free(ids);
    return got == want ? 0 : 1;
}
