/* imgview - shows a PNG.
 *
 * It reads the PNGs this system writes, which means the subset paint produces:
 * 8-bit truecolour, no interlacing, and a deflate stream of stored blocks. That
 * is a real constraint and it is stated plainly on screen when a file falls
 * outside it - a viewer that silently showed noise would be worse than one that
 * says it cannot read something.
 *
 * Decoding a general PNG needs an inflate implementation, which this system
 * does not have yet; see the README.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <widget.h>
#include <window.h>

#define MAX_FILE (2 * 1024 * 1024)
#define MAX_PIX  (1024 * 1024)

static uint32_t* g_px;
static unsigned  g_w = 480, g_h = 380;

static unsigned char g_file[MAX_FILE];
static uint32_t g_image[MAX_PIX];
static unsigned g_iw, g_ih;
static char g_note[128] = "";
static char g_path[256];
static int  g_ox, g_oy;         /* pan offset, for images larger than the window */

static uint32_t be32(const unsigned char* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/* Returns 0 on success. Deliberately strict: anything unexpected is reported
 * rather than guessed at. */
static int decode_png(unsigned long len)
{
    static const unsigned char sig[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
    if (len < 8 || memcmp(g_file, sig, 8) != 0) {
        snprintf(g_note, sizeof(g_note), "not a PNG");
        return -1;
    }
    unsigned long i = 8;
    unsigned char* idat = 0;
    unsigned long idat_len = 0;
    unsigned bit_depth = 0, colour = 0, interlace = 0;

    while (i + 12 <= len) {
        const uint32_t clen = be32(&g_file[i]);
        const unsigned char* type = &g_file[i + 4];
        unsigned char* data = &g_file[i + 8];
        if (i + 12 + clen > len)
            break;
        if (memcmp(type, "IHDR", 4) == 0) {
            g_iw = be32(data);
            g_ih = be32(data + 4);
            bit_depth = data[8];
            colour = data[9];
            interlace = data[12];
        } else if (memcmp(type, "IDAT", 4) == 0) {
            /* Contiguous in the files we write; a general reader would join
               them. */
            if (idat == 0) idat = data;
            idat_len += clen;
        }
        i += 12 + clen;
    }

    if (g_iw == 0 || g_ih == 0 || (unsigned long)g_iw * g_ih > MAX_PIX) {
        snprintf(g_note, sizeof(g_note), "size %ux%u is out of range", g_iw, g_ih);
        return -1;
    }
    if (bit_depth != 8 || colour != 2 || interlace != 0) {
        snprintf(g_note, sizeof(g_note),
                 "needs 8-bit truecolour, non-interlaced (got depth %u type %u)",
                 bit_depth, colour);
        return -1;
    }
    if (idat == 0 || idat_len < 2) {
        snprintf(g_note, sizeof(g_note), "no image data");
        return -1;
    }

    /* Walk the deflate stream. Only stored blocks are understood; a compressed
     * one needs an inflate this system does not have. */
    unsigned long p = 2;                        /* past the zlib header */
    unsigned long out = 0;
    const unsigned long row_bytes = 1ul + (unsigned long)g_iw * 3;
    const unsigned long want = row_bytes * g_ih;
    static unsigned char raw[MAX_PIX * 3 + 8192];

    for (;;) {
        if (p + 5 > idat_len)
            break;
        const unsigned char header = idat[p];
        const int final = header & 1;
        const int type = (header >> 1) & 3;
        if (type != 0) {
            snprintf(g_note, sizeof(g_note),
                     "compressed PNG: this viewer has no inflate");
            return -1;
        }
        const unsigned blen = (unsigned)idat[p + 1] | ((unsigned)idat[p + 2] << 8);
        p += 5;
        if (p + blen > idat_len || out + blen > sizeof(raw))
            break;
        memcpy(&raw[out], &idat[p], blen);
        out += blen;
        p += blen;
        if (final)
            break;
    }
    if (out < want) {
        snprintf(g_note, sizeof(g_note), "truncated: %lu of %lu bytes", out, want);
        return -1;
    }

    for (unsigned y = 0; y < g_ih; ++y) {
        const unsigned char* row = &raw[(unsigned long)y * row_bytes];
        if (row[0] != 0) {
            snprintf(g_note, sizeof(g_note),
                     "row %u uses filter %u; only 'none' is understood", y, row[0]);
            return -1;
        }
        for (unsigned x = 0; x < g_iw; ++x)
            g_image[(unsigned long)y * g_iw + x] =
                ((uint32_t)row[1 + x * 3] << 16) |
                ((uint32_t)row[2 + x * 3] << 8) | row[3 + x * 3];
    }
    snprintf(g_note, sizeof(g_note), "%ux%u", g_iw, g_ih);
    return 0;
}

static int load(const char* path)
{
    g_iw = g_ih = 0;
    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(g_note, sizeof(g_note), "cannot open %s", path);
        return -1;
    }
    const long n = (long)read(fd, g_file, MAX_FILE);
    close(fd);
    if (n <= 0) {
        snprintf(g_note, sizeof(g_note), "empty file");
        return -1;
    }
    return decode_png((unsigned long)n);
}

static void draw(void)
{
    wg_fill(0, 0, (int)g_w, (int)g_h, WG_FACE);
    const int top = 24;
    wg_text_clipped(8, 4, g_path, WG_INK, (int)g_w - 16);

    wg_fill(4, top, (int)g_w - 8, (int)g_h - top - 20, 0x404040);
    wg_bevel(4, top, (int)g_w - 8, (int)g_h - top - 20, 0);

    if (g_iw != 0) {
        const int vw = (int)g_w - 12, vh = (int)g_h - top - 28;
        for (int y = 0; y < vh; ++y) {
            const int sy = y + g_oy;
            if (sy < 0 || sy >= (int)g_ih)
                continue;
            for (int x = 0; x < vw; ++x) {
                const int sx = x + g_ox;
                if (sx < 0 || sx >= (int)g_iw)
                    continue;
                wg_plot(8 + x, top + 4 + y,
                        g_image[(unsigned long)sy * g_iw + sx]);
            }
        }
    }
    wg_fill(0, (int)g_h - 20, (int)g_w, 20, WG_FACE);
    wg_text_clipped(8, (int)g_h - 18, g_note, WG_DIM, (int)g_w - 16);
}

int main(int argc, char** argv)
{
    if (argc > 1) {
        int n = 0;
        while (argv[1][n] != '\0' && n < 255) { g_path[n] = argv[1][n]; ++n; }
        g_path[n] = '\0';
    } else {
        snprintf(g_path, sizeof(g_path), "/PAINT.PNG");
    }
    if (wg_font() != 0)
        return 1;
    const int id = win_create(200, 90, g_w, g_h, "Image");
    if (id < 0) {
        printf("imgview: no window server\n");
        return 1;
    }
    g_px = win_map(id);
    if (g_px == 0)
        return 1;
    win_set_min_size(id, 260, 200);
    wg_target(g_px, g_w, g_h);

    load(g_path);
    draw();
    win_present(id);

    int dragging = 0, last_x = 0, last_y = 0;
    for (;;) {
        struct win_event e;
        while (win_poll(id, &e)) {
            if (e.type == WIN_EVENT_CLOSE) { win_destroy(id); return 0; }
            if (e.type == WIN_EVENT_RESIZE) {
                g_w = (unsigned)e.x; g_h = (unsigned)e.y;
                g_px = win_map(id);
                if (g_px == 0) return 1;
                wg_target(g_px, g_w, g_h);
            } else if (e.type == WIN_EVENT_MOUSE_DOWN) {
                dragging = 1; last_x = e.x; last_y = e.y;
            } else if (e.type == WIN_EVENT_MOUSE_UP) {
                dragging = 0;
            } else if (e.type == WIN_EVENT_MOUSE_MOVE && dragging) {
                /* Drag to pan, which is the only navigation an image this
                 * simple needs. */
                g_ox -= e.x - last_x;
                g_oy -= e.y - last_y;
                last_x = e.x; last_y = e.y;
                if (g_ox < 0) g_ox = 0;
                if (g_oy < 0) g_oy = 0;
            } else if (e.type == WIN_EVENT_KEY && e.key == 'r') {
                load(g_path);
                g_ox = g_oy = 0;
            } else {
                continue;
            }
            draw();
            win_present(id);
        }
        msleep(15);
    }
}
