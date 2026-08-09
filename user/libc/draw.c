/* Blending, rounded rectangles, blur and shadows.
 *
 * The four operations the new chrome is made of. None of them existed while
 * the interface was bevels and one-pixel lines, and all four are the ordinary
 * way of doing this - which is worth saying, because each has an obvious
 * cheaper version that looks wrong in a way that is hard to name afterwards.
 */

#include <draw.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* --- blending -------------------------------------------------------------- */

uint32_t draw_over(uint32_t under, uint32_t over)
{
    const unsigned a = (over >> 24) & 0xFF;
    if (a == 0)
        return under;
    if (a == 255)
        return over | 0xFF000000u;

    const unsigned inverse = 255 - a;
    /* Divided by 255 without dividing.
     *
     * Rounding matters - truncating the remainder on every channel of every
     * pixel is a visible darkening once anything is blended twice, and this is
     * the compositor - but an integer divide per channel per pixel is three
     * divides on every pixel of every window. `(v + (v >> 8) + 128) >> 8` is
     * the standard identity for it: exact for every value a byte pair can
     * produce, and three shifts instead of a divide. */
    #define OVER255(v) (((v) + ((v) >> 8) + 128) >> 8)
    const unsigned r = OVER255(((over >> 16) & 0xFF) * a +
                               ((under >> 16) & 0xFF) * inverse);
    const unsigned g = OVER255(((over >> 8) & 0xFF) * a +
                               ((under >> 8) & 0xFF) * inverse);
    const unsigned b = OVER255((over & 0xFF) * a + (under & 0xFF) * inverse);
    #undef OVER255
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

void draw_pixel(const struct surface* s, int x, int y, uint32_t colour)
{
    if (x < 0 || y < 0 || x >= s->w || y >= s->h)
        return;
    uint32_t* p = &s->pixels[(long)y * s->w + x];
    *p = draw_over(*p, colour);
}

void draw_rect(const struct surface* s, int x, int y, int w, int h,
               uint32_t colour)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w, y1 = y + h;
    if (x1 > s->w) x1 = s->w;
    if (y1 > s->h) y1 = s->h;
    if (s->cw > 0 && s->ch > 0) {
        if (x0 < s->cx) x0 = s->cx;
        if (y0 < s->cy) y0 = s->cy;
        if (x1 > s->cx + s->cw) x1 = s->cx + s->cw;
        if (y1 > s->cy + s->ch) y1 = s->cy + s->ch;
    }

    const unsigned a = (colour >> 24) & 0xFF;
    if (a == 0)
        return;

    for (int py = y0; py < y1; ++py) {
        uint32_t* row = &s->pixels[(long)py * s->w];
        if (a == 255) {
            for (int px = x0; px < x1; ++px)
                row[px] = colour | 0xFF000000u;
        } else {
            for (int px = x0; px < x1; ++px)
                row[px] = draw_over(row[px], colour);
        }
    }
}

/* --- rounded rectangles ----------------------------------------------------
 *
 * Coverage from the signed distance to the shape, rather than from a corner
 * stamp. The distance to a rounded rectangle has a closed form: push the point
 * into the box's inner rectangle - the one inset by the radius - and the
 * distance to the rounded shape is the distance to that rectangle minus the
 * radius. Everything follows from those two lines.
 *
 * Sampling once at the pixel centre and taking one pixel of falloff is not
 * true area coverage, but for a shape whose edge curvature is far larger than
 * a pixel - which a corner radius always is - the two agree to within a
 * rounding step, and this costs one square root instead of a scanline fill.
 */

