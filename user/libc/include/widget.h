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

/* Pick up the desktop's theme, so a client's selection highlight and body
 * colour match every other client's. Safe to call every frame - it only reads
 * the shared block, and does nothing when there is no window server. The
 * colours below fall back to the built-in ones until it is called. */
void wg_theme(void);

uint32_t wg_sel_colour(void);   /* the selection highlight   */
uint32_t wg_body_colour(void);  /* a content-area background */
uint32_t wg_ink_colour(void);   /* ink on that background    */
unsigned wg_scale(void);        /* 1 or 2 - the text scale   */

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

/* --- scrollbars -----------------------------------------------------------
 *
 * Drawn and hit-tested, but the scroll position stays the caller's: a widget
 * that owned it would have to be told about every change of content, and the
 * caller already knows. `span` is the whole extent, `page` how much is visible.
 */
#define WG_SCROLL_W 14

void wg_scrollbar_v(int x, int y, int h, int first, int page, int span);
void wg_scrollbar_h(int x, int y, int w, int first, int page, int span);

/* Where a click on a bar wants to go: returns the new `first`, or the old one
 * when the click was not on the bar. Clicking the trough pages; clicking an
 * arrow steps. */
int wg_scroll_hit_v(int x, int y, int bx, int by, int bh,
                    int first, int page, int span);
int wg_scroll_hit_h(int x, int y, int bx, int by, int bw,
                    int first, int page, int span);

/* Whether a point is on a bar's thumb, so a caller can begin a drag. */
int wg_scroll_on_thumb_v(int y, int by, int bh, int first, int page, int span);
int wg_scroll_on_thumb_h(int x, int bx, int bw, int first, int page, int span);

/* Where a drag to this coordinate puts `first`. The thumb follows the pointer
 * rather than the pointer following the thumb, which is what makes a long list
 * usable. */
int wg_scroll_drag_v(int y, int by, int bh, int page, int span);
int wg_scroll_drag_h(int x, int bx, int bw, int page, int span);
