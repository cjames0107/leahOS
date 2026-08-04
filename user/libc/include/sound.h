#ifndef _SOUND_H
#define _SOUND_H

#include <stdint.h>

/* Reading sound files, a piece at a time.
 *
 * Streaming rather than "here is the whole song": three minutes of 48 kHz
 * stereo is thirty-four megabytes, and a music player that loads that before
 * making a noise is a music player that appears broken for several seconds.
 * The caller asks for as many samples as it can currently queue and gets them.
 *
 * Everything comes out in the one format audio.h accepts - 48 kHz, stereo,
 * interleaved 16-bit - whatever the file was. A file at another rate or in
 * mono is converted on the way through, so nothing above this has to know or
 * care. That conversion is linear interpolation, which is not the resampler a
 * mastering engineer would want and is entirely adequate for playing a tune.
 *
 * Which formats are actually understood is snd_formats(). Asking is better
 * than assuming: this list is expected to grow, and a caller that hardcodes it
 * will be wrong later.
 */

struct sound;

/* Open a file. Returns 0 if it could not be read, with snd_error() saying why
 * - which distinguishes "no such file" from "this is an MP3 and there is no
 * decoder for one yet", and those need different things from the person. */
struct sound* snd_open(const char* path);

void snd_close(struct sound* s);

const char* snd_error(void);

/* What the file turned out to be. `rate` and `channels` are the file's own,
 * before conversion, because that is what a person wants told about a file.
 * `frames` is its length in output frames, so at 48 kHz - seconds times
 * 48000 - which is what a progress bar wants. Zero means the length is not
 * known in advance. */
void snd_info(const struct sound* s, unsigned* rate, unsigned* channels,
              unsigned long* frames, const char** format);

/* Up to `max_samples` interleaved 16-bit samples at 48 kHz stereo. Returns how
 * many were produced - fewer than asked for only at the end of the file, where
 * it returns 0 from then on. `max_samples` counts samples, not frames, so it
 * should be even. */
long snd_read(struct sound* s, int16_t* out, long max_samples);

/* Move to `frame` (a 48 kHz output frame). Returns 0, or -1 if the format
 * cannot seek. */
int snd_seek(struct sound* s, unsigned long frame);

/* How far in we are, in output frames. */
unsigned long snd_position(const struct sound* s);

/* A human-readable list of what can be read, for a program that wants to say
 * so rather than fail one file at a time. */
const char* snd_formats(void);

#endif /* _SOUND_H */
