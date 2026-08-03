#ifndef _SCREEN_H
#define _SCREEN_H

/* Drawing straight onto the framebuffer, without a window server.
 *
 * For the two things that run before there is one: the boot splash and the
 * login screen. Everything else should open a window - this has no clipping
 * beyond the screen edge, no overlap, and no idea that anything else exists.
 *
 * Colours are 0x00RRGGBB.
 */

int screen_open(void);

unsigned screen_width(void);
unsigned screen_height(void);

void screen_fill(int x, int y, int w, int h, unsigned rgb);
void screen_frame(int x, int y, int w, int h, unsigned rgb);

/* `transparent` leaves the background pixels alone, which is what text over
 * something already drawn needs. */
void screen_char(int x, int y, char c, unsigned fg, unsigned bg, int transparent);
void screen_text(int x, int y, const char* s, unsigned fg, unsigned bg,
                 int transparent);
void screen_text_centred(int centre_x, int y, const char* s, unsigned fg,
                         unsigned bg, int transparent);

int screen_text_width(const char* s);
int screen_glyph_width(void);
int screen_glyph_height(void);

#endif /* _SCREEN_H */