static float round_distance(float px, float py, float x, float y,
                            float w, float h, float radius)
{
    const float half_w = w * 0.5f, half_h = h * 0.5f;
    const float cx = x + half_w, cy = y + half_h;

    /* Distance from the centre, folded into one quadrant, then measured
     * against the inner rectangle. */
    float dx = (px - cx < 0.0f ? cx - px : px - cx) - (half_w - radius);
    float dy = (py - cy < 0.0f ? cy - py : py - cy) - (half_h - radius);
    if (dx < 0.0f) dx = 0.0f;
    if (dy < 0.0f) dy = 0.0f;

    const float outside = sqrt(dx * dx + dy * dy) - radius;
    if (dx > 0.0f || dy > 0.0f)
        return outside;

    /* Inside the inner rectangle: the nearest edge, as a negative number. */
    float ex = (half_w - radius) - (px - cx < 0.0f ? cx - px : px - cx);
    float ey = (half_h - radius) - (py - cy < 0.0f ? cy - py : py - cy);
    const float nearest = ex < ey ? ex : ey;
    return -(nearest + radius);
}

int draw_round_coverage(int px, int py, int x, int y, int w, int h, int radius)
{
    const int limit = (w < h ? w : h) / 2;
    if (radius > limit) radius = limit;
    if (radius < 0) radius = 0;

    /* Only the corners are curved, and only a pixel either side of the edge is
     * partial. Everything else is a rectangle test, which answers without the
     * square root the distance needs - and almost every pixel of almost every
     * shape is everything else. */
    if (px < x - 1 || py < y - 1 || px > x + w || py > y + h)
        return 0;
    if (px >= x + radius && px < x + w - radius &&
        py >= y + 1 && py < y + h - 1)
        return 255;
    if (py >= y + radius && py < y + h - radius &&
        px >= x + 1 && px < x + w - 1)
        return 255;

    const float d = round_distance((float)px + 0.5f, (float)py + 0.5f,
                                   (float)x, (float)y, (float)w, (float)h,
                                   (float)radius);
    /* One pixel of falloff, centred on the boundary. */
    float coverage = 0.5f - d;
    if (coverage <= 0.0f) return 0;
    if (coverage >= 1.0f) return 255;
    return (int)(coverage * 255.0f + 0.5f);
}

void draw_round_rect(const struct surface* s, int x, int y, int w, int h,
                     int radius, uint32_t colour)
{
    if (w <= 0 || h <= 0)
        return;
    const int limit = (w < h ? w : h) / 2;
    if (radius > limit) radius = limit;
    if (radius < 0) radius = 0;

    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w, y1 = y + h;
    if (x1 > s->w) x1 = s->w;
    if (y1 > s->h) y1 = s->h;
    if (s->cw > 0 && s->ch > 0) {
        if (x0 < s->cx) x0 = s->cx;
        if (y0 < s->cy) y0 = s->cy;
        if (x1 > s->cx + s->cw) x1 = s->cx + s->cw;
        if (y1 > s->cy + s->ch) y1 = s->cy + s->ch;
    }

    const unsigned alpha = (colour >> 24) & 0xFF;
    const uint32_t rgb = colour & 0x00FFFFFFu;

    for (int py = y0; py < y1; ++py) {
        uint32_t* row = &s->pixels[(long)py * s->w];
        /* The middle band has no curvature, so the whole row between the
         * corners is a straight fill and needs no distance at all. */
        const int in_band = py >= y + radius && py < y + h - radius;
        /* And an opaque fill of that band is a store rather than a blend.
         * This is the whole of a window's panel when the glass is off, which
         * makes it the most-executed loop in the compositor. */
        if (in_band && alpha == 255) {
            for (int px = x0; px < x1; ++px)
                row[px] = 0xFF000000u | rgb;
            continue;
        }
        for (int px = x0; px < x1; ++px) {
            unsigned a;
            if (in_band) {
                a = alpha;
            } else {
                const int c = draw_round_coverage(px, py, x, y, w, h, radius);
                if (c == 0)
                    continue;
                a = (unsigned)(alpha * c + 127) / 255;
            }
            if (a == 0)
                continue;
            row[px] = draw_over(row[px], (a << 24) | rgb);
        }
    }
}

