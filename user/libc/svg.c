/* Vector icons, filled by the same rasteriser the font uses.
 *
 * The glyphs in this system's chrome are Material Symbols, which are SVG files
 * of one shape: a viewBox, a fill colour nothing here reads, and a single
 * <path> whose `d` attribute is the whole drawing. So this is not an SVG
 * renderer - there is no styling, no grouping, no transform stack, no text.
 * It finds the paths, walks their commands, and hands the contours to
 * draw_edge_deposit.
 *
 * That narrowness is the point. A general SVG renderer is a large program with
 * a CSS engine somewhere inside it; what an icon needs is a path parser, and
 * the difference between the two is several thousand lines nobody here would
 * ever run.
 *
 * Icons are filled at whatever size is asked for rather than at a size baked
 * in at build time, which is the reason for keeping them vector at all: the
 * same close box is drawn at 10 pixels in a title bar and at 24 in a menu, and
 * both are sharp.
 */

#include <draw.h>
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <svg.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_EDGES 8192

struct builder {
    float* area;
    int    w, h, stride;

    /* Edges are deposited as they are produced rather than collected, so a
     * complicated icon costs no more memory than a simple one. */
    float  start_x, start_y;    /* where the current contour began */
    float  x, y;                /* where the pen is */
    float  scale, offset_x, offset_y;
    int    open;
};

static void emit(struct builder* b, float x0, float y0, float x1, float y1)
{
    draw_edge_deposit(b->area, b->w, b->h, b->stride,
                      (x0 - b->offset_x) * b->scale,
                      (y0 - b->offset_y) * b->scale,
                      (x1 - b->offset_x) * b->scale,
                      (y1 - b->offset_y) * b->scale);
}

static void line_to(struct builder* b, float x, float y)
{
    emit(b, b->x, b->y, x, y);
    b->x = x;
    b->y = y;
}

/* Curves are split into a fixed number of pieces chosen from how far the
 * control points stray - the same judgement the font makes, for the same
 * reason: spend segments where the curve actually bends. */
static void cubic_to(struct builder* b, float c1x, float c1y,
                     float c2x, float c2y, float x, float y)
{
    const float dx = c1x - b->x + c2x - x, dy = c1y - b->y + c2y - y;
    const float bend = (dx * dx + dy * dy) * b->scale * b->scale;
    int steps = (int)(sqrt(sqrt(bend)) * 2.0f) + 3;
    if (steps > 24) steps = 24;

    const float x0 = b->x, y0 = b->y;
    for (int i = 1; i <= steps; ++i) {
        const float t = (float)i / (float)steps, u = 1.0f - t;
        const float px = u*u*u*x0 + 3*u*u*t*c1x + 3*u*t*t*c2x + t*t*t*x;
        const float py = u*u*u*y0 + 3*u*u*t*c1y + 3*u*t*t*c2y + t*t*t*y;
        emit(b, b->x, b->y, px, py);
        b->x = px;
        b->y = py;
    }
}

static void quad_to(struct builder* b, float cx, float cy, float x, float y)
{
    /* A quadratic is a cubic whose control points are two thirds of the way to
     * the single one, which saves writing the flattening twice. */
    cubic_to(b,
             b->x + 2.0f / 3.0f * (cx - b->x), b->y + 2.0f / 3.0f * (cy - b->y),
             x + 2.0f / 3.0f * (cx - x),       y + 2.0f / 3.0f * (cy - y),
             x, y);
}

/* An elliptical arc, as SVG spells it: two radii, a rotation, two flags and an
 * endpoint. Converted to centre form and then to short line segments. The
 * arithmetic is the one in the SVG specification's implementation notes, and
 * it is here because circles in icon sets are drawn with `a` more often than
 * with curves. */
