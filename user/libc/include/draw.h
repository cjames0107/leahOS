#ifndef _DRAW_H
#define _DRAW_H

#include <stdint.h>

/* The pixels underneath a modern interface.
 *
 * Four things, and the whole look is made of them: blending one colour over
 * another, filling a rounded rectangle with a soft edge, blurring a region,
 * and casting a shadow. The old chrome needed none of these - a bevel is two
 * one-pixel lines and a fill is a memset - which is why they live in a new
 * file rather than growing out of widget.c.
 *
 * A surface is somebody else's pixels: a window's buffer, the compositor's
 * back buffer, a scratch bitmap. Nothing here allocates one or owns one.
 *
 * Colours are 0xAARRGGBB and are *not* premultiplied. Premultiplied alpha is
 * the better representation for compositing - it makes `over` two multiplies
 * instead of four and makes repeated blending exact - but every other pixel in
 * this system is straight, from PNG decoding to the window buffers, and one
 * format that is right everywhere beats two that need converting between.
 */

struct surface {
    uint32_t* pixels;
    int       w, h;
    /* How far apart the rows of `pixels` are, when that is not `w`.
     *
     * A window's buffer is allocated with room to spare so a resize can change
     * what is shown without allocating anything, which puts its rows
     * round_up(width) apart - essentially never the width itself. Drawing into
     * such a buffer at its width writes each row a little further along than
     * the one before, and whatever reads it at the real stride gets the image
     * sheared into bands, worse the further down it goes.
     *
     * Zero means "the same as w", which is what a plain `{pixels, w, h}`
     * gives and is right for every buffer with no slack in it. */
    int       stride;
    /* What may be written to. A compositor repaints one damage rectangle at a
     * time and anything drawn outside it corrupts the screen, so the clip
     * belongs on the surface rather than in every caller. A zero width means
     * the whole surface, which is what a plain `{pixels, w, h}` gives. */
    int       cx, cy, cw, ch;
};

/* Whether (x, y) may be written. Exposed because a caller with its own
 * per-pixel loop - a glyph blitter, say - needs the same test. */
int draw_clipped(const struct surface* s, int x, int y);

/* --- blending -------------------------------------------------------------- */

/* `over` composited onto `under`, both 0xAARRGGBB. The result is opaque when
 * `under` was: this is painting onto something, not stacking transparencies. */
uint32_t draw_over(uint32_t under, uint32_t over);

/* One pixel, blended. Out-of-bounds coordinates are dropped rather than
 * wrapped, so a caller may draw past an edge without checking first. */
void draw_pixel(const struct surface* s, int x, int y, uint32_t colour);

/* A rectangle, blended. */
void draw_rect(const struct surface* s, int x, int y, int w, int h,
               uint32_t colour);

/* --- rounded rectangles ----------------------------------------------------
 *
 * Every panel, button and window in the new chrome is one of these, so it is
 * worth doing properly: the edge is antialiased from the true distance to the
 * shape rather than from a precomputed corner stamp, which means any radius
 * works and a radius larger than the box degrades into a capsule instead of
 * into a mess.
 */

/* Filled. `radius` is clamped to half the shorter side. */
void draw_round_rect(const struct surface* s, int x, int y, int w, int h,
                     int radius, uint32_t colour);

/* Outlined, `thickness` pixels inside the edge. */
void draw_round_rect_outline(const struct surface* s, int x, int y,
                             int w, int h, int radius, int thickness,
                             uint32_t colour);

/* How much of the pixel at (px, py) is inside that rounded rectangle, 0 to
 * 255. The primitive the two above are built from, exposed because a caller
 * clipping something else to a rounded shape - a window's own contents, say -
 * needs the same answer. */
int draw_round_coverage(int px, int py, int x, int y, int w, int h,
                        int radius);

/* --- blur ------------------------------------------------------------------
 *
 * Three box blurs approximate a gaussian closely enough that nobody can tell,
 * and each one is O(pixels) regardless of radius because it slides a running
 * sum along instead of re-adding a window per pixel.
 *
 * The region is shrunk before blurring and stretched back afterwards. That is
 * sixteen times less work at a quarter scale, and it is invisible: the output
 * is a blur, so the detail thrown away on the way down was going to be
 * destroyed anyway. It is the difference between this being affordable and not.
 */
/* `corner` rounds the blurred region to match the panel that will sit on it.
 * Zero blurs the plain rectangle. It matters: a rectangular blur behind a
 * rounded panel leaves four soft squares poking out from behind the glass. */
void draw_blur(const struct surface* s, int x, int y, int w, int h,
               int radius, int corner);

/* --- filling a path ---------------------------------------------------------
 *
 * The rasteriser the font uses, shared so that vector icons are filled by the
 * same code. Deposit every edge of every closed contour into `area`, which is
 * `(w + 2) * h` floats and starts at zero, then resolve it into `w * h` bytes
 * of coverage. The two extra columns are slack for an edge that leaves the
 * right-hand side.
 */
void draw_edge_deposit(float* area, int w, int h, int stride,
                       float x0, float y0, float x1, float y1);
void draw_area_resolve(const float* area, int w, int h, int stride,
                       unsigned char* out);

/* --- shadows ---------------------------------------------------------------
 *
 * A shadow depends only on the shape casting it - its size and its corner
 * radius - and never on what is inside it, so one can be built once and used
 * for every window of that size. That is the whole reason this takes a size
 * rather than a surface.
 */

struct shadow;

/* Build one for a `w` by `h` rounded rectangle, blurred by `radius` and at
 * `opacity` out of 255. Null if there is no memory for it. */
struct shadow* draw_shadow_make(int w, int h, int corner, int radius,
                                int opacity);
void           draw_shadow_free(struct shadow* s);

/* Cast it, with the shape's top-left at (x, y). The shadow extends beyond
 * that on every side, and is offset down by `drop` to suggest a light source
 * above - which is the one thing that stops it reading as a halo. */
void draw_shadow_cast(const struct surface* s, const struct shadow* shadow,
                      int x, int y, int drop);

#endif /* _DRAW_H */
