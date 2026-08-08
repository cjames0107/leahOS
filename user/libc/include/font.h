#ifndef _FONT_H
#define _FONT_H

#include <stdint.h>

/* Text with real letterforms.
 *
 * The font until now was the 8x16 one-bit glyph set lifted out of the video
 * BIOS: one size, one weight, every character the same width, and a hard edge
 * on every stroke. It was the right thing while the screen looked like 1984.
 * Nothing built on it can look like anything else.
 *
 * This reads TrueType outlines and fills them with coverage antialiasing, so a
 * letter is a shape rather than a stamp, at whatever size is asked for.
 *
 * What is understood: `glyf` outlines, which are quadratic and are what nearly
 * every TrueType file holds. Not CFF, which stores cubic outlines as a
 * bytecode nobody can read without an interpreter for it. Not hinting - the
 * `fpgm`/`prep`/`glyf` instruction streams are a stack language for nudging
 * points onto the pixel grid at small sizes, and modern rendering leaves them
 * alone anyway. Not the OpenType layout tables, so no kerning pairs, no
 * ligatures and no shaping: text is laid out one glyph at a time, left to
 * right, which is right for the Latin alphabet and wrong for a good deal else.
 *
 * A variable font is read at its default instance. The deltas that move it
 * along its axes live in `gvar`, and ignoring that table is exactly what gives
 * the default - so Google Sans Flex renders at the weight and width it was
 * drawn at, which is the one wanted here.
 */

struct font;

/* Read a font. Null if the file is missing or is not something this can draw
 * from - which is a thing to check, because "no CFF here" is a real answer. */
struct font* font_open(const char* path);
void         font_close(struct font* f);

/* Why the last font_open returned null. Never null itself. */
const char*  font_error(void);

/* One glyph, rasterised at one size.
 *
 * `coverage` is `w * h` bytes of how much of each pixel the outline covers,
 * from 0 to 255 - not a colour. The caller decides what to do with that, which
 * is what lets the same glyph be drawn in any colour over any background
 * without the rasteriser knowing about either.
 *
 * `left` and `top` are where the bitmap goes relative to the pen: `left` from
 * the pen's x, `top` *above* the baseline, both in pixels and both frequently
 * negative for glyphs that hang below or start left of where the pen is.
 *
 * The bitmap belongs to the font and stays valid until it is closed. Glyphs
 * are cached, so asking twice costs one rasterisation.
 */
struct glyph {
    const unsigned char* coverage;
    int w, h;
    int left, top;
    int advance;        /* how far the pen moves, in whole pixels */
};

/* Fill `out` for one Unicode character at `px` pixels tall (the em size, not
 * the cap height). Zero on success. A character the font has no glyph for
 * comes back as the font's own notdef, which is what a reader should see. */
int font_glyph(struct font* f, int px, unsigned codepoint, struct glyph* out);

/* Vertical metrics at a size, in pixels. The baseline sits `ascent` below the
 * top of a line, and successive baselines are `line_height` apart. */
int font_ascent(struct font* f, int px);
int font_descent(struct font* f, int px);
int font_line_height(struct font* f, int px);

/* How wide a string will be. UTF-8 in, pixels out. */
int font_width(struct font* f, int px, const char* text);

/* One character out of a UTF-8 string, advancing `at`. Malformed bytes come
 * back as U+FFFD rather than stopping the walk: text that is nearly right
 * should still be nearly readable. */
unsigned utf8_next(const char** at);

#endif /* _FONT_H */