void draw_round_rect_outline(const struct surface* s, int x, int y,
                             int w, int h, int radius, int thickness,
                             uint32_t colour)
{
    if (w <= 0 || h <= 0 || thickness <= 0)
        return;
    const int limit = (w < h ? w : h) / 2;
    if (radius > limit) radius = limit;

    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w, y1 = y + h;
    if (x1 > s->w) x1 = s->w;
    if (y1 > s->h) y1 = s->h;
    if (s->cw > 0 && s->ch > 0) {
        if (x0 < s->cx) x0 = s->cx;
        if (y0 < s->cy) y0 = s->cy;
        if (x1 > s->cx + s->cw) x1 = s->cx + s->cw;
        if (y1 > s->cy + s->ch) y1 = s->cy + s->ch;
    }

    const unsigned alpha = (colour >> 24) & 0xFF;
    const uint32_t rgb = colour & 0x00FFFFFFu;
    const int inner_radius = radius - thickness > 0 ? radius - thickness : 0;

    for (int py = y0; py < y1; ++py) {
        uint32_t* row = &s->pixels[(long)py * s->w];

        /* Rows down the straight sides only touch the ring at their two ends.
         * Walking the whole width of them was scanning the entire interior of
         * every panel to draw a one pixel border round it - the single most
         * expensive thing the old chrome did per frame, for pixels that were
         * never going to change. */
        const int straight = py >= y + radius + thickness &&
                             py < y + h - radius - thickness;
        for (int px = x0; px < x1; ++px) {
            if (straight && px >= x + thickness + 1 &&
                px < x + w - thickness - 1) {
                px = x + w - thickness - 2;     /* skip to the far edge */
                continue;
            }
            /* Inside the outer shape and outside the inner one: the ring
             * between them, with both edges antialiased. */
            const int outer = draw_round_coverage(px, py, x, y, w, h, radius);
            if (outer == 0)
                continue;
            const int inner = draw_round_coverage(
                px, py, x + thickness, y + thickness,
                w - 2 * thickness, h - 2 * thickness, inner_radius);
            const int ring = outer - inner;
            if (ring <= 0)
                continue;
            const unsigned a = (unsigned)(alpha * ring + 127) / 255;
            if (a == 0)
                continue;
            row[px] = draw_over(row[px], (a << 24) | rgb);
        }
    }
}

/* --- blur -------------------------------------------------------------------
 *
 * Shrink, blur three times, stretch back.
 *
 * A single box blur is a poor gaussian - it has a hard edge in its response
 * and shows as banding on a gradient. Two is better and three is
 * indistinguishable, which is a result old enough to be folklore and true
 * enough to rely on.
 *
 * Each pass slides a running sum: add the pixel entering the window, subtract
 * the one leaving. That makes the cost independent of the radius, so a wide
 * blur costs the same as a narrow one and the radius can be chosen for how it
 * looks rather than for what it costs.
 */

#define BLUR_SHRINK 8

static void box_pass(unsigned* src, unsigned* dst, int w, int h, int radius,
                     int horizontal)
{
    const int length = horizontal ? w : h;
    const int lines  = horizontal ? h : w;
    const int step   = horizontal ? 1 : w;
    const int window = radius * 2 + 1;

    for (int line = 0; line < lines; ++line) {
        unsigned* in  = src + (horizontal ? (long)line * w : line);
        unsigned* out = dst + (horizontal ? (long)line * w : line);

        /* The window starts clamped against the near edge, which is what
         * stops a blurred region darkening towards its own border. */
        unsigned sum_r = 0, sum_g = 0, sum_b = 0;
        for (int k = -radius; k <= radius; ++k) {
            const int at = k < 0 ? 0 : (k >= length ? length - 1 : k);
            const unsigned p = in[(long)at * step];
            sum_r += (p >> 16) & 0xFF;
            sum_g += (p >> 8) & 0xFF;
            sum_b += p & 0xFF;
        }

        for (int i = 0; i < length; ++i) {
            out[(long)i * step] = 0xFF000000u
                                | ((sum_r / window) << 16)
                                | ((sum_g / window) << 8)
                                | (sum_b / window);

            const int leaving  = i - radius;
            const int entering = i + radius + 1;
            const int a = leaving < 0 ? 0 : (leaving >= length ? length - 1 : leaving);
            const int b = entering < 0 ? 0 : (entering >= length ? length - 1 : entering);
            const unsigned gone = in[(long)a * step];
            const unsigned came = in[(long)b * step];
            sum_r += ((came >> 16) & 0xFF) - ((gone >> 16) & 0xFF);
            sum_g += ((came >> 8) & 0xFF) - ((gone >> 8) & 0xFF);
            sum_b += (came & 0xFF) - (gone & 0xFF);
        }
    }
}

