/* TrueType outlines, filled with coverage antialiasing.
 *
 * Three pieces, in the order the work happens:
 *
 *   the tables      sfnt directory, then head/hhea/maxp/cmap/hmtx/loca/glyf.
 *                   Everything else in a font file describes typography this
 *                   does not do.
 *
 *   the outline     a glyph is closed contours of quadratic curves. Points are
 *                   on-curve or off-curve, and two off-curve points in a row
 *                   imply an on-curve point exactly between them - a
 *                   compression trick that every reader has to undo.
 *
 *   the fill        signed-area accumulation. Each edge deposits how much of
 *                   each pixel it sweeps past, signed by which way it crosses;
 *                   running along a row and summing gives coverage directly.
 *                   No supersampling, no per-pixel inside test, and the answer
 *                   is analytic rather than sampled - a diagonal stem comes
 *                   out evenly shaded instead of stepped.
 *
 * The fill is the interesting one. The obvious way to rasterise a filled shape
 * is to ask, for each pixel, whether its centre is inside - which is fast and
 * gives jagged edges, because a pixel is either in or out. Supersampling asks
 * the same question sixteen times per pixel and is sixteen times slower for an
 * answer that is still quantised. Accumulating area asks a different question:
 * not "is this pixel inside" but "how much of it did the outline sweep across",
 * which is the number wanted in the first place.
 */

#include <fcntl.h>
#include <draw.h>
#include <font.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_POINTS   1024       /* points in one glyph outline */
#define MAX_CONTOURS 64
#define MAX_EDGES    4096       /* line segments after flattening */
#define CACHE_SIZE   256        /* rasterised glyphs kept */
#define MAX_DEPTH    4          /* nested composite glyphs */

static char g_error[128] = "";

static void fail(const char* what)
{
    snprintf(g_error, sizeof(g_error), "%s", what);
}

const char* font_error(void) { return g_error[0] != '\0' ? g_error : "no error"; }

/* --- reading the file ------------------------------------------------------
 *
 * Big-endian, because sfnt was designed on a 68000 and never changed. Bounds
 * are checked on every read: a font is a file off a disk, and a malformed one
 * should be a font that does not open rather than a program that does not run.
 */

struct font {
    unsigned char* data;
    unsigned long  size;

    unsigned long  cmap, glyf, loca, hmtx;
    unsigned long  cmap_sub;        /* the subtable actually chosen */
    int            long_loca;
    int            units_per_em;
    int            ascent, descent, line_gap;
    int            num_glyphs, num_h_metrics;

    struct {
        int            used;
        unsigned       codepoint;
        int            px;
        unsigned char* coverage;
        int w, h, left, top, advance;
    } cache[CACHE_SIZE];
    int cache_next;
};

static unsigned rd8(const struct font* f, unsigned long at)
{
    return at < f->size ? f->data[at] : 0;
}

static unsigned rd16(const struct font* f, unsigned long at)
{
    return (rd8(f, at) << 8) | rd8(f, at + 1);
}

static int rd16s(const struct font* f, unsigned long at)
{
    const unsigned v = rd16(f, at);
    return v >= 0x8000 ? (int)v - 0x10000 : (int)v;
}

static unsigned long rd32(const struct font* f, unsigned long at)
{
    return ((unsigned long)rd16(f, at) << 16) | rd16(f, at + 2);
}

static unsigned long find_table(const struct font* f, const char* tag)
{
    const unsigned count = rd16(f, 4);
    for (unsigned i = 0; i < count; ++i) {
        const unsigned long at = 12 + 16 * (unsigned long)i;
        if (memcmp(f->data + at, tag, 4) == 0)
            return rd32(f, at + 8);
    }
    return 0;
}

/* Which cmap subtable to read.
 *
 * Preferring the Unicode ones and taking format 12 over format 4 where both
 * exist: format 4 stops at U+FFFF, which is every character this system is
 * going to show, but a font that offers both offers 12 as the better one and
 * there is no reason to take the worse. */
