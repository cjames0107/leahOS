#ifndef _WIDGET_H
#define _WIDGET_H

#include <stdint.h>

/* The small amount of drawing every window client turns out to need.
 *
 * Not a toolkit: a client owns a rectangle of pixels and draws it itself, and
 * these are just the four operations it takes to do that in the desktop's
 * idiom. They live here because paint, uitest, the browser and the editor were
 * otherwise going to carry four copies of the same bevel routine. */

/* Near-white rather than the old plate grey. The whole interface is a light
 * surface with soft shadows now, and a mid grey face is the one thing that
 * still read as a machine from 1991. */
#define WG_FACE    0xF2F4F7u
#define WG_LIGHT   0xFFFFFFu
#define WG_SHADOW  0x9AA3AEu
#define WG_INK     0x000000u
#define WG_ACCENT  0x2C6BEDu
#define WG_PAPER   0xFFFFFFu
#define WG_DIM     0x8B94A0u

/* One radius for every control in the interface, matching the windows they sit
 * in. Smaller than a window's, because a button inside a rounded panel with the
 * same radius reads as a hole rather than as a control. */
#define WG_RADIUS  6

/* The bitmap cell these were, kept because layout code still counts in them.
 * Text is proportional now: ask wg_text_width what a string actually measures
 * rather than multiplying by eight. */
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

/* An RGB picker: three sliders and a preview, occupying WG_RGB_H pixels.
 * A grid of ready-made colours can only ever offer the colours somebody
 * thought of; this offers all of them, and shows the one being made. */
#define WG_RGB_H 62

/* A plain horizontal slider: a track, a filled part, and a thumb. Separate
 * from the RGB picker because a quantity is not a colour - there is nothing
 * to show along the track but how far along it you are. */
#define WG_SLIDER_H 18
void wg_slider_draw(int x, int y, int w, int value, int max);
int  wg_slider_hit(int x, int y, int w, int mx, int my);      /* 1 or 0 */
int  wg_slider_value(int x, int w, int mx, int max);
void     wg_rgb_draw(int x, int y, int w, uint32_t colour);
int      wg_rgb_hit(int x, int y, int w, int mx, int my);   /* channel or -1 */
uint32_t wg_rgb_move(uint32_t colour, int channel, int x, int w, int mx);
uint32_t wg_body_colour(void);  /* a content-area background */
uint32_t wg_ink_colour(void);   /* ink on that background    */
unsigned wg_scale(void);        /* 1 or 2 - the text scale   */

/* Load the console font. Call once; without it text draws nothing. */
int  wg_font(void);

void wg_plot(int x, int y, uint32_t colour);
void wg_fill(int x, int y, int w, int h, uint32_t colour);

/* Blit an icon, skipping its transparent pixels. See <icon.h> for where the
 * pixels come from. */
void wg_icon(int x, int y, const uint32_t* px, int w, int h);

/* The same, resampled - for the list view, where a 32-pixel icon in an
 * 18-pixel row would draw over its neighbours. */
void wg_icon_scaled(int x, int y, const uint32_t* px, int sw, int sh,
                    int dw, int dh);

/* Raised, or sunken when `raised` is 0: light on the top and left edges, shadow
 * on the bottom and right, which is the whole visual language of this era. */
void wg_bevel(int x, int y, int w, int h, int raised);

void wg_text(int x, int y, const char* s, uint32_t colour);

/* What a string measures, in pixels, in the font actually being used. The
 * glyphs are not all one width any more, so strlen times eight is a guess that
 * is wrong in both directions. */
int  wg_text_width(const char* s);
int  wg_text_height(void);
int  wg_text_size(void);

/* Text clipped to `max_w` pixels, ending in ".." when it does not fit - which
 * is what a file listing needs far more often than it needs the whole name. */
void wg_text_clipped(int x, int y, const char* s, uint32_t colour, int max_w);

/* A push button, drawn pressed when `down`. */
void wg_button(int x, int y, int w, int h, const char* label, int down);

/* --- the glass vocabulary --------------------------------------------------
 *
 * Each of these asks the server whether the glass is on, because that changes
 * what "slightly apart from its background" has to mean: a wash of white over
 * a blurred backdrop, or a shade darker over a flat one. A window using them
 * looks right in both without knowing which it is in. */

/* What a window's background is, which is also what its title bar is - the
 * server paints the frame this colour so the two meet without a seam. */
uint32_t wg_base_colour(void);

/* Paint the window's background. With the glass on this leaves alpha in the
 * pixels so the server blends them over the blur; with it off it is the flat
 * panel colour. A window using this must have called win_set_alpha. */
void wg_glass_clear(void);

/* A box that holds other things. `pad` is its distance from the window edge
 * and the corner radius follows from it: pressed against the edge wants a
 * tight curve, floating in space can afford a generous one. */
int  wg_container_radius(int pad);
void wg_container(int x, int y, int w, int h, int pad);

/* Full height, square, a shade apart from the content beside it. */
void wg_sidebar(int x, int y, int w, int h);

/* Controls that belong together share one pill; anything doing a different
 * kind of job gets its own. `selected` is the pressed segment, or -1. */
void wg_pill_group(int x, int y, int seg_w, int h, int count,
                   const char* const* labels, int selected);
void wg_pill(int x, int y, int w, int h, const char* label, int down);

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
