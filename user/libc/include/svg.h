#ifndef _SVG_H
#define _SVG_H

#include <draw.h>
#include <stdint.h>

/* Vector icons.
 *
 * Not an SVG renderer. The glyphs this system's chrome uses are Material
 * Symbols, which are files of one shape - a viewBox and a single <path> - so
 * this finds the paths, walks their commands and fills them with the same
 * rasteriser the font uses. There is no styling, no grouping, no transform
 * stack and no text, because an icon needs none of those and a general
 * renderer is several thousand lines that would never run.
 *
 * The point of keeping them vector is size: the same close box is drawn at ten
 * pixels in a title bar and at twenty-four in a menu, and both are sharp.
 */

struct svg_icon {
    unsigned char* coverage;    /* w * h, 0..255 - not a colour */
    int w, h;
};

/* Fill the icon at `size` by `size` pixels. Zero on success. A non-square
 * viewBox is letterboxed rather than stretched. */
int  svg_render(const char* path, int size, struct svg_icon* out);
void svg_free(struct svg_icon* icon);

/* Paint it in one colour. An icon is a shape, so the colour is the caller's -
 * which is what lets the same file be a dark glyph on glass and a light one on
 * a dock. */
void svg_draw(const struct surface* s, const struct svg_icon* icon,
              int x, int y, uint32_t colour);

#endif /* _SVG_H */