static unsigned long choose_cmap(struct font* f)
{
    const unsigned count = rd16(f, f->cmap + 2);
    unsigned long best = 0;
    int best_score = -1;

    for (unsigned i = 0; i < count; ++i) {
        const unsigned long rec = f->cmap + 4 + 8 * (unsigned long)i;
        const unsigned platform = rd16(f, rec);
        const unsigned encoding = rd16(f, rec + 2);
        const unsigned long sub = f->cmap + rd32(f, rec + 4);
        const unsigned format = rd16(f, sub);

        int score = -1;
        if (platform == 3 && encoding == 10 && format == 12) score = 4;
        else if (platform == 0 && format == 12)              score = 3;
        else if (platform == 3 && encoding == 1 && format == 4) score = 2;
        else if (platform == 0 && format == 4)               score = 1;
        else if (format == 4 || format == 12)                score = 0;

        if (score > best_score) {
            best_score = score;
            best = sub;
        }
    }
    return best;
}

static unsigned glyph_of(const struct font* f, unsigned codepoint)
{
    const unsigned long sub = f->cmap_sub;
    if (sub == 0)
        return 0;
    const unsigned format = rd16(f, sub);

    if (format == 4) {
        if (codepoint > 0xFFFF)
            return 0;
        const unsigned segs = rd16(f, sub + 6) / 2;
        const unsigned long ends   = sub + 14;
        const unsigned long starts = ends + 2 * segs + 2;
        const unsigned long deltas = starts + 2 * segs;
        const unsigned long ranges = deltas + 2 * segs;

        for (unsigned s = 0; s < segs; ++s) {
            if (codepoint > rd16(f, ends + 2 * s))
                continue;
            const unsigned start = rd16(f, starts + 2 * s);
            if (codepoint < start)
                return 0;               /* falls in a gap between segments */
            const unsigned offset = rd16(f, ranges + 2 * s);
            if (offset == 0)
                return (codepoint + rd16(f, deltas + 2 * s)) & 0xFFFF;
            /* The offset is from the slot it was read out of, which is the
             * one piece of this format that cannot be read literally. */
            const unsigned long at = ranges + 2 * s + offset
                                   + 2 * (codepoint - start);
            const unsigned g = rd16(f, at);
            return g == 0 ? 0 : (g + rd16(f, deltas + 2 * s)) & 0xFFFF;
        }
        return 0;
    }

    if (format == 12) {
        const unsigned long groups = rd32(f, sub + 12);
        for (unsigned long g = 0; g < groups; ++g) {
            const unsigned long at = sub + 16 + 12 * g;
            const unsigned long first = rd32(f, at);
            const unsigned long last  = rd32(f, at + 4);
            if (codepoint < first)
                return 0;               /* the groups are sorted */
            if (codepoint <= last)
                return (unsigned)(rd32(f, at + 8) + (codepoint - first));
        }
    }
    return 0;
}

static void glyph_range(const struct font* f, unsigned glyph,
                        unsigned long* from, unsigned long* to)
{
    if ((int)glyph >= f->num_glyphs) {
        *from = *to = 0;
        return;
    }
    if (f->long_loca) {
        *from = rd32(f, f->loca + 4 * glyph);
        *to   = rd32(f, f->loca + 4 * glyph + 4);
    } else {
        *from = 2 * (unsigned long)rd16(f, f->loca + 2 * glyph);
        *to   = 2 * (unsigned long)rd16(f, f->loca + 2 * glyph + 2);
    }
}

static int advance_of(const struct font* f, unsigned glyph)
{
    /* The last entry in hmtx repeats for every glyph after it, which is how a
     * font with a run of equal-width glyphs at the end stays small. */
    const unsigned n = (unsigned)f->num_h_metrics;
    if (n == 0)
        return 0;
    const unsigned which = glyph < n ? glyph : n - 1;
    return (int)rd16(f, f->hmtx + 4 * which);
}