static void arc_to(struct builder* b, float rx, float ry, float rotation,
                   int large, int sweep, float x, float y)
{
    const float x0 = b->x, y0 = b->y;
    if (rx == 0.0f || ry == 0.0f) {
        line_to(b, x, y);
        return;
    }
    if (rx < 0.0f) rx = -rx;
    if (ry < 0.0f) ry = -ry;

    const float phi = rotation * 3.14159265358979f / 180.0f;
    const float cs = cos(phi), sn = sin(phi);
    const float dx2 = (x0 - x) * 0.5f, dy2 = (y0 - y) * 0.5f;
    const float x1 =  cs * dx2 + sn * dy2;
    const float y1 = -sn * dx2 + cs * dy2;

    /* Radii too small to reach the endpoint are scaled up, which the spec
     * requires and which real files rely on. */
    float lambda = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
    if (lambda > 1.0f) {
        const float k = sqrt(lambda);
        rx *= k;
        ry *= k;
    }

    float numerator = rx*rx*ry*ry - rx*rx*y1*y1 - ry*ry*x1*x1;
    const float denominator = rx*rx*y1*y1 + ry*ry*x1*x1;
    if (numerator < 0.0f) numerator = 0.0f;
    float factor = denominator > 0.0f ? sqrt(numerator / denominator) : 0.0f;
    if (large == sweep) factor = -factor;

    const float cx1 =  factor * rx * y1 / ry;
    const float cy1 = -factor * ry * x1 / rx;
    const float cx = cs * cx1 - sn * cy1 + (x0 + x) * 0.5f;
    const float cy = sn * cx1 + cs * cy1 + (y0 + y) * 0.5f;

    const float ux = (x1 - cx1) / rx, uy = (y1 - cy1) / ry;
    const float vx = (-x1 - cx1) / rx, vy = (-y1 - cy1) / ry;

    float start = atan2(uy, ux);
    float delta = atan2(ux * vy - uy * vx, ux * vx + uy * vy);
    if (!sweep && delta > 0.0f) delta -= 6.28318530717959f;
    if (sweep && delta < 0.0f)  delta += 6.28318530717959f;

    int steps = (int)((delta < 0.0f ? -delta : delta) * 8.0f) + 2;
    if (steps > 48) steps = 48;
    for (int i = 1; i <= steps; ++i) {
        const float t = start + delta * (float)i / (float)steps;
        const float ex = cx + rx * cos(t) * cs - ry * sin(t) * sn;
        const float ey = cy + rx * cos(t) * sn + ry * sin(t) * cs;
        emit(b, b->x, b->y, ex, ey);
        b->x = ex;
        b->y = ey;
    }
}

static void close_contour(struct builder* b)
{
    if (!b->open)
        return;
    /* Always closed, whether the file said so or not: a contour that does not
     * meet itself leaves a row whose crossings do not cancel, and the fill
     * floods sideways from it. */
    if (b->x != b->start_x || b->y != b->start_y)
        emit(b, b->x, b->y, b->start_x, b->start_y);
    b->x = b->start_x;
    b->y = b->start_y;
    b->open = 0;
}

/* --- reading the numbers --------------------------------------------------- */

static void skip_separators(const char** at)
{
    while (**at == ' ' || **at == ',' || **at == '\t' ||
           **at == '\n' || **at == '\r')
        ++*at;
}

static float number(const char** at)
{
    skip_separators(at);
    char* end = 0;
    const float v = (float)strtod(*at, &end);
    if (end != *at)
        *at = end;
    else
        ++*at;                          /* not a number; do not spin here */
    return v;
}

/* A flag in an arc is a single character, and is allowed to run into the
 * number after it without a separator - "a1 1 0 011 1" is four arguments, not
 * two. Reading it with strtod swallows the next argument as well. */
static int flag(const char** at)
{
    skip_separators(at);
    const int v = **at == '1';
    if (**at == '0' || **at == '1')
        ++*at;
    return v;
}

