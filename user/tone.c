/* tone - play a note, so there is a way to hear whether audio works.
 *
 * A sine from a 256-entry table, stepped by a fixed-point phase increment.
 * There is no floating point here and a sine is not something to compute a
 * sample at a time anyway; a table and a 16.16 phase accumulator give an exact
 * frequency and cost two adds per sample.
 */

#include <audio.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* One full cycle, amplitude +/-32767. */
static const short kSine[256] = {
         0,    804,   1608,   2410,   3212,   4011,   4808,   5602,
      6393,   7179,   7962,   8739,   9512,  10278,  11039,  11793,
     12539,  13279,  14010,  14732,  15446,  16151,  16846,  17530,
     18204,  18868,  19519,  20159,  20787,  21403,  22005,  22594,
     23170,  23731,  24279,  24811,  25329,  25832,  26319,  26790,
     27245,  27683,  28105,  28510,  28898,  29268,  29621,  29956,
     30273,  30571,  30852,  31113,  31356,  31580,  31785,  31971,
     32137,  32285,  32412,  32521,  32609,  32678,  32728,  32757,
     32767,  32757,  32728,  32678,  32609,  32521,  32412,  32285,
     32137,  31971,  31785,  31580,  31356,  31113,  30852,  30571,
     30273,  29956,  29621,  29268,  28898,  28510,  28105,  27683,
     27245,  26790,  26319,  25832,  25329,  24811,  24279,  23731,
     23170,  22594,  22005,  21403,  20787,  20159,  19519,  18868,
     18204,  17530,  16846,  16151,  15446,  14732,  14010,  13279,
     12539,  11793,  11039,  10278,   9512,   8739,   7962,   7179,
      6393,   5602,   4808,   4011,   3212,   2410,   1608,    804,
         0,   -804,  -1608,  -2410,  -3212,  -4011,  -4808,  -5602,
     -6393,  -7179,  -7962,  -8739,  -9512, -10278, -11039, -11793,
    -12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530,
    -18204, -18868, -19519, -20159, -20787, -21403, -22005, -22594,
    -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790,
    -27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956,
    -30273, -30571, -30852, -31113, -31356, -31580, -31785, -31971,
    -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
    -32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285,
    -32137, -31971, -31785, -31580, -31356, -31113, -30852, -30571,
    -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683,
    -27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731,
    -23170, -22594, -22005, -21403, -20787, -20159, -19519, -18868,
    -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
    -12539, -11793, -11039, -10278,  -9512,  -8739,  -7962,  -7179,
     -6393,  -5602,  -4808,  -4011,  -3212,  -2410,  -1608,   -804
};

/* A short fade at each end. A tone that starts and stops at full amplitude
 * clicks, and the click is louder than the note. */
static short envelope(short sample, long i, long total)
{
    const long fade = AUDIO_RATE / 100;         /* 10 ms */
    long gain = 256;
    if (i < fade)                gain = i * 256 / fade;
    else if (i > total - fade)   gain = (total - i) * 256 / fade;
    if (gain < 0)   gain = 0;
    if (gain > 256) gain = 256;
    return (short)((int)sample * gain / 256);
}

int main(int argc, char** argv)
{
    const int hz = argc > 1 ? atoi_simple(argv[1]) : 440;
    const int ms = argc > 2 ? atoi_simple(argv[2]) : 500;
    if (hz <= 0 || ms <= 0) {
        printf("usage: tone [hz] [ms]\n");
        return 1;
    }

    const long frames = (long)AUDIO_RATE * ms / 1000;
    /* 16.16 fixed point: how far around the table each frame advances. */
    const unsigned step = (unsigned)(((unsigned long long)hz << 24) / AUDIO_RATE);
    unsigned phase = 0;

    static short chunk[512];        /* 256 stereo frames at a time */
    long done = 0;
    while (done < frames) {
        long n = 0;
        while (n < 256 && done + n < frames) {
            const short s = envelope(kSine[(phase >> 16) & 0xFF], done + n, frames);
            chunk[n * 2]     = s;   /* the same in both ears: it is a test, */
            chunk[n * 2 + 1] = s;   /* not a stereo demonstration */
            phase += step;
            ++n;
        }
        /* The queue is finite and does not block, so offer the remainder again
         * rather than assuming it was all taken. */
        long off = 0;
        while (off < n) {
            const long took = audio_play(&chunk[off * 2], (n - off) * 2);
            if (took <= 0) {
                msleep(5);
                continue;
            }
            off += took / 2;
        }
        done += n;
    }

    /* The last partial buffer needs handing over explicitly, and then the
     * queue needs time to drain: exiting immediately cuts off the tail. */
    audio_flush();
    msleep(ms + 250);
    return 0;
}