/* --- outlines --------------------------------------------------------------
 *
 * Points come out in font units, which are unitsPerEm to the em - 2000 for
 * this font, 1000 or 2048 for others. Scaling happens once, on the way into
 * the rasteriser.
 */

struct outline {
    float x[MAX_POINTS];
    float y[MAX_POINTS];
    unsigned char on[MAX_POINTS];
    int  contour_end[MAX_CONTOURS];
    int  points, contours;
};

static int read_simple(const struct font* f, unsigned long at, int contours,
                       struct outline* out, float ox, float oy,
                       float sx, float sy)
{
    if (contours > MAX_CONTOURS - out->contours)
        return -1;

    const unsigned long ends = at + 10;
    const int first = out->points;
    const int total = rd16(f, ends + 2 * (unsigned long)(contours - 1)) + 1;
    if (total <= 0 || total > MAX_POINTS - out->points)
        return -1;

    /* Past the contour ends and the hinting bytecode, which is skipped whole:
     * the instructions nudge points onto the pixel grid, and nothing here runs
     * them. */
    const unsigned long instructions = ends + 2 * (unsigned long)contours;
    unsigned long p = instructions + 2 + rd16(f, instructions);

    /* Flags, which repeat: a set bit 3 means the next byte is a count of how
     * many more points share this flag. */
    unsigned char flags[MAX_POINTS];
    for (int i = 0; i < total; ) {
        const unsigned char flag = (unsigned char)rd8(f, p++);
        flags[i++] = flag;
        if ((flag & 8) != 0) {
            int repeat = (int)rd8(f, p++);
            while (repeat-- > 0 && i < total)
                flags[i++] = flag;
        }
    }

    /* Coordinates are deltas, and each axis is stored whole before the next.
     * Bit 1 says this delta is one byte, and then bit 4 says it is positive;
     * with bit 1 clear, bit 4 instead means "same as the last one". */
    int value = 0;
    for (int i = 0; i < total; ++i) {
        const unsigned char flag = flags[i];
        if ((flag & 2) != 0) {
            const int delta = (int)rd8(f, p++);
            value += (flag & 16) != 0 ? delta : -delta;
        } else if ((flag & 16) == 0) {
            value += rd16s(f, p);
            p += 2;
        }
        out->x[first + i] = ox + (float)value * sx;
    }
    value = 0;
    for (int i = 0; i < total; ++i) {
        const unsigned char flag = flags[i];
        if ((flag & 4) != 0) {
            const int delta = (int)rd8(f, p++);
            value += (flag & 32) != 0 ? delta : -delta;
        } else if ((flag & 32) == 0) {
            value += rd16s(f, p);
            p += 2;
        }
        out->y[first + i] = oy + (float)value * sy;
        out->on[first + i] = (unsigned char)(flags[i] & 1);
    }

    for (int c = 0; c < contours; ++c)
        out->contour_end[out->contours + c] =
            first + rd16(f, ends + 2 * (unsigned long)c);
    out->contours += contours;
    out->points = first + total;
    return 0;
}

static int read_outline(const struct font* f, unsigned glyph,
                        struct outline* out, float ox, float oy,
                        float sx, float sy, int depth);

