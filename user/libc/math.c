/* The maths library.
 *
 * Every function here reduces its argument to a small range, evaluates a
 * polynomial there, and puts the reduction back. That is the whole shape of
 * the thing; the interest is in which range and which polynomial.
 *
 * The polynomials are Taylor series, with enough terms that the first omitted
 * one is below the last bit of a double over the reduced range. A minimax
 * polynomial would reach the same accuracy in two or three fewer terms, but
 * its coefficients are the output of a fitting process that cannot be checked
 * by reading them. A factorial can. The terms cost a multiply and an add each
 * and this is not the expensive part of anything.
 *
 * Coefficients are written as divisions - (1.0/6.0) rather than
 * 0.16666666666666666 - because the compiler folds them to the correctly
 * rounded double, and because a transcribed seventeen-digit constant is a
 * typo waiting to be found by someone else.
 */

#include <math.h>
#include <stdint.h>

/* --- moving between a double and its bits ---------------------------------- */

static uint64_t bits_of(double x)
{
    uint64_t u;
    __builtin_memcpy(&u, &x, sizeof(u));
    return u;
}

static double from_bits(uint64_t u)
{
    double x;
    __builtin_memcpy(&x, &u, sizeof(x));
    return x;
}

/* --- classification --------------------------------------------------------- */

int isnan(double x)    { return x != x; }
int isinf(double x)    { return !isnan(x) && x != 0.0 && x + x == x; }
int isfinite(double x) { return !isnan(x) && !isinf(x); }
int signbit(double x)  { return (int)(bits_of(x) >> 63); }

/* --- the hardware's own ----------------------------------------------------- */

double fabs(double x)
{
    /* Clearing the sign bit, which is exact for every value including NaN and
     * the infinities - where negating by subtraction would not be. */
    return from_bits(bits_of(x) & 0x7FFFFFFFFFFFFFFFull);
}

double sqrt(double x)
{
    /* SSE2 has this in silicon and correctly rounded, which is more than any
     * polynomial here manages. A negative argument produces a NaN, as IEEE
     * says it should, and -0 comes back as -0. */
#if defined(__x86_64__)
    double r;
    __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
    return r;
#else
    /* Not the target, but this file is also compiled on the build host to
     * check it against a known-good libm, and that host is not x86. Every
     * architecture worth testing on has the instruction; asking for it by
     * name gets it without going through a library call. */
    return __builtin_sqrt(x);
#endif
}

/* --- rounding ----------------------------------------------------------------
 *
 * SSE4.1 has ROUNDSD, which does all of these in one instruction. SSE2, which
 * is the baseline this system builds for, does not - so they go through the
 * integer conversion, which truncates, and are adjusted from there.
 *
 * The magnitude test is not an optimisation. Every double of magnitude 2^52 or
 * more is already an integer, and the conversion these use would overflow long
 * before that; returning x unchanged is both correct and the only option. */

#define BIG 4503599627370496.0          /* 2^52 */

double trunc(double x)
{
    if (!isfinite(x) || fabs(x) >= BIG)
        return x;
    const double r = (double)(long long)x;
    /* (long long) rounds toward zero, which is what trunc means - but -0.5
     * becomes +0.0 and should stay -0.0. */
    return (r == 0.0 && signbit(x)) ? -0.0 : r;
}

double floor(double x)
{
    const double t = trunc(x);
    if (!isfinite(x) || fabs(x) >= BIG)
        return x;
    return (t > x) ? t - 1.0 : t;
}

double ceil(double x)
{
    const double t = trunc(x);
    if (!isfinite(x) || fabs(x) >= BIG)
        return x;
    return (t < x) ? t + 1.0 : t;
}

double round(double x)
{
    if (!isfinite(x) || fabs(x) >= BIG)
        return x;
    /* Halfway away from zero, which is what C says round does - and not what
     * the hardware's nearest-even conversion would give. */
    const double t = trunc(x);
    const double frac = x - t;
    if (frac >= 0.5)  return t + 1.0;
    if (frac <= -0.5) return t - 1.0;
    return t;
}

/* --- exponent surgery -------------------------------------------------------- */

