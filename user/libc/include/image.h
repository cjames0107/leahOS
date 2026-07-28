#ifndef _IMAGE_H
#define _IMAGE_H

#include <stdint.h>

/* Writing images out.
 *
 * Both formats here are real ones - a PNG written by this is a PNG, not a
 * lookalike - but neither actually compresses. PNG's deflate stream is emitted
 * as *stored* blocks, which the format explicitly allows, and GIF's LZW is
 * emitted with the table cleared before it ever fills, which is the standard
 * way to produce valid LZW without implementing the dictionary. The result is
 * larger than it needs to be and correct, which is the right trade for a system
 * that has no compressor yet: a wrong file that is small is worth nothing.
 *
 * Pixels are 0x00RRGGBB, `width * height` of them, top row first - which is the
 * layout a window's buffer already has.
 */

/* Returns 0, or -1 if the file could not be written. */
int img_write_png(const char* path, const uint32_t* px,
                  unsigned width, unsigned height);

/* GIF is palettised, so this quantises to the 216-entry colour cube plus a grey
 * ramp. Flat-coloured drawings come through exactly; photographs would not,
 * which is the format's own limitation rather than this one's. */
int img_write_gif(const char* path, const uint32_t* px,
                  unsigned width, unsigned height);

/* Read a PNG back. Returns a malloc'd buffer of `width * height` pixels, or 0.
 *
 * It reads what this system writes: 8-bit truecolour, no interlacing, stored
 * deflate blocks. A compressed one needs an inflate that does not exist here
 * yet, and this says so by failing rather than by showing noise. */
uint32_t* img_read_png(const char* path, unsigned* width, unsigned* height);

#endif /* _IMAGE_H */