static int read_composite(const struct font* f, unsigned long at,
                          struct outline* out, float ox, float oy,
                          float sx, float sy, int depth)
{
    unsigned long p = at + 10;
    for (;;) {
        const unsigned flags = rd16(f, p);
        const unsigned index = rd16(f, p + 2);
        p += 4;

        int dx, dy;
        if ((flags & 1) != 0) {         /* arguments are words */
            dx = rd16s(f, p);
            dy = rd16s(f, p + 2);
            p += 4;
        } else {
            const int a = (int)rd8(f, p), b = (int)rd8(f, p + 1);
            dx = a >= 128 ? a - 256 : a;
            dy = b >= 128 ? b - 256 : b;
            p += 2;
        }
        /* Only the xy form is honoured. The other reading - "line up point m
         * of this component with point n of the last" - needs the points of a
         * component that has not been placed yet, and no font this draws from
         * uses it. */
        if ((flags & 2) == 0)
            dx = dy = 0;

        float cx = 1.0f, cy = 1.0f;
        if ((flags & 8) != 0) {                 /* one scale for both axes */
            cx = cy = (float)rd16s(f, p) / 16384.0f;
            p += 2;
        } else if ((flags & 0x40) != 0) {       /* a scale each */
            cx = (float)rd16s(f, p) / 16384.0f;
            cy = (float)rd16s(f, p + 2) / 16384.0f;
            p += 4;
        } else if ((flags & 0x80) != 0) {       /* a full 2x2, of which the */
            cx = (float)rd16s(f, p) / 16384.0f;         /* diagonal is used */
            cy = (float)rd16s(f, p + 6) / 16384.0f;
            p += 8;
        }

        if (read_outline(f, index, out,
                         ox + (float)dx * sx, oy + (float)dy * sy,
                         sx * cx, sy * cy, depth + 1) != 0)
            return -1;

        if ((flags & 0x20) == 0)                /* no more components */
            break;
    }
    return 0;
}

static int read_outline(const struct font* f, unsigned glyph,
                        struct outline* out, float ox, float oy,
                        float sx, float sy, int depth)
{
    if (depth > MAX_DEPTH)
        return -1;

    unsigned long from, to;
    glyph_range(f, glyph, &from, &to);
    if (to <= from)
        return 0;                       /* an empty glyph, such as a space */

    const unsigned long at = f->glyf + from;
    const int contours = rd16s(f, at);
    if (contours >= 0)
        return read_simple(f, at, contours, out, ox, oy, sx, sy);
    return read_composite(f, at, out, ox, oy, sx, sy, depth);
}

/* --- flattening ------------------------------------------------------------
 *
 * Curves become short straight lines. How short is a judgement: too many and
 * the fill crawls, too few and a large O has visible flats on it. Splitting by
 * how far the control point strays from the chord spends segments where the
 * curve actually bends, which is the whole of the difference at a big size and
 * costs nothing at a small one.
 */

struct edges {
    float x0[MAX_EDGES], y0[MAX_EDGES], x1[MAX_EDGES], y1[MAX_EDGES];
    int count;
};

static void add_edge(struct edges* e, float x0, float y0, float x1, float y1)
{
    if (e->count >= MAX_EDGES || y0 == y1)
        return;                         /* a horizontal edge contributes none */
    e->x0[e->count] = x0; e->y0[e->count] = y0;
    e->x1[e->count] = x1; e->y1[e->count] = y1;
    ++e->count;
}

static void add_quadratic(struct edges* e, float x0, float y0,
                          float cx, float cy, float x1, float y1)
{
    const float dx = (x0 + x1) * 0.5f - cx;
    const float dy = (y0 + y1) * 0.5f - cy;
    const float bend = dx * dx + dy * dy;

    int steps = 2;
    if (bend > 0.05f) {
        steps = (int)(sqrt(sqrt(bend)) * 3.0f) + 2;
        if (steps > 24)
            steps = 24;
    }

    float px = x0, py = y0;
    for (int i = 1; i <= steps; ++i) {
        const float t = (float)i / (float)steps;
        const float u = 1.0f - t;
        const float qx = u * u * x0 + 2.0f * u * t * cx + t * t * x1;
        const float qy = u * u * y0 + 2.0f * u * t * cy + t * t * y1;
        add_edge(e, px, py, qx, qy);
        px = qx;
        py = qy;
    }
}

/* One contour's points into edges, undoing the implied-midpoint compression:
 * two off-curve points in a row have an on-curve point exactly between them
 * that the file does not store. */
static void flatten(const struct outline* o, int first, int last,
                    struct edges* e)
{
    const int n = last - first + 1;
    if (n < 2)
        return;