double frexp(double x, int* exponent)
{
    *exponent = 0;
    if (x == 0.0 || !isfinite(x))
        return x;

    uint64_t u = bits_of(x);
    int e = (int)((u >> 52) & 0x7FF);
    if (e == 0) {
        /* Subnormal: scale it into the normal range and take the cost off the
         * exponent afterwards. 2^54 is enough to normalise the smallest. */
        x *= 18014398509481984.0;        /* 2^54 */
        u = bits_of(x);
        e = (int)((u >> 52) & 0x7FF) - 54;
    }
    *exponent = e - 1022;
    /* Force the exponent field to 1022, which puts the mantissa in [0.5, 1). */
    u = (u & ~(0x7FFull << 52)) | (1022ull << 52);
    return from_bits(u);
}

double ldexp(double x, int exponent)
{
    if (x == 0.0 || !isfinite(x))
        return x;
    /* Applied in steps small enough that each scale factor is itself a normal
     * double. One 2^n built directly would be a nonsense bit pattern for
     * |n| past 1023, and the intermediate would flush to zero or infinity in
     * cases where the final answer is perfectly representable. */
    while (exponent > 1023) {
        x *= from_bits(1023ull << 52);   /* 2^1023 */
        exponent -= 1023;
        if (isinf(x))
            return x;
    }
    while (exponent < -1022) {
        x *= from_bits(1ull << 52);      /* 2^-1022 */
        exponent += 1022;
        if (x == 0.0)
            return x;
    }
    return x * from_bits((uint64_t)(exponent + 1023) << 52);
}

double fmod(double x, double y)
{
    if (isnan(x) || isnan(y) || isinf(x) || y == 0.0)
        return NAN;
    if (isinf(y) || x == 0.0)
        return x;

    const int negative = signbit(x);
    double a = fabs(x);
    const double b = fabs(y);
    if (a < b)
        return x;

    /* Repeated subtraction, but by the largest shifted multiple of b that
     * still fits - so this runs in as many steps as the two numbers differ in
     * exponent, not in as many steps as b goes into a. Each subtraction is
     * exact, because the operands are within a factor of two of each other. */
    int ea, eb;
    frexp(a, &ea);
    frexp(b, &eb);
    for (int shift = ea - eb; shift >= 0; --shift) {
        const double scaled = ldexp(b, shift);
        if (scaled <= a)
            a -= scaled;
    }
    return negative ? -a : a;
}

/* --- exp ---------------------------------------------------------------------
 *
 * x = k*ln2 + r, so exp(x) = 2^k * exp(r) with |r| <= ln2/2. The power of two
 * is exact and free; the polynomial only has to cover a range of about 0.35,
 * where the Taylor series converges very fast.
 *
 * ln2 is subtracted in two pieces. k*LN2_HI is exact - LN2_HI has its low
 * mantissa bits clear, so the product has no rounding - which means the whole
 * error of the reduction is in the second, much smaller, subtraction. */

#define LN2_HI   0x1.62e4200000000p-1
#define LN2_LO   0x1.fdf473de6af28p-22
#define INV_LN2  0x1.71547652b82fep+0

double exp(double x)
{
    if (isnan(x))
        return x;
    if (x > 709.782712893384)           /* the largest that does not overflow */
        return HUGE_VAL;
    if (x < -745.1332191019411)         /* below this it underflows to zero */
        return 0.0;

    const double kd = round(x * INV_LN2);
    const int k = (int)kd;
    const double r = (x - kd * LN2_HI) - kd * LN2_LO;

    /* 1 + r + r^2/2! + ... through r^13/13!. The next term over |r| <= 0.347
     * is about 4e-18, which is below the last bit of the result. */
    const double p =
        1.0 + r * (1.0 + r * ((1.0/2.0) + r * ((1.0/6.0) + r * ((1.0/24.0) +
        r * ((1.0/120.0) + r * ((1.0/720.0) + r * ((1.0/5040.0) +
        r * ((1.0/40320.0) + r * ((1.0/362880.0) + r * ((1.0/3628800.0) +
        r * ((1.0/39916800.0) + r * ((1.0/479001600.0) +
        r * (1.0/6227020800.0)))))))))))));

    return ldexp(p, k);
}

