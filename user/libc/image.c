#include <fcntl.h>
#include <image.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* --- a growable output buffer -------------------------------------------- */
/*
 * Encoding into memory and writing once keeps the format code free of error
 * handling on every byte, and a whole image at this size is nothing.
 */
#define IMG_MAX (2u * 1024u * 1024u)
static unsigned char g_out[IMG_MAX];
static unsigned long g_len;

static void put(unsigned char b)
{
    if (g_len < IMG_MAX)
        g_out[g_len++] = b;
}

static void put_be32(uint32_t v)
{
    put((unsigned char)(v >> 24)); put((unsigned char)(v >> 16));
    put((unsigned char)(v >> 8));  put((unsigned char)v);
}

static int flush(const char* path)
{
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return -1;
    const long n = (long)write(fd, g_out, g_len);
    close(fd);
    return (n == (long)g_len) ? 0 : -1;
}

/* --- PNG ------------------------------------------------------------------ */

static uint32_t crc_table[256];
static int crc_ready;

static void crc_init(void)
{
    for (uint32_t n = 0; n < 256; ++n) {
        uint32_t c = n;
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_table[n] = c;
    }
    crc_ready = 1;
}

static uint32_t crc32_of(const unsigned char* buf, unsigned long len)
{
    if (!crc_ready)
        crc_init();
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned long i = 0; i < len; ++i)
        c = crc_table[(c ^ buf[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* A chunk is length, type, data, and a CRC over the type and data - so the CRC
 * is taken after the fact over the range just written. */
static void chunk(const char* type, unsigned long data_start)
{
    const unsigned long data_len = g_len - data_start;
    /* Splice the length in ahead of the type, which was reserved by the caller. */
    const unsigned long lenpos = data_start - 8;
    g_out[lenpos + 0] = (unsigned char)(data_len >> 24);
    g_out[lenpos + 1] = (unsigned char)(data_len >> 16);
    g_out[lenpos + 2] = (unsigned char)(data_len >> 8);
    g_out[lenpos + 3] = (unsigned char)data_len;
    (void)type;
    put_be32(crc32_of(&g_out[lenpos + 4], data_len + 4));
}

static unsigned long begin_chunk(const char* type)
{
    put(0); put(0); put(0); put(0);          /* length, filled in later */
    put((unsigned char)type[0]); put((unsigned char)type[1]);
    put((unsigned char)type[2]); put((unsigned char)type[3]);
    return g_len;                            /* where the data starts */
}

int img_write_png(const char* path, const uint32_t* px,
                  unsigned width, unsigned height)
{
    if (width == 0 || height == 0)
        return -1;
    g_len = 0;

    static const unsigned char sig[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
    for (int i = 0; i < 8; ++i)
        put(sig[i]);

    unsigned long at = begin_chunk("IHDR");
    put_be32(width);
    put_be32(height);
    put(8);                 /* 8 bits per channel */
    put(2);                 /* truecolour, no alpha */
    put(0); put(0); put(0); /* deflate, adaptive filtering, no interlace */
    chunk("IHDR", at);

    /* The raw scanlines: a filter byte of 0 then RGB triples. Adler-32 runs
     * over exactly these bytes, which is what the zlib trailer carries. */
    at = begin_chunk("IDAT");
    put(0x78); put(0x01);   /* zlib header: deflate, 32K window, no dictionary */

    const unsigned long row_bytes = 1u + (unsigned long)width * 3u;
    const unsigned long raw_total = row_bytes * height;
    uint32_t a = 1, b = 0;

    /* Stored deflate blocks: each is a header bit-triple padded to a byte, then
     * LEN and its complement, then the literal bytes. Blocks are capped at
     * 65535, so a large image needs several. */
    unsigned long done = 0;
    unsigned row = 0, col = 0, ch = 0;
    int wrote_filter = 0;
    while (done < raw_total) {
        unsigned long left = raw_total - done;
        unsigned block = (left > 65535ul) ? 65535u : (unsigned)left;
        const int final = (done + block == raw_total);
        put(final ? 1 : 0);
        put((unsigned char)(block & 0xFF));
        put((unsigned char)(block >> 8));
        put((unsigned char)(~block & 0xFF));
        put((unsigned char)((~block >> 8) & 0xFF));

        for (unsigned i = 0; i < block; ++i) {
            unsigned char byte;
            if (!wrote_filter) {
                byte = 0;               /* filter: none */
                wrote_filter = 1;
                ch = 0;
            } else {
                /* The position is carried in explicit counters rather than
                 * derived from the loop index: a deflate block boundary does
                 * not fall on a row boundary, so anything computed from the
                 * index within the block stops lining up after the first row. */
                const uint32_t p = px[(unsigned long)row * width + col];
                byte = (unsigned char)(ch == 0 ? (p >> 16) :
                                       ch == 1 ? (p >> 8) : p);
                if (++ch == 3) {
                    ch = 0;
                    if (++col == width) {
                        col = 0;
                        ++row;
                        wrote_filter = 0;
                    }
                }
            }
            put(byte);
            a = (a + byte) % 65521u;
            b = (b + a) % 65521u;
        }
        done += block;
    }
    put_be32((b << 16) | a);            /* adler-32 of the raw scanlines */
    chunk("IDAT", at);

    at = begin_chunk("IEND");
    chunk("IEND", at);
    return flush(path);
}

/* --- GIF ------------------------------------------------------------------ */

/* The 6x6x6 colour cube, then a 40-step grey ramp: 256 entries that cover flat
 * artwork exactly and everything else approximately. */
static unsigned char palette_index(uint32_t p)
{
    const unsigned r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
    if (r == g && g == b)
        return (unsigned char)(216 + (r * 39u) / 255u);
    return (unsigned char)((r * 5u / 255u) * 36u + (g * 5u / 255u) * 6u +
                           (b * 5u / 255u));
}

int img_write_gif(const char* path, const uint32_t* px,
                  unsigned width, unsigned height)
{
    if (width == 0 || height == 0 || width > 65535u || height > 65535u)
        return -1;
    g_len = 0;

    const char* hdr = "GIF89a";
    for (int i = 0; i < 6; ++i)
        put((unsigned char)hdr[i]);
    put((unsigned char)(width & 0xFF));  put((unsigned char)(width >> 8));
    put((unsigned char)(height & 0xFF)); put((unsigned char)(height >> 8));
    put(0xF7);              /* global table, 256 entries, 8 bits per channel */
    put(0);                 /* background index */
    put(0);                 /* no aspect ratio */

    for (unsigned i = 0; i < 216; ++i) {
        put((unsigned char)((i / 36) * 51));
        put((unsigned char)(((i / 6) % 6) * 51));
        put((unsigned char)((i % 6) * 51));
    }
    for (unsigned i = 0; i < 40; ++i) {
        const unsigned char v = (unsigned char)(i * 255u / 39u);
        put(v); put(v); put(v);
    }

    put(0x2C);              /* image descriptor */
    put(0); put(0); put(0); put(0);                 /* left, top */
    put((unsigned char)(width & 0xFF));  put((unsigned char)(width >> 8));
    put((unsigned char)(height & 0xFF)); put((unsigned char)(height >> 8));
    put(0);                 /* no local table, not interlaced */
    put(8);                 /* LZW minimum code size */

    /* LZW that never builds a dictionary: emit CLEAR, then one 9-bit code per
     * pixel, and CLEAR again before the codes would have to widen. Every decoder
     * handles this, and it costs a byte per pixel rather than a compressor. */
    const unsigned clear = 256, end = 257;
    unsigned bitbuf = 0, bitcount = 0, since_clear = 0;
    unsigned char block[255];
    unsigned block_len = 0;

    /* Codes go out little-endian across a byte stream that is itself carved
     * into sub-blocks of at most 255 bytes. */
    #define EMIT(code)                                                        \
        do {                                                                  \
            bitbuf |= ((unsigned)(code)) << bitcount;                         \
            bitcount += 9;                                                    \
            while (bitcount >= 8) {                                           \
                block[block_len++] = (unsigned char)(bitbuf & 0xFF);          \
                bitbuf >>= 8; bitcount -= 8;                                  \
                if (block_len == 255) {                                       \
                    put(255);                                                 \
                    for (unsigned q = 0; q < 255; ++q) put(block[q]);         \
                    block_len = 0;                                            \
                }                                                             \
            }                                                                 \
        } while (0)

    EMIT(clear);
    for (unsigned long i = 0; i < (unsigned long)width * height; ++i) {
        EMIT(palette_index(px[i]));
        /* The dictionary would reach 511 entries after 254 more codes; clear
         * well before that so nine bits always suffice. */
        if (++since_clear >= 250) {
            EMIT(clear);
            since_clear = 0;
        }
    }
    EMIT(end);
    if (bitcount > 0)
        block[block_len++] = (unsigned char)(bitbuf & 0xFF);
    if (block_len > 0) {
        put((unsigned char)block_len);
        for (unsigned q = 0; q < block_len; ++q)
            put(block[q]);
    }
    #undef EMIT
    put(0);                 /* block terminator */
    put(0x3B);              /* trailer */
    return flush(path);
}

/* --- reading a PNG back --------------------------------------------------- */

static uint32_t be32_of(const unsigned char* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

uint32_t* img_read_png(const char* path, unsigned* width, unsigned* height)
{
    const int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    const long len = (long)read(fd, g_out, IMG_MAX);
    close(fd);
    if (len < 8)
        return 0;

    static const unsigned char sig[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
    if (memcmp(g_out, sig, 8) != 0)
        return 0;

    unsigned long i = 8;
    unsigned char* idat = 0;
    unsigned long idat_len = 0;
    unsigned w = 0, h = 0, depth = 0, colour = 0, interlace = 0;
    while (i + 12 <= (unsigned long)len) {
        const uint32_t clen = be32_of(&g_out[i]);
        const unsigned char* type = &g_out[i + 4];
        unsigned char* data = &g_out[i + 8];
        if (i + 12 + clen > (unsigned long)len)
            break;
        if (memcmp(type, "IHDR", 4) == 0) {
            w = be32_of(data); h = be32_of(data + 4);
            depth = data[8]; colour = data[9]; interlace = data[12];
        } else if (memcmp(type, "IDAT", 4) == 0) {
            if (idat == 0) idat = data;
            idat_len += clen;
        }
        i += 12 + clen;
    }
    if (w == 0 || h == 0 || depth != 8 || colour != 2 || interlace != 0)
        return 0;
    if ((unsigned long)w * h > 4u * 1024u * 1024u || idat == 0 || idat_len < 2)
        return 0;

    uint32_t* out = (uint32_t*)malloc((unsigned long)w * h * 4);
    if (out == 0)
        return 0;

    /* Walk the stored blocks straight into the pixel array, keeping the
     * position in explicit counters for the same reason the writer does: a
     * block boundary does not fall on a row boundary. */
    unsigned long p = 2;                        /* past the zlib header */
    unsigned row = 0, col = 0, ch = 0;
    int want_filter = 1;
    for (;;) {
        if (p + 5 > idat_len)
            break;
        const unsigned char header = idat[p];
        const int final = header & 1;
        if (((header >> 1) & 3) != 0) {         /* compressed: no inflate here */
            free(out);
            return 0;
        }
        const unsigned blen = (unsigned)idat[p + 1] | ((unsigned)idat[p + 2] << 8);
        p += 5;
        if (p + blen > idat_len)
            break;
        for (unsigned k = 0; k < blen; ++k) {
            const unsigned char byte = idat[p + k];
            if (want_filter) {
                if (byte != 0) {                /* only "none" is understood */
                    free(out);
                    return 0;
                }
                want_filter = 0;
                ch = 0;
                continue;
            }
            uint32_t* px = &out[(unsigned long)row * w + col];
            if (ch == 0)      *px = (uint32_t)byte << 16;
            else if (ch == 1) *px |= (uint32_t)byte << 8;
            else              *px |= byte;
            if (++ch == 3) {
                ch = 0;
                if (++col == w) {
                    col = 0;
                    if (++row == h)
                        goto done;
                    want_filter = 1;
                }
            }
        }
        p += blen;
        if (final)
            break;
    }
done:
    if (row < h) {          /* truncated */
        free(out);
        return 0;
    }
    *width = w;
    *height = h;
    return out;
}