    /* Somewhere on the curve to start from. A contour may open on an
     * off-curve point, in which case the implied midpoint before it is the
     * start - and if every point is off-curve, any midpoint will do. */
    float sx, sy;
    int begin;
    if (o->on[first]) {
        sx = o->x[first];
        sy = o->y[first];
        begin = 1;
    } else if (o->on[last]) {
        sx = o->x[last];
        sy = o->y[last];
        begin = 0;
    } else {
        sx = (o->x[first] + o->x[last]) * 0.5f;
        sy = (o->y[first] + o->y[last]) * 0.5f;
        begin = 0;
    }

    float px = sx, py = sy;
    float cx = 0.0f, cy = 0.0f;
    int have_control = 0;

    for (int k = 0; k < n; ++k) {
        const int i = first + (begin + k) % n;
        const float x = o->x[i], y = o->y[i];

        if (o->on[i]) {
            if (have_control) {
                add_quadratic(e, px, py, cx, cy, x, y);
                have_control = 0;
            } else {
                add_edge(e, px, py, x, y);
            }
            px = x;
            py = y;
        } else if (have_control) {
            const float mx = (cx + x) * 0.5f, my = (cy + y) * 0.5f;
            add_quadratic(e, px, py, cx, cy, mx, my);
            px = mx;
            py = my;
            cx = x;
            cy = y;
        } else {
            cx = x;
            cy = y;
            have_control = 1;
        }
    }

    /* Closed, always: a contour that does not meet itself leaves the fill with
     * a row whose crossings do not cancel, and the whole scanline floods. */
    if (have_control)
        add_quadratic(e, px, py, cx, cy, sx, sy);
    else
        add_edge(e, px, py, sx, sy);
}

/* --- the fill --------------------------------------------------------------
 *
 * Every edge deposits, into the cells it passes, how much of each it swept and
 * which way it was going. Summing along a row then gives coverage: inside the
 * shape the deposits from the left edge have accumulated and the right edge
 * has not yet cancelled them, and between two edges of the same contour they
 * cancel exactly.
 *
 * The area for one scanline is worked out in closed form rather than sampled,
 * which is why a near-horizontal edge comes out smooth instead of staircased.
 */

/* The fill lives in draw.c, shared with the SVG loader: both turn closed
 * contours into coverage, and the only difference between a letter and an
 * icon is where the outline came from. */

/* --- the cache -------------------------------------------------------------
 *
 * Rasterising is the expensive part and text repeats itself constantly - the
 * same letters, at the same two or three sizes, several times a second. The
 * cache is a plain ring: when it is full the oldest slot is reused, which for
 * this access pattern is as good as anything cleverer.
 */

static void cache_drop(struct font* f, int slot)
{
    if (f->cache[slot].used) {
        free(f->cache[slot].coverage);
        f->cache[slot].coverage = 0;
        f->cache[slot].used = 0;
    }
}

static int cache_find(struct font* f, unsigned codepoint, int px)
{
    for (int i = 0; i < CACHE_SIZE; ++i)
        if (f->cache[i].used && f->cache[i].codepoint == codepoint &&
            f->cache[i].px == px)
            return i;
    return -1;
}

/* --- what the outside sees -------------------------------------------------- */

