#ifndef _WIDGET_H
#define _WIDGET_H

#include <stdint.h>

/* The small amount of drawing every window client turns out to need.
 *
 * Not a toolkit: a client owns a rectangle of pixels and draws it itself, and
 * these are just the four operations it takes to do that in the desktop's
 * idiom. They live here because paint, uitest, the browser and the editor were
 * otherwise going to carry four copies of the same bevel routine. */

#define WG_FACE    0xC0C0C0u
#define WG_LIGHT   0xFFFFFFu
#define WG_SHADOW  0x606060u
#define WG_INK     0x000000u
#define WG_ACCENT  0x000080u
#define WG_PAPER   0xFFFFFFu
#define WG_DIM     0x808080u

#define WG_GLYPH_W 8
#define WG_GLYPH_H 16

/* Point drawing at a buffer. Every call below clips to it, so a client can draw
 * in window coordinates without checking whether it fits. */
void wg_target(uint32_t* pixels, unsigned width, unsigned height);

/* Load the console font. Call once; without it text draws nothing. */
int  wg_font(void);

void wg_plot(int x, int y, uint32_t colour);
void wg_fill(int x, int y, int w, int h, uint32_t colour);

/* Raised, or sunken when `raised` is 0: light on the top and left edges, shadow
 * on the bottom and right, which is the whole visual language of this era. */
void wg_bevel(int x, int y, int w, int h, int raised);

void wg_text(int x, int y, const char* s, uint32_t colour);

/* Text clipped to `max_w` pixels, ending in ".." when it does not fit - which
 * is what a file listing needs far more often than it needs the whole name. */
void wg_text_clipped(int x, int y, const char* s, uint32_t colour, int max_w);

/* A push button, drawn pressed when `down`. */
void wg_button(int x, int y, int w, int h, const char* label, int down);

#endif /* _WIDGET_H */
