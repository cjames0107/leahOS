#ifndef _AUD_H
#define _AUD_H

#include <stdint.h>

/* The sound card, as seen from outside the process that drives it.
 *
 * Samples go through a shared ring rather than in messages, for the reason
 * frames and sectors do: a message holds 256 bytes and a fifth of a second of
 * stereo is thirty-odd kilobytes.
 *
 * The ring is a queue the driver drains, not a slot that has to be handed over
 * one buffer at a time - a program feeding audio has to be able to get ahead of
 * the card, or every gap in its scheduling is a gap you can hear.
 */

#define AUD_SHM_KEY 0x4155u         /* "AU" */
#define AUD_RING    32768           /* samples, not bytes: about a third of a
                                       second of stereo at 48 kHz */

struct aud_shared {
    volatile uint32_t head;         /* the writer fills here   */
    volatile uint32_t tail;         /* the driver drains here  */
    volatile uint32_t reserved[2];
    int16_t samples[AUD_RING];
};

/* Message tags on IPC_PORT_AUDIO. */
#define AUD_INFO   1    /* -> w0 = rate, w1 = channels, data = the device name */
#define AUD_KICK   2    /* w0 = samples now in the ring -> w0 = taken          */
#define AUD_SPACE  3    /* -> w0 = how many samples would fit                  */
#define AUD_FLUSH  4    /* hand over a part-filled buffer                      */
#define AUD_STOP   5    /* drop what is queued                                 */
#define AUD_VOLUME 6    /* w0 < 0 reads, otherwise sets 0..100 -> w0 = volume  */

#endif