/* --- log ---------------------------------------------------------------------
 *
 * x = 2^k * m with m in [sqrt(2)/2, sqrt(2)), so log(x) = k*ln2 + log(m). The
 * bracket is chosen around 1 rather than starting at 1 so that |log(m)| is as
 * small as it can be, which is what makes the series short.
 *
 * log(m) is evaluated as 2*atanh(s) with s = (m-1)/(m+1). The atanh series has
 * only odd powers and s stays under 0.1716 over that bracket, so it converges
 * roughly twice as fast as the series for log(1+f) does directly. */

/* --- a little extra precision ------------------------------------------------
 *
 * pow(x, y) is exp(y * log(x)), and that composition is where accuracy goes to
 * die. An error of one last-place unit in log(x) is multiplied by y and then
 * *exponentiated*, so for y around seventeen the answer came out some thirty
 * units out. Rounding the product y*log(x) to a double contributes about as
 * much again.
 *
 * Both are fixed the same way: carry the intermediate as an unevaluated sum of
 * two doubles, a leading term and the exact residual that rounding threw away.
 * These two are the standard means of producing that residual without any
 * wider type - Dekker's, from 1971, and they need nothing but the arithmetic
 * already here.
 */

/* a + b as a rounded sum and the part that did not fit. */
static void two_sum(double a, double b, double* sum, double* residual)
{
    *sum = a + b;
    const double shifted = *sum - a;
    *residual = (a - (*sum - shifted)) + (b - shifted);
}

/* a * b, likewise. Each operand is split into halves narrow enough that their
 * products are exact, so the residual can be recovered by subtraction. */
static void two_product(double a, double b, double* product, double* residual)
{
    const double split = 134217729.0;           /* 2^27 + 1 */
    double c = split * a;
    const double a_hi = c - (c - a), a_lo = a - a_hi;
    c = split * b;
    const double b_hi = c - (c - b), b_lo = b - b_hi;

    *product = a * b;
    *residual = ((a_hi * b_hi - *product) + a_hi * b_lo + a_lo * b_hi) +
                a_lo * b_lo;
}

/* log(x) as hi + lo, with hi the double log() would return and lo the part
 * below its last bit. Only pow needs this; log() itself just adds them. */
static void log_extended(double x, double* hi, double* lo)
{
    int k;
    double m = frexp(x, &k);
    if (m < M_SQRT1_2) {
        m *= 2.0;
        --k;
    }

    /* s = (m-1)/(m+1), carried to twice the precision. The numerator is exact
     * - subtracting 1 from something between 0.7 and 1.5 loses nothing - so
     * the whole error is the division, and that is recoverable: multiply the
     * quotient back out and divide the shortfall again. */
    const double numerator = m - 1.0, denominator = m + 1.0;
    const double s_hi = numerator / denominator;
    double product, residual;
    two_product(s_hi, denominator, &product, &residual);
    const double s_lo = ((numerator - product) - residual) / denominator;

    const double w = s_hi * s_hi;
    /* Everything past the leading 2s. It is under a percent of the result, so
     * its own rounding is far below the last bit of the whole. */
    const double tail =
        w * ((1.0/3.0) + w * ((1.0/5.0) + w * ((1.0/7.0) +
        w * ((1.0/9.0) + w * ((1.0/11.0) + w * ((1.0/13.0) + w * ((1.0/15.0) +
        w * ((1.0/17.0) + w * ((1.0/19.0) + w * (1.0/21.0)))))))))); 

    /* Everything below the leading 2*s_hi, which is small but is a genuine
     * part of the value rather than a rounding residual. */
    const double small = 2.0 * s_lo + 2.0 * s_hi * tail;

    /* Summed largest first, keeping what each addition dropped. `hi` ends up
     * being the double log() would have returned and `lo` the part below its
     * last bit - which is the whole point. Putting a real term such as `small`
     * or k*LN2_LO into `lo` instead would leave `lo` around 1e-3, and a
     * caller treating that as a residual would be badly wrong. */
    double t1, t2, e1, e2, e3;
    two_sum((double)k * LN2_HI, 2.0 * s_hi, &t1, &e1);
    two_sum(t1, small, &t2, &e2);
    two_sum(t2, (double)k * LN2_LO, hi, &e3);
    *lo = (e1 + e2) + e3;
}