void draw_blur(const struct surface* s, int x, int y, int w, int h,
               int radius, int corner)
{
    if (w <= 0 || h <= 0 || radius <= 0)
        return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    if (w <= 0 || h <= 0)
        return;

    /* The shape being blurred *through*, which is not the region being
     * blurred once the clip has narrowed the second one. */
    const int ox = x, oy = y, ow = w, oh = h;

    /* Only as much as can actually be written, plus the distance the blur
     * reaches for.
     *
     * Nothing here looked at the clip, so nudging the mouse over a window -
     * a sixteen pixel damage rectangle - downsampled, blurred three times and
     * stretched back the entire window. That was the whole of the lag: a
     * one-pixel change cost exactly what a full repaint cost.
     *
     * Three box passes of radius r spread about 3r, so a sub-region grown by
     * that much and then written back only inside the clip is
     * indistinguishable from blurring the lot. */
    if (s->cw > 0 && s->ch > 0) {
        const int reach = radius * 3 + BLUR_SHRINK;
        int x0 = s->cx - reach, y0 = s->cy - reach;
        int x1 = s->cx + s->cw + reach, y1 = s->cy + s->ch + reach;
        if (x0 < x) x0 = x;
        if (y0 < y) y0 = y;
        if (x1 > x + w) x1 = x + w;
        if (y1 > y + h) y1 = y + h;
        if (x1 <= x0 || y1 <= y0)
            return;                     /* nothing of it is visible */
        x = x0; y = y0;
        w = x1 - x0; h = y1 - y0;
    }

    const int sw = (w + BLUR_SHRINK - 1) / BLUR_SHRINK;
    const int sh = (h + BLUR_SHRINK - 1) / BLUR_SHRINK;
    if (sw < 1 || sh < 1)
        return;

    unsigned* small = (unsigned*)malloc((size_t)sw * sh * sizeof(unsigned));
    unsigned* scratch = (unsigned*)malloc((size_t)sw * sh * sizeof(unsigned));
    if (small == 0 || scratch == 0) {
        free(small);
        free(scratch);
        return;
    }

    /* Down: each small pixel is the average of the block it stands for, so
     * nothing is dropped on the way - a point sample here would alias, and
     * alias visibly, because the thing being shrunk is a photograph. */
    for (int sy = 0; sy < sh; ++sy) {
        for (int sx = 0; sx < sw; ++sx) {
            unsigned r = 0, g = 0, b = 0, n = 0;
            for (int by = 0; by < BLUR_SHRINK; ++by) {
                const int py = y + sy * BLUR_SHRINK + by;
                if (py >= y + h)
                    break;
                const uint32_t* row = &s->pixels[(long)py * s->w];
                for (int bx = 0; bx < BLUR_SHRINK; ++bx) {
                    const int px = x + sx * BLUR_SHRINK + bx;
                    if (px >= x + w)
                        break;
                    const uint32_t p = row[px];
                    r += (p >> 16) & 0xFF;
                    g += (p >> 8) & 0xFF;
                    b += p & 0xFF;
                    ++n;
                }
            }
            if (n == 0) n = 1;
            small[(long)sy * sw + sx] = 0xFF000000u | ((r / n) << 16)
                                      | ((g / n) << 8) | (b / n);
        }
    }

    int small_radius = radius / BLUR_SHRINK;
    if (small_radius < 1) small_radius = 1;
    if (small_radius > sw / 2) small_radius = sw / 2 > 1 ? sw / 2 : 1;
    if (small_radius > sh / 2) small_radius = sh / 2 > 1 ? sh / 2 : 1;

    for (int pass = 0; pass < 3; ++pass) {
        box_pass(small, scratch, sw, sh, small_radius, 1);
        box_pass(scratch, small, sw, sh, small_radius, 0);
    }

    /* Up, bilinearly. Nearest-neighbour here would put visible four-pixel
     * blocks through the middle of a blur, which is the one artefact that
     * gives the trick away. */
    for (int py = 0; py < h; ++py) {
        const float fy = ((float)py + 0.5f) / BLUR_SHRINK - 0.5f;
        int y0 = (int)floor(fy);
        float ty = fy - (float)y0;
        if (y0 < 0) { y0 = 0; ty = 0.0f; }
        int y1 = y0 + 1;
        if (y1 >= sh) { y1 = sh - 1; if (y0 > y1) y0 = y1; }

        uint32_t* out = &s->pixels[(long)(y + py) * s->w + x];
        for (int px = 0; px < w; ++px) {
            const float fx = ((float)px + 0.5f) / BLUR_SHRINK - 0.5f;
            int x0 = (int)floor(fx);
            float tx = fx - (float)x0;
            if (x0 < 0) { x0 = 0; tx = 0.0f; }
            int x1 = x0 + 1;
            if (x1 >= sw) { x1 = sw - 1; if (x0 > x1) x0 = x1; }

            const unsigned a = small[(long)y0 * sw + x0];
            const unsigned b = small[(long)y0 * sw + x1];
            const unsigned c = small[(long)y1 * sw + x0];
            const unsigned d = small[(long)y1 * sw + x1];

            unsigned out_r = 0, out_g = 0, out_b = 0;
            for (int channel = 0; channel < 3; ++channel) {
                const int shift = 16 - 8 * channel;
                const float va = (float)((a >> shift) & 0xFF);
                const float vb = (float)((b >> shift) & 0xFF);
                const float vc = (float)((c >> shift) & 0xFF);
                const float vd = (float)((d >> shift) & 0xFF);
                const float top = va + (vb - va) * tx;
                const float bottom = vc + (vd - vc) * tx;
                const unsigned v = (unsigned)(top + (bottom - top) * ty + 0.5f);
                if (channel == 0) out_r = v;
                else if (channel == 1) out_g = v;
                else out_b = v;
            }
            const uint32_t blurred = 0xFF000000u | (out_r << 16)
                                   | (out_g << 8) | out_b;
            if (corner <= 0) {
                out[px] = blurred;
                continue;
            }
            /* Through the rounded shape, not the rectangle it lives in.
             *
             * Blurring the whole box and letting the panel cover it leaves the
             * corners smeared *outside* the shape - four soft squares poking
             * out from behind rounded glass, which is the first thing the eye
             * catches and the last thing anybody can name. Weighting the
             * write-back by the same coverage the fill uses puts the blur
             * exactly where the panel is and nowhere else. */
            const int inside = draw_round_coverage(x + px, y + py,
                                                   ox, oy, ow, oh, corner);
            if (inside == 0)
                continue;
            out[px] = inside == 255
                        ? blurred
                        : draw_over(out[px], ((unsigned)inside << 24) |
                                             (blurred & 0x00FFFFFFu));
        }
    }

    free(small);
    free(scratch);
}

