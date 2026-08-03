#ifndef _MATH_H
#define _MATH_H

/* Arithmetic beyond what the hardware does in one instruction.
 *
 * The processor gives us add, subtract, multiply, divide and square root, and
 * that is the whole list. Everything else here is built from them: sine and
 * cosine from a polynomial after the angle is folded into a quarter turn,
 * exponential and logarithm from polynomials after the number is split into a
 * power of two and a remainder, and pow from those two.
 *
 * These are accurate to within a few units in the last place, not correctly
 * rounded. A correctly-rounded library has to decide, for every input, which
 * of two adjacent doubles is nearer the true answer - and near a tie that can
 * need hundreds of bits of intermediate precision. That is a different and
 * much larger project (crlibm, and the table-maker's dilemma behind it). What
 * is here is the accuracy that graphics, audio and measurement want.
 *
 * Measured against a known-good libm, worst case over the ranges tested:
 *
 *     sqrt                     0 ulp   (the hardware's, correctly rounded)
 *     exp                      1
 *     sin, cos, log, log2      2
 *     tan                      3
 *     pow, fractional exponent 5
 *     pow, whole exponent     11
 *
 * A whole-number exponent goes through repeated squaring rather than through
 * exp(y*log(x)). That is the larger error of the two and it is the right
 * trade: repeated squaring is exact whenever the answer is representable, so
 * pow(10, 10) is ten billion. Through the logarithm it came out
 * 9999999999.9999981, which is a smaller error by every measure except the
 * one that matters when somebody prints it.
 *
 * Where accuracy falls off is stated rather than hidden. sin and cos fold the
 * angle using a three-part split of pi/2, which holds to full precision for
 * |x| up to about 2^20 - beyond that the fold itself loses bits, and by 2^40
 * the result is not worth having. Doing better needs Payne-Hanek reduction
 * against a many-digit 2/pi, which is not here.
 */

/* --- constants ------------------------------------------------------------ */

#define M_E        2.71828182845904523536
#define M_LOG2E    1.44269504088896340736
#define M_LOG10E   0.43429448190325182765
#define M_LN2      0.69314718055994530942
#define M_LN10     2.30258509299404568402
#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_PI_4     0.78539816339744830962
#define M_1_PI     0.31830988618379067154
#define M_2_PI     0.63661977236758134308
#define M_SQRT2    1.41421356237309504880
#define M_SQRT1_2  0.70710678118654752440

/* Built rather than written out: a literal 1e400 is a constant the compiler
 * has to diagnose, and 0.0/0.0 is a division it is entitled to fold into a
 * diagnostic too. __builtin_ forms name the values directly. */
#define INFINITY   (__builtin_inff())
#define NAN        (__builtin_nanf(""))
#define HUGE_VAL   (__builtin_inf())

/* --- classification -------------------------------------------------------- */

/* A NaN is the only value that is not equal to itself, which is the whole
 * test. An infinity is what survives being halved. */
int isnan(double x);
int isinf(double x);
int isfinite(double x);
int signbit(double x);

/* --- the pieces the rest is built from -------------------------------------- */

double fabs(double x);
double sqrt(double x);          /* one instruction, and correctly rounded */

double floor(double x);
double ceil(double x);
double trunc(double x);
double round(double x);         /* halfway cases away from zero */
double fmod(double x, double y);

/* Splitting a double into a mantissa and a power of two, and putting one back
 * together. Exact in both directions: these only move the exponent. */
double frexp(double x, int* exponent);
double ldexp(double x, int exponent);

/* --- what was asked for ------------------------------------------------------ */

double exp(double x);
double log(double x);           /* natural; negative x is a NaN */
double log2(double x);
double log10(double x);
double pow(double x, double y);

double sin(double x);
double cos(double x);
double tan(double x);

/* --- float forms ------------------------------------------------------------- */
/* Computed as doubles and rounded once at the end. Genuinely single-precision
 * kernels would be faster and are not worth two implementations here. */

static inline float fabsf(float x)          { return (float)fabs(x); }
static inline float sqrtf(float x)          { return (float)sqrt(x); }
static inline float floorf(float x)         { return (float)floor(x); }
static inline float ceilf(float x)          { return (float)ceil(x); }
static inline float expf(float x)           { return (float)exp(x); }
static inline float logf(float x)           { return (float)log(x); }
static inline float powf(float x, float y)  { return (float)pow(x, y); }
static inline float sinf(float x)           { return (float)sin(x); }
static inline float cosf(float x)           { return (float)cos(x); }
static inline float tanf(float x)           { return (float)tan(x); }

#endif /* _MATH_H */