double log(double x)
{
    if (isnan(x))    return x;
    if (x < 0.0)     return NAN;
    if (x == 0.0)    return -HUGE_VAL;
    if (isinf(x))    return x;

    int k;
    double m = frexp(x, &k);            /* m in [0.5, 1) */
    if (m < M_SQRT1_2) {
        m *= 2.0;                       /* into [sqrt(2)/2, sqrt(2)) */
        --k;
    }

    const double s = (m - 1.0) / (m + 1.0);
    const double w = s * s;

    /* s + s^3/3 + ... + s^21/21, doubled. The next term over |s| <= 0.1716 is
     * about 4e-18. */
    const double series =
        1.0 + w * ((1.0/3.0) + w * ((1.0/5.0) + w * ((1.0/7.0) +
        w * ((1.0/9.0) + w * ((1.0/11.0) + w * ((1.0/13.0) + w * ((1.0/15.0) +
        w * ((1.0/17.0) + w * ((1.0/19.0) + w * (1.0/21.0))))))))));

    /* k*ln2 in two pieces for the same reason exp splits it: k*LN2_HI is
     * exact, so k contributes no rounding error of its own. */
    return (double)k * LN2_HI + (2.0 * s * series + (double)k * LN2_LO);
}

double log2(double x)  { return log(x) * M_LOG2E; }
double log10(double x) { return log(x) * M_LOG10E; }

/* --- sin and cos --------------------------------------------------------------
 *
 * The angle is folded into [-pi/4, pi/4] by subtracting a whole number of
 * quarter turns, and which quarter it was decides whether the sine kernel or
 * the cosine kernel is used and with what sign. Both series are then evaluated
 * over a range where they converge quickly and where neither has to cope with
 * cancellation.
 *
 * pi/2 is subtracted in three pieces, for the same reason ln2 is subtracted in
 * two: the first two products are exact, so the error of the fold is only what
 * the third piece contributes. This holds to full precision while n fits in
 * about 33 bits, which is |x| up to roughly 2^20. Past that the answer decays,
 * and past 2^40 it is meaningless - getting that right needs Payne-Hanek
 * reduction against a many-digit 2/pi, which is a different piece of work. */

#define PIO2_HI   0x1.921fb00000000p+0
#define PIO2_MID  0x1.5110b00000000p-22
#define PIO2_LO   0x1.18469898cc517p-44
#define TWO_OVER_PI 0x1.45f306dc9c883p-1

/* sin(r) for |r| <= pi/4, as r - r^3/3! + ... - r^17/17!. */
static double sin_kernel(double r)
{
    const double z = r * r;
    return r * (1.0 - z * ((1.0/6.0) - z * ((1.0/120.0) - z * ((1.0/5040.0) -
        z * ((1.0/362880.0) - z * ((1.0/39916800.0) - z * ((1.0/6227020800.0) -
        z * ((1.0/1307674368000.0) - z * (1.0/355687428096000.0)))))))));
}

/* cos(r) for |r| <= pi/4, as 1 - r^2/2! + ... + r^16/16!. */
static double cos_kernel(double r)
{
    const double z = r * r;
    return 1.0 - z * ((1.0/2.0) - z * ((1.0/24.0) - z * ((1.0/720.0) -
        z * ((1.0/40320.0) - z * ((1.0/3628800.0) - z * ((1.0/479001600.0) -
        z * ((1.0/87178291200.0) - z * (1.0/20922789888000.0))))))));
}

/* Fold x into [-pi/4, pi/4]. Returns the quadrant, 0..3. */
static int fold(double x, double* r)
{
    const double nd = round(x * TWO_OVER_PI);
    *r = ((x - nd * PIO2_HI) - nd * PIO2_MID) - nd * PIO2_LO;
    /* The quadrant only needs the low two bits, and fmod keeps nd inside what
     * an int can hold even when x is enormous. */
    const double q = fmod(nd, 4.0);
    int n = (int)q;
    return (n < 0) ? n + 4 : n;
}