/* --- filling a path ---------------------------------------------------------
 *
 * Shared with the font, which is where it started. A glyph and an icon are the
 * same problem once the outline exists: closed contours in, coverage out. The
 * comment explaining why this accumulates signed area rather than testing
 * pixel centres is in font.c, where the need for it is obvious.
 */

void draw_edge_deposit(float* area, int w, int h, int stride,
                    float x0, float y0, float x1, float y1)
{
    float direction = 1.0f;
    if (y0 > y1) {
        float t;
        t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
        direction = -1.0f;
    }
    if (y1 <= 0.0f || y0 >= (float)h)
        return;

    const float dxdy = (x1 - x0) / (y1 - y0);
    if (y0 < 0.0f) {
        x0 -= y0 * dxdy;                /* enter at the top edge instead */
        y0 = 0.0f;
    }

    int y = (int)y0;
    const int y_end = (int)((y1 < (float)h ? y1 : (float)h) + 0.9999f);
    float x = x0;

    for (; y < y_end && y < h; ++y) {
        const float top    = (float)y     > y0 ? (float)y     : y0;
        const float bottom = (float)(y + 1) < y1 ? (float)(y + 1) : y1;
        const float dy = bottom - top;
        if (dy <= 0.0f)
            continue;

        const float x_next = x + dxdy * dy;
        const float d = dy * direction;

        float left  = x < x_next ? x : x_next;
        float right = x < x_next ? x_next : x;
        if (left < 0.0f)  left = 0.0f;
        if (right < 0.0f) right = 0.0f;
        if (left  > (float)w) left  = (float)w;
        if (right > (float)w) right = (float)w;

        float* row = area + (long)y * stride;
        const float left_floor = floor(left);
        const int   first = (int)left_floor;
        const int   after = (int)ceil(right);   /* one past the last column */

        if (after <= first + 1) {
            /* The crossing stays within one column. Split the deposit between
             * it and its neighbour by where the midpoint of the crossing fell,
             * which is the same formula as below with the sum collapsed. */
            const float middle = 0.5f * (left + right) - left_floor;
            row[first]     += d * (1.0f - middle);
            row[first + 1] += d * middle;
        } else {
            /* Across several columns. Each gets the area of the trapezium the
             * edge cut out of it, which for a straight line is exact - and the
             * whole point of doing this analytically rather than by sampling.
             *
             * The first and last columns are partial wedges; everything
             * between them is crossed completely and takes an equal share. The
             * running total is carried along so the last column can be given
             * whatever is left, which keeps the row summing to exactly d and
             * stops a seam appearing down the middle of a wide stroke. */
            const float inverse   = 1.0f / (right - left);
            const float into_first = left - left_floor;
            const float first_area = 0.5f * inverse *
                                     (1.0f - into_first) * (1.0f - into_first);
            const float past_last  = right - (float)after + 1.0f;
            const float last_area  = 0.5f * inverse * past_last * past_last;

            row[first] += d * first_area;

            if (after == first + 2) {
                row[first + 1] += d * (1.0f - first_area - last_area);
            } else {
                const float second = inverse * (1.5f - into_first);
                row[first + 1] += d * (second - first_area);
                for (int c = first + 2; c < after - 1; ++c)
                    row[c] += d * inverse;
                const float before_last =
                    second + (float)(after - first - 3) * inverse;
                row[after - 1] += d * (1.0f - before_last - last_area);
            }
            row[after] += d * last_area;
        }
        x = x_next;
    }
}

