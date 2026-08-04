/* Reading sound files.
 *
 * The shape here is a decoder behind a stream: snd_open works out what the
 * file is, and snd_read pulls the next few thousand samples through whatever
 * that turned out to need. Everything leaves in one format - 48 kHz stereo,
 * interleaved 16-bit - so nothing above has to negotiate.
 *
 * WAV is implemented. MP3 and Ogg Vorbis are recognised and refused with a
 * message that says so, which is a better answer than silence: the file
 * opening and producing nothing is indistinguishable from a broken speaker.
 * See the note by sniff() for what each of those two actually needs.
 */

#include <audio.h>
#include <fcntl.h>
#include <sound.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FORMAT_WAV  1
#define FORMAT_MP3  2
#define FORMAT_OGG  3

/* A block of file at a time. Big enough that the disk is read in useful
 * chunks, small enough that it is not worth streaming from a streaming
 * reader. */
#define CHUNK 16384

struct sound {
    int      fd;
    int      format;
    const char* format_name;

    /* The file's own shape, before conversion. */
    unsigned rate;
    unsigned channels;
    unsigned bits;
    int      is_float;

    unsigned long data_start;       /* byte offset of the first sample */
    unsigned long data_bytes;       /* length of the sample data */
    unsigned long data_pos;         /* how far into it we have read */

    unsigned long out_frames;       /* total length, in 48 kHz frames */
    unsigned long out_pos;          /* how far we have produced */

    /* Resampling state. `phase` is where we are between two input frames, as
     * a fraction; `have`/`prev` are the two frames being interpolated. */
    double        phase;
    double        ratio;            /* input frames per output frame */
    int           prev[2];
    int           next[2];
    int           primed;

    unsigned char buf[CHUNK];
    unsigned long buf_len;
    unsigned long buf_at;
};

static char g_error[128] = "";

const char* snd_error(void) { return g_error; }

const char* snd_formats(void)
{
    return "WAV (PCM 8/16/24/32-bit and 32-bit float)";
}

static void fail(const char* what)
{
    snprintf(g_error, sizeof(g_error), "%s", what);
}

/* --- little-endian reads ---------------------------------------------------- */