double sin(double x)
{
    if (!isfinite(x))
        return NAN;                     /* including sin(inf), which has none */
    double r;
    switch (fold(x, &r)) {
    case 0:  return  sin_kernel(r);
    case 1:  return  cos_kernel(r);
    case 2:  return -sin_kernel(r);
    default: return -cos_kernel(r);
    }
}

double cos(double x)
{
    if (!isfinite(x))
        return NAN;
    double r;
    switch (fold(x, &r)) {
    case 0:  return  cos_kernel(r);
    case 1:  return -sin_kernel(r);
    case 2:  return -cos_kernel(r);
    default: return  sin_kernel(r);
    }
}

double tan(double x)
{
    const double c = cos(x);
    /* No special case for c == 0: the division gives an infinity of the right
     * sign, which is the closest a double gets to what tan does there. */
    return sin(x) / c;
}

/* --- the inverse trigonometry -------------------------------------------------
 *
 * atan is the one that has to be built; the other three are it plus algebra.
 *
 * Its Taylor series converges on |x| < 1 and does so uselessly slowly near the
 * end of that - at x = 1 the terms are 1, 1/3, 1/5 and it would take millions
 * of them. So the argument is folded twice. Anything past 1 is turned inside
 * out by atan(x) = pi/2 - atan(1/x), and what remains is folded again against
 * a thirty-degree angle, leaving |t| no larger than tan(pi/12), about 0.268.
 * There the series needs fourteen terms to fall below the last bit.
 */

#define SQRT3_HI  0x1.bb67a00000000p+0
#define SQRT3_LO  0x1.d0b09954e764bp-21
#define M_PI_6    0x1.0c152382d7366p-1
#define TAN_PI_12 0.26794919243112270647          /* 2 - sqrt(3) */

/* atan(t) for |t| <= tan(pi/12), as t - t^3/3 + t^5/5 - ... - t^29/29. */
static double atan_kernel(double t)
{
    const double w = t * t;
    return t * (1.0 - w * ((1.0/3.0) - w * ((1.0/5.0) - w * ((1.0/7.0) -
        w * ((1.0/9.0) - w * ((1.0/11.0) - w * ((1.0/13.0) - w * ((1.0/15.0) -
        w * ((1.0/17.0) - w * ((1.0/19.0) - w * ((1.0/21.0) - w * ((1.0/23.0) -
        w * ((1.0/25.0) - w * ((1.0/27.0) - w * (1.0/29.0)))))))))))))));
}

/* atan for a non-negative, finite argument. */
static double atan_positive(double x)
{
    if (x <= TAN_PI_12)
        return atan_kernel(x);

    if (x > 1.0)
        return M_PI_2 - atan_positive(1.0 / x);

    /* atan(x) = pi/6 + atan((x*sqrt3 - 1) / (x + sqrt3)), which brings
     * [tan(pi/12), 1] down into [0, tan(pi/12)].
     *
     * The numerator cancels almost completely near x = 1/sqrt(3), where it is
     * meant to reach zero. Splitting sqrt(3) into a head with its low bits
     * clear makes x*SQRT3_HI - 1 exact, so what cancellation there is happens
     * on a term that is exactly right, and the small correction is added
     * afterwards rather than being lost inside it. */
    const double numerator = (x * SQRT3_HI - 1.0) + x * SQRT3_LO;
    const double denominator = x + SQRT3_HI + SQRT3_LO;
    return M_PI_6 + atan_kernel(numerator / denominator);
}

double atan(double x)
{
    if (isnan(x))
        return x;
    if (isinf(x))
        return signbit(x) ? -M_PI_2 : M_PI_2;
    const double r = atan_positive(fabs(x));
    return signbit(x) ? -r : r;
}