static void walk_path(struct builder* b, const char* d)
{
    char command = 0, previous = 0;
    float last_cx = 0.0f, last_cy = 0.0f;    /* for S and T */

    for (;;) {
        skip_separators(&d);
        if (*d == '\0')
            break;

        if ((*d >= 'A' && *d <= 'Z') || (*d >= 'a' && *d <= 'z')) {
            command = *d++;
        } else if (command == 'M') {
            command = 'L';              /* extra pairs after a moveto are */
        } else if (command == 'm') {    /* linetos, which is easy to miss */
            command = 'l';
        }

        const int relative = command >= 'a' && command <= 'z';
        const char kind = relative ? (char)(command - 'a' + 'A') : command;
        const float ox = relative ? b->x : 0.0f;
        const float oy = relative ? b->y : 0.0f;

        switch (kind) {
        case 'M': {
            close_contour(b);
            const float x = ox + number(&d), y = oy + number(&d);
            b->x = b->start_x = x;
            b->y = b->start_y = y;
            b->open = 1;
            break;
        }
        case 'L': {
            const float x = ox + number(&d), y = oy + number(&d);
            line_to(b, x, y);
            break;
        }
        case 'H': line_to(b, ox + number(&d), b->y); break;
        case 'V': line_to(b, b->x, oy + number(&d)); break;
        case 'C': {
            const float c1x = ox + number(&d), c1y = oy + number(&d);
            const float c2x = ox + number(&d), c2y = oy + number(&d);
            const float x = ox + number(&d), y = oy + number(&d);
            cubic_to(b, c1x, c1y, c2x, c2y, x, y);
            last_cx = c2x; last_cy = c2y;
            break;
        }
        case 'S': {
            /* The first control point is the reflection of the last one, but
             * only if the previous command was itself a cubic. */
            const int smooth = previous == 'C' || previous == 'c' ||
                               previous == 'S' || previous == 's';
            const float c1x = smooth ? 2.0f * b->x - last_cx : b->x;
            const float c1y = smooth ? 2.0f * b->y - last_cy : b->y;
            const float c2x = ox + number(&d), c2y = oy + number(&d);
            const float x = ox + number(&d), y = oy + number(&d);
            cubic_to(b, c1x, c1y, c2x, c2y, x, y);
            last_cx = c2x; last_cy = c2y;
            break;
        }
        case 'Q': {
            const float cx = ox + number(&d), cy = oy + number(&d);
            const float x = ox + number(&d), y = oy + number(&d);
            quad_to(b, cx, cy, x, y);
            last_cx = cx; last_cy = cy;
            break;
        }
        case 'T': {
            const int smooth = previous == 'Q' || previous == 'q' ||
                               previous == 'T' || previous == 't';
            const float cx = smooth ? 2.0f * b->x - last_cx : b->x;
            const float cy = smooth ? 2.0f * b->y - last_cy : b->y;
            const float x = ox + number(&d), y = oy + number(&d);
            quad_to(b, cx, cy, x, y);
            last_cx = cx; last_cy = cy;
            break;
        }
        case 'A': {
            const float rx = number(&d), ry = number(&d);
            const float rotation = number(&d);
            const int large = flag(&d), sweep = flag(&d);
            const float x = ox + number(&d), y = oy + number(&d);
            arc_to(b, rx, ry, rotation, large, sweep, x, y);
            break;
        }
        case 'Z':
            close_contour(b);
            break;
        default:
            return;                     /* something unknown; stop cleanly */
        }
        previous = command;
    }
    close_contour(b);
}

/* --- the file --------------------------------------------------------------- */

/* The value of an attribute, as a pointer into the document. Enough XML for a
 * file of one element with a handful of attributes, and no more. */
static const char* attribute(const char* text, const char* name, int* length)
{
    const unsigned long n = strlen(name);
    for (const char* at = text; (at = strstr(at, name)) != 0; at += n) {
        const char* p = at + n;
        /* A real attribute, not a substring of a longer one. */
        if (at != text && at[-1] != ' ' && at[-1] != '\t' && at[-1] != '\n')
            continue;
        while (*p == ' ') ++p;
        if (*p != '=')
            continue;
        ++p;
        while (*p == ' ') ++p;
        if (*p != '"' && *p != '\'')
            continue;
        const char quote = *p++;
        const char* end = strchr(p, quote);
        if (end == 0)
            continue;
        *length = (int)(end - p);
        return p;
    }
    return 0;
}