struct font* font_open(const char* path)
{
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size < 12) {
        fail("no such font file");
        return 0;
    }

    struct font* f = (struct font*)malloc(sizeof(struct font));
    if (f == 0) {
        fail("out of memory");
        return 0;
    }
    memset(f, 0, sizeof(*f));
    f->size = (unsigned long)st.st_size;
    f->data = (unsigned char*)malloc(f->size);
    if (f->data == 0) {
        free(f);
        fail("out of memory");
        return 0;
    }

    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        free(f->data);
        free(f);
        fail("cannot open the font");
        return 0;
    }
    unsigned long got = 0;
    while (got < f->size) {
        const long n = read(fd, f->data + got, f->size - got);
        if (n <= 0)
            break;
        got += (unsigned long)n;
    }
    close(fd);
    if (got != f->size) {
        free(f->data);
        free(f);
        fail("the font file ended early");
        return 0;
    }

    const unsigned long version = rd32(f, 0);
    if (version != 0x00010000ul && version != 0x74727565ul) {
        free(f->data);
        free(f);
        fail(version == 0x4F54544Ful
                 ? "this is a CFF font, and only glyf outlines are drawn here"
                 : "not a TrueType file");
        return 0;
    }

    const unsigned long head = find_table(f, "head");
    const unsigned long hhea = find_table(f, "hhea");
    const unsigned long maxp = find_table(f, "maxp");
    f->cmap = find_table(f, "cmap");
    f->glyf = find_table(f, "glyf");
    f->loca = find_table(f, "loca");
    f->hmtx = find_table(f, "hmtx");

    if (head == 0 || hhea == 0 || maxp == 0 || f->cmap == 0 ||
        f->glyf == 0 || f->loca == 0 || f->hmtx == 0) {
        free(f->data);
        free(f);
        fail("the font is missing a table needed to draw from it");
        return 0;
    }

    f->units_per_em  = (int)rd16(f, head + 18);
    f->long_loca     = rd16s(f, head + 50) != 0;
    f->ascent        = rd16s(f, hhea + 4);
    f->descent       = rd16s(f, hhea + 6);
    f->line_gap      = rd16s(f, hhea + 8);
    f->num_h_metrics = (int)rd16(f, hhea + 34);
    f->num_glyphs    = (int)rd16(f, maxp + 4);
    f->cmap_sub      = choose_cmap(f);

    if (f->units_per_em <= 0 || f->cmap_sub == 0) {
        free(f->data);
        free(f);
        fail("the font has no usable character map");
        return 0;
    }
    return f;
}

void font_close(struct font* f)
{
    if (f == 0)
        return;
    for (int i = 0; i < CACHE_SIZE; ++i)
        cache_drop(f, i);
    free(f->data);
    free(f);
}

int font_ascent(struct font* f, int px)
{
    return (f->ascent * px + f->units_per_em / 2) / f->units_per_em;
}

int font_descent(struct font* f, int px)
{
    /* Positive, as the number of pixels below the baseline: hhea stores it
     * negative and every caller wants a height. */
    const int d = -f->descent;
    return (d * px + f->units_per_em / 2) / f->units_per_em;
}

int font_line_height(struct font* f, int px)
{
    const int total = f->ascent - f->descent + f->line_gap;
    return (total * px + f->units_per_em / 2) / f->units_per_em;
}