double atan2(double y, double x)
{
    if (isnan(x) || isnan(y))
        return NAN;

    /* The infinities have their own answers, because y/x would be a NaN for
     * two of them and the direction is perfectly well defined. */
    if (isinf(x) || isinf(y)) {
        if (isinf(x) && isinf(y)) {
            /* Diagonals: the eighths of the circle. */
            const double base = signbit(x) ? 3.0 * M_PI_4 : M_PI_4;
            return signbit(y) ? -base : base;
        }
        if (isinf(y))
            return signbit(y) ? -M_PI_2 : M_PI_2;
        /* x is the infinite one, so the direction is along the axis. */
        const double base = signbit(x) ? M_PI : 0.0;
        return signbit(y) ? -base : base;
    }

    if (x == 0.0) {
        if (y == 0.0) {
            /* Not an error: a zero has a sign, and C says to use it. The
             * answer is which side of the axis the point is on. */
            const double base = signbit(x) ? M_PI : 0.0;
            return signbit(y) ? -base : base;
        }
        return signbit(y) ? -M_PI_2 : M_PI_2;
    }
    if (y == 0.0) {
        const double base = signbit(x) ? M_PI : 0.0;
        return signbit(y) ? -base : base;
    }

    /* The ratio can overflow or flush to zero when the two are wildly
     * different in magnitude, and in exactly those cases the angle is an axis
     * or a diagonal to well within a last bit anyway. */
    int ey, ex;
    frexp(y, &ey);
    frexp(x, &ex);
    double angle;
    if (ey - ex > 1100)
        angle = M_PI_2;                 /* |y/x| past anything a double holds */
    else if (ex - ey > 1100)
        angle = 0.0;
    else
        angle = atan_positive(fabs(y / x));

    if (!signbit(x))
        return signbit(y) ? -angle : angle;
    /* Left half plane: reflect through the vertical axis. */
    const double reflected = M_PI - angle;
    return signbit(y) ? -reflected : reflected;
}

/* asin and acos both come from atan, but not by the obvious identity alone.
 *
 * asin(x) = atan(x / sqrt(1 - x^2)) is exact enough while |x| is small, and
 * falls apart as |x| approaches one: 1 - x^2 cancels, and the square root of a
 * badly-known small number is worse still. So the top half of the range is
 * folded onto the bottom by the half-angle identity, where the subtraction
 * 1 - |x| is exact because the two are within a factor of two of each other. */

double asin(double x)
{
    if (isnan(x))
        return x;
    const double a = fabs(x);
    if (a > 1.0)
        return NAN;                     /* outside the domain */
    if (a == 1.0)
        return signbit(x) ? -M_PI_2 : M_PI_2;

    double r;
    if (a <= 0.9) {
        /* 0.9 rather than the 0.5 the half-angle identity would allow. The
         * direct form only suffers where 1 - a*a cancels, and at 0.9 that is
         * still 0.19 - no cancellation worth the name. The folded form has a
         * subtraction from pi/2 of its own, and measured worse: pushing the
         * boundary out took the worst case from five last-place units to
         * three. */
        r = atan_positive(a / sqrt(1.0 - a * a));
    } else {
        /* Close to one, where 1 - a*a would lose most of its digits. Folded
         * instead: asin(a) = pi/2 - 2*asin(sqrt((1-a)/2)). Here 1 - a is exact,
         * the two being within a factor of two of each other. */
        const double half = (1.0 - a) * 0.5;
        const double s = sqrt(half);
        r = M_PI_2 - 2.0 * atan_positive(s / sqrt(1.0 - half));
    }
    return signbit(x) ? -r : r;
}

double acos(double x)
{
    if (isnan(x))
        return x;
    if (x > 1.0 || x < -1.0)
        return NAN;

    /* Not pi/2 - asin(x) throughout: near x = 1 the answer is near zero and
     * that subtraction cancels away most of the digits. The halves are folded
     * so that the small answer is computed small. */
    if (x >= 0.5) {
        const double half = (1.0 - x) * 0.5;
        const double s = sqrt(half);
        return 2.0 * atan_positive(s / sqrt(1.0 - half));
    }
    if (x <= -0.5) {
        const double half = (1.0 + x) * 0.5;
        const double s = sqrt(half);
        return M_PI - 2.0 * atan_positive(s / sqrt(1.0 - half));
    }
    return M_PI_2 - asin(x);
}