static unsigned rd16(const unsigned char* p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned long rd32(const unsigned char* p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

/* --- what is this file? -------------------------------------------------------
 *
 * By content, not by name. A file called .wav that is an MP3 should be told
 * apart, and one called .dat that is a WAV should play.
 *
 * MP3 and Ogg are recognised here and then refused, deliberately. Both need a
 * real decoder and neither is small:
 *
 *   MP3 is Huffman-coded spectral data, requantised, alias-reduced, put
 *   through an 18-point IMDCT and then a 32-band polyphase synthesis filter.
 *   The awkward part is not the algorithm but the tables - thirty-four Huffman
 *   code tables and a 512-entry synthesis window, all specified by ISO 11172-3
 *   and none of them derivable. They have to be transcribed correctly or the
 *   output is confident noise.
 *
 *   Ogg Vorbis carries its codebooks in the file rather than in the standard,
 *   so it has no such table problem, but the decoder is larger: codebook
 *   unpacking, floor curves, residue vectors, channel coupling and an MDCT.
 *
 * Both are jobs of their own rather than a corner of this file. */
static int sniff(struct sound* s, const unsigned char* head, long n)
{
    if (n >= 12 && memcmp(head, "RIFF", 4) == 0 && memcmp(head + 8, "WAVE", 4) == 0) {
        s->format = FORMAT_WAV;
        s->format_name = "WAV";
        return 0;
    }
    if (n >= 4 && memcmp(head, "OggS", 4) == 0) {
        s->format = FORMAT_OGG;
        s->format_name = "Ogg";
        fail("Ogg Vorbis: no decoder for one yet");
        return -1;
    }
    /* An MP3 is either an ID3 tag or a frame sync: eleven set bits. */
    if ((n >= 3 && memcmp(head, "ID3", 3) == 0) ||
        (n >= 2 && head[0] == 0xFF && (head[1] & 0xE0) == 0xE0)) {
        s->format = FORMAT_MP3;
        s->format_name = "MP3";
        fail("MP3: no decoder for one yet");
        return -1;
    }
    fail("not a sound file this system knows");
    return -1;
}

/* --- WAV ----------------------------------------------------------------------
 *
 * A RIFF file is a list of chunks, and the two that matter are `fmt ` and
 * `data`. Everything else is skipped by its own length rather than by being
 * recognised - which is not a nicety: the files this system's own build
 * produces carry a four-kilobyte `FLLR` padding chunk between the two, and a
 * reader that assumed `data` came straight after `fmt ` would play it. */
static int parse_wav(struct sound* s)
{
    unsigned char header[8];
    unsigned long at = 12;                      /* past "RIFF....WAVE" */
    int have_fmt = 0;

    for (;;) {
        if (lseek(s->fd, (long)at, SEEK_SET) != (long)at)
            break;
        if (read(s->fd, header, 8) != 8)
            break;
        const unsigned long size = rd32(header + 4);

        if (memcmp(header, "fmt ", 4) == 0) {
            unsigned char fmt[40];
            const unsigned long want = size > sizeof(fmt) ? sizeof(fmt) : size;
            if ((unsigned long)read(s->fd, fmt, want) != want)
                break;
            unsigned tag = rd16(fmt);
            s->channels = rd16(fmt + 2);
            s->rate     = (unsigned)rd32(fmt + 4);
            s->bits     = rd16(fmt + 14);
            /* WAVE_FORMAT_EXTENSIBLE says "look at the subformat GUID", whose
             * first two bytes are the tag it stands in for. */
            if (tag == 0xFFFE && want >= 26)
                tag = rd16(fmt + 24);
            if (tag == 3)
                s->is_float = 1;
            else if (tag != 1) {
                fail("WAV, but compressed - only PCM is understood");
                return -1;
            }
            have_fmt = 1;
        } else if (memcmp(header, "data", 4) == 0) {
            s->data_start = at + 8;
            s->data_bytes = size;
            if (!have_fmt) {
                fail("WAV with its data before its format");
                return -1;
            }
            break;
        }
        /* Chunks are padded to an even length, and the pad byte is not counted
         * in the size. Missing that walks the parser one byte out of step. */
        at += 8 + size + (size & 1);
    }

    if (!have_fmt || s->data_bytes == 0) {
        fail("WAV with no sample data");
        return -1;
    }
    if (s->channels == 0 || s->channels > 8 || s->rate == 0) {
        fail("WAV with an impossible format");
        return -1;
    }
    if (s->is_float ? (s->bits != 32)
                    : (s->bits != 8 && s->bits != 16 && s->bits != 24 && s->bits != 32)) {
        fail("WAV with a sample size that is not 8, 16, 24 or 32 bits");
        return -1;
    }
    return 0;
}

/* One sample, as a signed value scaled to 16 bits. The formats differ in more
 * than width: 8-bit WAV is unsigned with a bias of 128, and everything wider
 * is signed. */
static int sample_from(const unsigned char* p, unsigned bits, int is_float)
{
    if (is_float) {
        float f;
        memcpy(&f, p, 4);
        /* Nominally -1..1, but nothing enforces that in the file, so clip. */
        double v = (double)f * 32767.0;
        if (v > 32767.0)  v = 32767.0;
        if (v < -32768.0) v = -32768.0;
        return (int)v;
    }
    switch (bits) {
    case 8:  return ((int)p[0] - 128) << 8;
    case 16: return (int)(short)(unsigned short)rd16(p);
    case 24: {
        long v = (long)p[0] | ((long)p[1] << 8) | ((long)p[2] << 16);
        if (v & 0x800000L)
            v -= 0x1000000L;
        return (int)(v >> 8);
    }
    default: {
        long v = (long)rd32(p);
        if (v & 0x80000000L)
            v -= 0x100000000LL;
        return (int)(v >> 16);
    }
    }
}

/* Pull one input frame, mixed down to two channels. Returns 0 at the end. */
static int next_frame(struct sound* s, int out[2])
{
    const unsigned width = s->bits / 8;
    const unsigned stride = width * s->channels;

    if (s->buf_at + stride > s->buf_len) {
        /* Refill, keeping whatever partial frame is left over. */
        const unsigned long left = s->buf_len - s->buf_at;
        memmove(s->buf, s->buf + s->buf_at, left);
        s->buf_len = left;
        s->buf_at = 0;

        unsigned long want = CHUNK - left;
        const unsigned long remaining = s->data_bytes - s->data_pos;
        if (want > remaining)
            want = remaining;
        if (want > 0) {
            const long got = read(s->fd, s->buf + left, want);
            if (got > 0) {
                s->buf_len += (unsigned long)got;
                s->data_pos += (unsigned long)got;
            }
        }
        if (s->buf_at + stride > s->buf_len)
            return 0;
    }

    const unsigned char* p = s->buf + s->buf_at;
    if (s->channels == 1) {
        out[0] = out[1] = sample_from(p, s->bits, s->is_float);
    } else {
        out[0] = sample_from(p, s->bits, s->is_float);
        out[1] = sample_from(p + width, s->bits, s->is_float);
        /* More than two channels: the extras are dropped rather than folded
         * in. Downmixing properly needs the channel layout, which is in a
         * chunk this does not read. */
    }
    s->buf_at += stride;
    return 1;
}

/* --- the public face ---------------------------------------------------------- */

struct sound* snd_open(const char* path)
{
    g_error[0] = '\0';

    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fail("cannot open the file");
        return 0;
    }

    struct sound* s = (struct sound*)malloc(sizeof(struct sound));
    if (s == 0) {
        close(fd);
        fail("out of memory");
        return 0;
    }
    memset(s, 0, sizeof(*s));
    s->fd = fd;

    unsigned char head[16];
    const long n = read(fd, head, sizeof(head));
    if (n < 4 || sniff(s, head, n) != 0) {
        if (g_error[0] == '\0')
            fail("not a sound file this system knows");
        close(fd);
        return 0;
    }

    if (parse_wav(s) != 0) {
        close(fd);
        return 0;
    }

    if (lseek(s->fd, (long)s->data_start, SEEK_SET) != (long)s->data_start) {
        fail("cannot reach the sample data");
        close(fd);
        return 0;
    }

    const unsigned long frame_bytes = (s->bits / 8) * s->channels;
    const unsigned long in_frames = s->data_bytes / frame_bytes;
    s->ratio = (double)s->rate / (double)AUDIO_RATE;
    s->out_frames = (unsigned long)((double)in_frames / s->ratio);
    return s;
}

void snd_close(struct sound* s)
{
    if (s == 0)
        return;
    close(s->fd);
    /* No free: this libc's is a no-op, and saying so is better than a call
     * that reads as reclamation and is not. */
}

void snd_info(const struct sound* s, unsigned* rate, unsigned* channels,
              unsigned long* frames, const char** format)
{
    if (s == 0)
        return;
    if (rate)     *rate = s->rate;
    if (channels) *channels = s->channels;
    if (frames)   *frames = s->out_frames;
    if (format)   *format = s->format_name;
}

unsigned long snd_position(const struct sound* s)
{
    return s ? s->out_pos : 0;
}

long snd_read(struct sound* s, int16_t* out, long max_samples)
{
    if (s == 0 || out == 0 || max_samples < 2)
        return 0;

    long produced = 0;

    /* The common case by far: the file is already at the output rate, so every
     * input frame is an output frame and there is nothing to interpolate. Worth
     * its own path - the general one does two multiplies and a compare per
     * sample to arrive at the same answer. */
    if (s->rate == AUDIO_RATE) {
        while (produced + 2 <= max_samples) {
            int frame[2];
            if (!next_frame(s, frame))
                break;
            out[produced++] = (int16_t)frame[0];
            out[produced++] = (int16_t)frame[1];
            ++s->out_pos;
        }
        return produced;
    }

    if (!s->primed) {
        if (!next_frame(s, s->prev))
            return 0;
        if (!next_frame(s, s->next))
            s->next[0] = s->prev[0], s->next[1] = s->prev[1];
        s->primed = 1;
    }

    while (produced + 2 <= max_samples) {
        /* Advance the input until the phase is inside the current pair. */
        while (s->phase >= 1.0) {
            s->prev[0] = s->next[0];
            s->prev[1] = s->next[1];
            if (!next_frame(s, s->next))
                return produced;
            s->phase -= 1.0;
        }
        const double t = s->phase;
        for (int c = 0; c < 2; ++c) {
            const double v = (double)s->prev[c] * (1.0 - t) + (double)s->next[c] * t;
            out[produced++] = (int16_t)(v < -32768.0 ? -32768.0
                                      : v > 32767.0 ? 32767.0 : v);
        }
        s->phase += s->ratio;
        ++s->out_pos;
    }
    return produced;
}

int snd_seek(struct sound* s, unsigned long frame)
{
    if (s == 0 || s->format != FORMAT_WAV)
        return -1;

    const unsigned long frame_bytes = (s->bits / 8) * s->channels;
    unsigned long in_frame = (unsigned long)((double)frame * s->ratio);
    unsigned long offset = in_frame * frame_bytes;
    if (offset > s->data_bytes)
        offset = s->data_bytes;
    /* Land on a frame boundary whatever the arithmetic did, or the channels
     * swap and stay swapped. */
    offset -= offset % frame_bytes;

    const long target = (long)(s->data_start + offset);
    if (lseek(s->fd, target, SEEK_SET) != target)
        return -1;

    s->data_pos = offset;
    s->buf_len = s->buf_at = 0;
    s->phase = 0.0;
    s->primed = 0;
    s->out_pos = frame;
    return 0;
}