void draw_area_resolve(const float* area, int w, int h, int stride,
                       unsigned char* out)
{
    for (int y = 0; y < h; ++y) {
        const float* row = area + (long)y * stride;
        unsigned char* line = out + (long)y * w;
        float running = 0.0f;
        for (int x = 0; x < w; ++x) {
            running += row[x];
            float c = running < 0.0f ? -running : running;
            if (c > 1.0f)
                c = 1.0f;
            line[x] = (unsigned char)(c * 255.0f + 0.5f);
        }
    }
}


/* --- shadows ---------------------------------------------------------------- */

struct shadow {
    unsigned char* alpha;       /* w * h coverage */
    int w, h;
    int margin;                 /* how far it extends past the shape */
};

/* A blurred coverage mask. Separate from draw_blur because that one works on
 * colour in a surface and this one on a single channel in a scratch buffer -
 * sharing them would mean widening one to a case it never sees. */
static void blur_alpha(unsigned char* a, int w, int h, int radius)
{
    unsigned char* tmp = (unsigned char*)malloc((size_t)w * h);
    if (tmp == 0)
        return;
    const int window = radius * 2 + 1;

    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < h; ++y) {
            unsigned sum = 0;
            for (int k = -radius; k <= radius; ++k) {
                const int at = k < 0 ? 0 : (k >= w ? w - 1 : k);
                sum += a[(long)y * w + at];
            }
            for (int x = 0; x < w; ++x) {
                tmp[(long)y * w + x] = (unsigned char)(sum / window);
                const int gone = x - radius < 0 ? 0 : x - radius;
                const int came = x + radius + 1 >= w ? w - 1 : x + radius + 1;
                sum += a[(long)y * w + came] - a[(long)y * w + gone];
            }
        }
        for (int x = 0; x < w; ++x) {
            unsigned sum = 0;
            for (int k = -radius; k <= radius; ++k) {
                const int at = k < 0 ? 0 : (k >= h ? h - 1 : k);
                sum += tmp[(long)at * w + x];
            }
            for (int y = 0; y < h; ++y) {
                a[(long)y * w + x] = (unsigned char)(sum / window);
                const int gone = y - radius < 0 ? 0 : y - radius;
                const int came = y + radius + 1 >= h ? h - 1 : y + radius + 1;
                sum += tmp[(long)came * w + x] - tmp[(long)gone * w + x];
            }
        }
    }
    free(tmp);
}