/* --- pow ---------------------------------------------------------------------
 *
 * exp(y * log(x)) in the general case, and that is where the accuracy goes:
 * log(x) is good to about a last bit, multiplying by y scales that error, and
 * exp then amplifies whatever is left. For a large y the result can be several
 * bits out.
 *
 * So a whole-number exponent - which is most of the uses - does not go that
 * way at all. Binary exponentiation is a chain of multiplications, each
 * correctly rounded, and the error grows with the log of the exponent rather
 * than being exponentiated. It is also exact for the cases that ought to be
 * exact: pow(2, 10) is 1024 and not 1023.9999999999998.
 *
 * The special cases below are the ones C specifies, and they are not
 * decoration: pow(-1, inf) is 1, and reaching exp(y*log(-1)) would give a NaN.
 */

static double pow_integer(double x, long long n)
{
    double result = 1.0;
    double base = x;
    unsigned long long e = (n < 0) ? (unsigned long long)(-(n + 1)) + 1
                                   : (unsigned long long)n;
    while (e != 0) {
        if (e & 1)
            result *= base;
        base *= base;
        e >>= 1;
    }
    return (n < 0) ? 1.0 / result : result;
}

double pow(double x, double y)
{
    /* Ordered so that the cases which are defined even for a NaN argument are
     * tested before the NaN check that would otherwise swallow them. */
    if (y == 0.0)                return 1.0;      /* including pow(nan, 0) */
    if (x == 1.0)                return 1.0;      /* including pow(1, nan) */
    if (isnan(x) || isnan(y))    return NAN;

    if (isinf(y)) {
        const double a = fabs(x);
        if (a == 1.0) return 1.0;                 /* pow(-1, +-inf) is 1 */
        if ((a > 1.0) == (y > 0.0)) return HUGE_VAL;
        return 0.0;
    }

    const double ny = fabs(y);
    const int y_is_integer = (ny < 9.2e18) && (trunc(y) == y);
    const int y_is_odd_integer = y_is_integer && (fmod(fabs(y), 2.0) == 1.0);

    if (x == 0.0) {
        /* Zero to a negative power is a pole; the sign follows x's when the
         * exponent is an odd integer, and is positive otherwise. */
        const int negative = signbit(x) && y_is_odd_integer;
        if (y < 0.0) return negative ? -HUGE_VAL : HUGE_VAL;
        return negative ? -0.0 : 0.0;
    }
    if (isinf(x)) {
        const int negative = signbit(x) && y_is_odd_integer;
        if (y > 0.0) return negative ? -HUGE_VAL : HUGE_VAL;
        return negative ? -0.0 : 0.0;
    }

    if (x < 0.0 && !y_is_integer)
        return NAN;                     /* no real root to return */

    /* Whole exponents small enough to be worth the loop: 4096 multiplications
     * at the very worst, and twelve for a typical one. */
    if (y_is_integer && ny <= 4096.0)
        return pow_integer(x, (long long)y);

    /* y * log|x|, carried as a leading term plus the residual so that neither
     * the logarithm's last bit nor the product's rounding is exponentiated. */
    double log_hi, log_lo;
    log_extended(fabs(x), &log_hi, &log_lo);

    double product, dropped;
    two_product(y, log_hi, &product, &dropped);
    const double correction = dropped + y * log_lo;

    double magnitude;
    if (fabs(product) > 800.0 || fabs(correction) > 1e-3) {
        /* Outside the range where the correction is a small factor - which is
         * to say the answer already overflows or underflows, or y is large
         * enough that the splitting above would not have been exact anyway. */
        magnitude = exp(y * (log_hi + log_lo));
    } else {
        /* exp(a+b) = exp(a) * exp(b), and for a b this small exp(b) is its own
         * first three terms with room to spare. */
        magnitude = exp(product) *
                    (1.0 + correction * (1.0 + correction * 0.5));
    }
    return (x < 0.0 && y_is_odd_integer) ? -magnitude : magnitude;
}