int svg_render(const char* path, int size, struct svg_icon* out)
{
    struct stat st;
    if (size <= 0 || size > 512 || stat(path, &st) != 0 || st.st_size <= 0 ||
        st.st_size > 256 * 1024)
        return -1;

    char* text = (char*)malloc((size_t)st.st_size + 1);
    if (text == 0)
        return -1;
    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        free(text);
        return -1;
    }
    long got = read(fd, text, (unsigned long)st.st_size);
    close(fd);
    if (got <= 0) {
        free(text);
        return -1;
    }
    text[got] = '\0';

    /* The viewBox is the icon's own coordinate space, and Material Symbols use
     * one whose origin is at the bottom - "0 -960 960 960" - so it cannot be
     * assumed to start at zero. */
    float vx = 0.0f, vy = 0.0f, vw = 0.0f, vh = 0.0f;
    int length = 0;
    const char* box = attribute(text, "viewBox", &length);
    if (box != 0) {
        const char* at = box;
        vx = number(&at); vy = number(&at);
        vw = number(&at); vh = number(&at);
    }
    if (vw <= 0.0f || vh <= 0.0f) {
        free(text);
        return -1;
    }

    const int w = size, h = size;
    const int stride = w + 2;
    float* area = (float*)malloc((size_t)stride * h * sizeof(float));
    unsigned char* coverage = (unsigned char*)malloc((size_t)w * h);
    if (area == 0 || coverage == 0) {
        free(area);
        free(coverage);
        free(text);
        return -1;
    }
    memset(area, 0, (size_t)stride * h * sizeof(float));

    struct builder b;
    memset(&b, 0, sizeof(b));
    b.area = area;
    b.w = w;
    b.h = h;
    b.stride = stride;
    /* Fitted to the shorter side so a non-square viewBox is letterboxed rather
     * than stretched. */
    b.scale = (vw > vh ? (float)size / vw : (float)size / vh);
    b.offset_x = vx;
    b.offset_y = vy;

    /* Every path in the file, filled into one bitmap. Material Symbols have
     * exactly one, but a two-path icon should not come out half drawn. */
    const char* at = text;
    int paths = 0;
    while ((at = strstr(at, "<path")) != 0) {
        int d_length = 0;
        const char* d = attribute(at, "d", &d_length);
        if (d == 0)
            break;
        char* copy = (char*)malloc((size_t)d_length + 1);
        if (copy == 0)
            break;
        memcpy(copy, d, (size_t)d_length);
        copy[d_length] = '\0';
        b.x = b.y = b.start_x = b.start_y = 0.0f;
        b.open = 0;
        walk_path(&b, copy);
        free(copy);
        ++paths;
        at += 5;
    }

    draw_area_resolve(area, w, h, stride, coverage);
    free(area);
    free(text);

    if (paths == 0) {
        free(coverage);
        return -1;
    }
    out->coverage = coverage;
    out->w = w;
    out->h = h;
    return 0;
}

void svg_free(struct svg_icon* icon)
{
    if (icon == 0)
        return;
    free(icon->coverage);
    icon->coverage = 0;
}

void svg_draw(const struct surface* s, const struct svg_icon* icon,
              int x, int y, uint32_t colour)
{
    if (icon == 0 || icon->coverage == 0)
        return;
    const unsigned alpha = (colour >> 24) & 0xFF;
    const uint32_t rgb = colour & 0x00FFFFFFu;

    for (int iy = 0; iy < icon->h; ++iy)
        for (int ix = 0; ix < icon->w; ++ix) {
            const unsigned c = icon->coverage[(long)iy * icon->w + ix];
            if (c == 0)
                continue;
            draw_pixel(s, x + ix, y + iy,
                       (((alpha * c + 127) / 255) << 24) | rgb);
        }
}