struct shadow* draw_shadow_make(int w, int h, int corner, int radius,
                                int opacity)
{
    if (w <= 0 || h <= 0 || radius <= 0)
        return 0;

    struct shadow* s = (struct shadow*)malloc(sizeof(struct shadow));
    if (s == 0)
        return 0;

    /* Room for the blur to spread into. Without the margin the shadow is cut
     * off square at the shape's own edge, which reads as a dark rectangle
     * rather than as light falling around something. */
    s->margin = radius * 2;
    s->w = w + s->margin * 2;
    s->h = h + s->margin * 2;
    s->alpha = (unsigned char*)malloc((size_t)s->w * s->h);
    if (s->alpha == 0) {
        free(s);
        return 0;
    }
    memset(s->alpha, 0, (size_t)s->w * s->h);

    if (opacity < 0) opacity = 0;
    if (opacity > 255) opacity = 255;

    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int c = draw_round_coverage(x, y, 0, 0, w, h, corner);
            s->alpha[(long)(y + s->margin) * s->w + (x + s->margin)] =
                (unsigned char)((c * opacity + 127) / 255);
        }

    blur_alpha(s->alpha, s->w, s->h, radius);
    return s;
}

void draw_shadow_free(struct shadow* s)
{
    if (s == 0)
        return;
    free(s->alpha);
    free(s);
}

void draw_shadow_cast(const struct surface* target, const struct shadow* s,
                      int x, int y, int drop)
{
    if (s == 0)
        return;
    const int ox = x - s->margin;
    const int oy = y - s->margin + drop;

    /* Bounded by the clip up front rather than tested per pixel. A shadow mask
     * is larger than the window it belongs to, and walking all of it to write
     * sixteen pixels is the same waste the blur had. */
    int sy0 = 0, sy1 = s->h, sx0 = 0, sx1 = s->w;
    if (target->cw > 0 && target->ch > 0) {
        if (target->cy - oy > sy0) sy0 = target->cy - oy;
        if (target->cx - ox > sx0) sx0 = target->cx - ox;
        if (target->cy + target->ch - oy < sy1) sy1 = target->cy + target->ch - oy;
        if (target->cx + target->cw - ox < sx1) sx1 = target->cx + target->cw - ox;
    }
    if (sy0 < 0) sy0 = 0;
    if (sx0 < 0) sx0 = 0;

    for (int sy = sy0; sy < sy1; ++sy) {
        const int py = oy + sy;
        if (py < 0 || py >= target->h)
            continue;
        uint32_t* row = &target->pixels[(long)py * target->w];
        const unsigned char* line = &s->alpha[(long)sy * s->w];
        for (int sx = sx0; sx < sx1; ++sx) {
            const unsigned a = line[sx];
            if (a == 0)
                continue;
            const int px = ox + sx;
            if (px < 0 || px >= target->w)
                continue;
            row[px] = draw_over(row[px], a << 24);   /* black, at that alpha */
        }
    }
}
