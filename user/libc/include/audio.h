#ifndef _AUDIO_H
#define _AUDIO_H

#include <stdint.h>

/* Sound, in exactly one format: interleaved 16-bit signed samples, left then
 * right, 48000 of each per second. That is what the hardware wants, so nothing
 * in between has to resample and there is no format negotiation to get wrong.
 * A program with something else converts before it calls here. */
#define AUDIO_RATE     48000
#define AUDIO_CHANNELS 2

struct audio_info {
    unsigned present;           /* 0 when there is no output device */
    unsigned rate;
    unsigned channels;
    char     name[32];
};

int audio_info(struct audio_info* out);

/* Queue samples. Returns how many were taken, which may be fewer than offered
 * and may be zero: this never blocks, because the programs that make sound are
 * also the programs drawing windows. Offer the remainder again later. */
long audio_play(const int16_t* samples, long count);

/* How many samples would be taken right now. */
long audio_space(void);

/* Master volume, 0..100. Both calls return the volume now in effect, so
 * setting it and reading it back is one call. 0 is silence. */
int  audio_volume(void);
int  audio_set_volume(int percent);

/* Hand over a part-filled buffer. audio_play only passes on whole ones, so a
 * program that has finished has to say so or lose its last few milliseconds. */
void audio_flush(void);

/* Drop everything queued and go quiet immediately. */
void audio_stop(void);

#endif
