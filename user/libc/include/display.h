#ifndef _DISPLAY_H
#define _DISPLAY_H

#include <stdint.h>

/* The screen and the raw input devices.
 *
 * Both are root-only: whoever holds the framebuffer can draw over anything
 * anyone is looking at, and whoever polls input sees every keystroke. They
 * exist so a window server can run as an ordinary process instead of living in
 * the kernel. */

struct fb_info {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;             /* bytes per scanline, not always width * 4 */
    uint32_t bits_per_pixel;
};

int fb_info(struct fb_info* out);

/* Map the linear framebuffer. Returns its address, or 0. */
void* fb_map(void);

struct input_state {
    int32_t mouse_x;
    int32_t mouse_y;
    int32_t buttons;            /* 1 left, 2 right, 4 middle */
    int32_t key;                /* one character, or 0 when nothing is waiting */
    int32_t modifiers;          /* MOD_SHIFT, MOD_CTRL - held now, for clicks */
};

#define MOD_SHIFT 1
#define MOD_CTRL  2

int input_poll(struct input_state* out);

/* The 8x16 console font: 256 glyphs of 16 rows, one byte per row with bit 7
 * leftmost. `out` must have room for 4096 bytes. */
int fb_font(unsigned char* out);

#endif /* _DISPLAY_H */