int font_glyph(struct font* f, int px, unsigned codepoint, struct glyph* out)
{
    if (f == 0 || px <= 0 || px > 512)
        return -1;

    const int hit = cache_find(f, codepoint, px);
    if (hit >= 0) {
        out->coverage = f->cache[hit].coverage;
        out->w        = f->cache[hit].w;
        out->h        = f->cache[hit].h;
        out->left     = f->cache[hit].left;
        out->top      = f->cache[hit].top;
        out->advance  = f->cache[hit].advance;
        return 0;
    }

    const unsigned glyph = glyph_of(f, codepoint);
    const float scale = (float)px / (float)f->units_per_em;

    static struct outline outline;      /* static: far too big for a stack */
    static struct edges edges;
    outline.points = 0;
    outline.contours = 0;
    edges.count = 0;

    if (read_outline(f, glyph, &outline, 0.0f, 0.0f, scale, scale, 0) != 0)
        outline.points = 0;             /* too complicated; draw it blank */

    /* y is flipped here and nowhere else: a font counts upwards from the
     * baseline and a screen counts downwards from the top. */
    for (int i = 0; i < outline.points; ++i)
        outline.y[i] = -outline.y[i];

    int start = 0;
    for (int c = 0; c < outline.contours; ++c) {
        flatten(&outline, start, outline.contour_end[c], &edges);
        start = outline.contour_end[c] + 1;
    }

    float min_x = 0.0f, min_y = 0.0f, max_x = 0.0f, max_y = 0.0f;
    if (edges.count > 0) {
        min_x = max_x = edges.x0[0];
        min_y = max_y = edges.y0[0];
        for (int i = 0; i < edges.count; ++i) {
            const float xs[2] = { edges.x0[i], edges.x1[i] };
            const float ys[2] = { edges.y0[i], edges.y1[i] };
            for (int k = 0; k < 2; ++k) {
                if (xs[k] < min_x) min_x = xs[k];
                if (xs[k] > max_x) max_x = xs[k];
                if (ys[k] < min_y) min_y = ys[k];
                if (ys[k] > max_y) max_y = ys[k];
            }
        }
    }

    const int left = (int)floor(min_x) - 1;
    const int top  = (int)floor(min_y) - 1;
    int w = (int)ceil(max_x) - left + 2;
    int h = (int)ceil(max_y) - top + 2;
    if (edges.count == 0 || w <= 0 || h <= 0 || w > 1024 || h > 1024) {
        w = h = 0;
    }

    unsigned char* coverage = 0;
    if (w > 0 && h > 0) {
        const int stride = w + 2;
        float* area = (float*)malloc((size_t)stride * (size_t)h * sizeof(float));
        coverage = (unsigned char*)malloc((size_t)w * (size_t)h);
        if (area == 0 || coverage == 0) {
            free(area);
            free(coverage);
            return -1;
        }
        memset(area, 0, (size_t)stride * (size_t)h * sizeof(float));

        for (int i = 0; i < edges.count; ++i)
            draw_edge_deposit(area, w, h, stride,
                    edges.x0[i] - (float)left, edges.y0[i] - (float)top,
                    edges.x1[i] - (float)left, edges.y1[i] - (float)top);

        draw_area_resolve(area, w, h, stride, coverage);
        free(area);
    }

    const int advance = (int)((float)advance_of(f, glyph) * scale + 0.5f);

    const int slot = f->cache_next;
    f->cache_next = (f->cache_next + 1) % CACHE_SIZE;
    cache_drop(f, slot);
    f->cache[slot].used      = 1;
    f->cache[slot].codepoint = codepoint;
    f->cache[slot].px        = px;
    f->cache[slot].coverage  = coverage;
    f->cache[slot].w         = w;
    f->cache[slot].h         = h;
    f->cache[slot].left      = left;
    f->cache[slot].top       = -top;    /* above the baseline, as a height */
    f->cache[slot].advance   = advance;

    out->coverage = coverage;
    out->w        = w;
    out->h        = h;
    out->left     = left;
    out->top      = -top;
    out->advance  = advance;
    return 0;
}

unsigned utf8_next(const char** at)
{
    const unsigned char* p = (const unsigned char*)*at;
    if (*p == 0)
        return 0;

    unsigned c = *p++;
    int extra = 0;
    if      (c < 0x80) { extra = 0; }
    else if ((c & 0xE0) == 0xC0) { c &= 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { c &= 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { c &= 0x07; extra = 3; }
    else { *at = (const char*)p; return 0xFFFD; }

    for (int i = 0; i < extra; ++i) {
        if ((*p & 0xC0) != 0x80) {      /* truncated: report and carry on */
            *at = (const char*)p;
            return 0xFFFD;
        }
        c = (c << 6) | (unsigned)(*p++ & 0x3F);
    }
    *at = (const char*)p;
    return c;
}

int font_width(struct font* f, int px, const char* text)
{
    int total = 0;
    const char* at = text;
    for (;;) {
        const unsigned c = utf8_next(&at);
        if (c == 0)
            break;
        struct glyph g;
        if (font_glyph(f, px, c, &g) == 0)
            total += g.advance;
    }
    return total;
}
