#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// A tiny formatter shared by printf and the snprintf family. It writes into a
// caller-supplied sink so the same digit-emitting code serves both the
// "format straight to the write syscall" and "format into a buffer" cases.

typedef struct {
    char*  buffer;      // NULL means "count only", used by printf's sizing
    size_t capacity;
    size_t length;
    FILE*  stream;      // when set, characters go here instead of the buffer
} Sink;

static void sink_putc(Sink* sink, char c)
{
    if (sink->stream != NULL) {
        // Straight into the stream's buffer. No length limit, which is the
        // whole reason fprintf can print something longer than any array a
        // caller happens to have.
        fputc((unsigned char)c, sink->stream);
        ++sink->length;
        return;
    }
    if (sink->buffer != NULL && sink->length + 1 < sink->capacity)
        sink->buffer[sink->length] = c;
    ++sink->length;
}

static void sink_puts(Sink* sink, const char* str)
{
    while (*str != '\0')
        sink_putc(sink, *str++);
}

static void sink_unsigned(Sink* sink, unsigned long value, unsigned base,
                          int upper, unsigned min_width, char pad)
{
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];
    unsigned n = 0;
    do {
        tmp[n++] = digits[value % base];
        value /= base;
    } while (value != 0);

    for (unsigned i = n; i < min_width; ++i)
        sink_putc(sink, pad);
    while (n > 0)
        sink_putc(sink, tmp[--n]);
}

static void sink_signed(Sink* sink, long value, unsigned min_width, char pad)
{
    if (value < 0) {
        sink_putc(sink, '-');
        sink_unsigned(sink, (unsigned long)(-(value + 1)) + 1, 10, 0,
                      min_width > 0 ? min_width - 1 : 0, pad);
        return;
    }
    sink_unsigned(sink, (unsigned long)value, 10, 0, min_width, pad);
}

/* --- floating point ---------------------------------------------------------
 *
 * There is a floating-point unit now, so printf has to be able to show what it
 * produces. A number that can only be computed and not printed is half a
 * feature, and the usual workaround - scaling by a thousand and printing an
 * integer - is exactly the sort of thing having doubles was meant to stop.
 *
 * Deliberately not correctly-rounded. Printing a double back at its shortest
 * exact representation is a genuinely hard problem (Steele and White, Grisu,
 * Ryu) and the answer is thousands of lines. This scales, rounds once and
 * emits digits, which is right to the precision asked for and good enough for
 * every use this system has. Exponents beyond a double's range are handled by
 * falling back to the exponential form rather than by overflowing an integer.
 */

static int f_is_nan(double v)  { return v != v; }
static int f_is_inf(double v)  { return v != 0.0 && v * 0.5 == v; }

static double f_pow10(int n)
{
    double r = 1.0;
    /* Repeated multiplication, not a table: the table would be the same
     * rounding either way, and this is called once per conversion. */
    while (n >= 8) { r *= 1e8; n -= 8; }
    while (n > 0)  { r *= 10.0; --n; }
    while (n <= -8) { r /= 1e8; n += 8; }
    while (n < 0)  { r /= 10.0; ++n; }
    return r;
}

/* The fixed form. `value` must already be non-negative and finite. */
static void sink_fixed(Sink* sink, double value, unsigned prec)
{
    /* Round once, at the digit being printed, so 9.999 at two places becomes
     * 10.00 and not 9.100 - the carry has to reach the integer part. */
    value += 0.5 * f_pow10(-(int)prec);

    unsigned long whole = (unsigned long)value;
    sink_unsigned(sink, whole, 10, 0, 0, ' ');
    if (prec == 0)
        return;

    sink_putc(sink, '.');
    double frac = value - (double)whole;
    for (unsigned i = 0; i < prec; ++i) {
        frac *= 10.0;
        int digit = (int)frac;
        if (digit < 0) digit = 0;
        if (digit > 9) digit = 9;
        sink_putc(sink, (char)('0' + digit));
        frac -= (double)digit;
    }
}

/* The exponential form, and the fallback whenever the fixed one would need an
 * integer part larger than an unsigned long can hold. */
static void sink_exponential(Sink* sink, double value, unsigned prec, char e)
{
    int exponent = 0;
    if (value != 0.0) {
        while (value >= 10.0) { value /= 10.0; ++exponent; }
        while (value < 1.0)   { value *= 10.0; --exponent; }
    }
    /* Rounding the mantissa can carry it to 10.0, which is one digit too many. */
    if (value + 0.5 * f_pow10(-(int)prec) >= 10.0) {
        value /= 10.0;
        ++exponent;
    }
    sink_fixed(sink, value, prec);
    sink_putc(sink, e);
    sink_putc(sink, exponent < 0 ? '-' : '+');
    if (exponent < 0)
        exponent = -exponent;
    sink_unsigned(sink, (unsigned long)exponent, 10, 0, 2, '0');
}

/* Anything a double can be, in whichever of the two forms was asked for.
 * `spec` is 'f', 'e' or 'g'; 'g' picks between them the way C says to. */
static void sink_double(Sink* sink, double value, unsigned prec, char spec,
                        int have_prec)
{
    /* The uppercase conversions differ only in the exponent's letter, so the
     * specifier is folded here and the letter carried alongside it. */
    const char e_char = (spec == 'E' || spec == 'G') ? 'E' : 'e';
    if (spec == 'F') spec = 'f';
    if (spec == 'E') spec = 'e';
    if (spec == 'G') spec = 'g';

    const int upper = (e_char == 'E');
    if (f_is_nan(value)) { sink_puts(sink, upper ? "NAN" : "nan"); return; }
    if (value < 0.0 || (value == 0.0 && 1.0 / value < 0.0)) {
        sink_putc(sink, '-');
        value = -value;
    }
    if (f_is_inf(value)) { sink_puts(sink, upper ? "INF" : "inf"); return; }
    if (!have_prec)
        prec = 6;

    if (spec == 'g') {
        /* C's rule: the exponential form when the value is very small or
         * needs more digits than the precision allows, and precision counts
         * significant digits rather than places after the point. */
        if (prec == 0)
            prec = 1;
        int exponent = 0;
        double scan = value;
        if (scan != 0.0) {
            while (scan >= 10.0) { scan /= 10.0; ++exponent; }
            while (scan < 1.0)   { scan *= 10.0; --exponent; }
        }

        /* %g drops trailing zeros, which means the digits cannot go straight
         * out - they have to be looked at first. Rendered into a buffer, and
         * trimmed there. 48 bytes covers the widest this can produce: a
         * seventeen-digit mantissa, a point, a sign and a three-digit
         * exponent, with room to spare. */
        char buf[48];
        Sink into = { buf, sizeof(buf), 0, NULL };
        if (exponent < -4 || exponent >= (int)prec)
            sink_exponential(&into, value, prec - 1, e_char);
        else
            sink_fixed(&into, value, prec - 1 - (unsigned)exponent);
        size_t n = into.length < sizeof(buf) - 1 ? into.length : sizeof(buf) - 1;
        buf[n] = '\0';

        /* Trim only within the mantissa: "1.2000e+05" loses four zeros, and
         * the exponent's own are not the mantissa's to drop. */
        size_t mantissa_end = n;
        for (size_t k = 0; k < n; ++k)
            if (buf[k] == 'e' || buf[k] == 'E') { mantissa_end = k; break; }
        int has_point = 0;
        for (size_t k = 0; k < mantissa_end; ++k)
            if (buf[k] == '.') { has_point = 1; break; }
        if (has_point) {
            size_t cut = mantissa_end;
            while (cut > 0 && buf[cut - 1] == '0')
                --cut;
            if (cut > 0 && buf[cut - 1] == '.')
                --cut;                  /* "3." is just "3" */
            for (size_t k = 0; k < cut; ++k)
                sink_putc(sink, buf[k]);
            for (size_t k = mantissa_end; k < n; ++k)
                sink_putc(sink, buf[k]);
        } else {
            sink_puts(sink, buf);
        }
        return;
    }

    /* 1e18 is where an unsigned long stops being able to hold the integer
     * part. Past it the fixed form cannot be produced at all, so say the same
     * number a way that works rather than printing a wrapped integer. */
    if (spec == 'e' || value >= 1e18)
        sink_exponential(sink, value, prec, e_char);
    else
        sink_fixed(sink, value, prec);
}


static void format(Sink* sink, const char* fmt, va_list args)
{
    for (size_t i = 0; fmt[i] != '\0'; ++i) {
        if (fmt[i] != '%') {
            sink_putc(sink, fmt[i]);
            continue;
        }

        ++i;
        /* Flags. An unrecognised one used to fall straight through to the
         * specifier switch, which then failed to consume its argument - and
         * from there every later argument in the call was off by one. Silently
         * mis-parsing a standard flag is a worse failure than printing it
         * literally, so left-justify is handled rather than ignored. */
        int left = 0;
        char pad = ' ';
        for (;;) {
            if (fmt[i] == '-') {
                left = 1;
                ++i;
            } else if (fmt[i] == '0') {
                pad = '0';
                ++i;
            } else {
                break;
            }
        }

        unsigned width = 0;
        if (fmt[i] == '*') {
            /* The width comes from the arguments. A negative one means left,
             * which is what printf has always done with it. */
            ++i;
            const int given = va_arg(args, int);
            if (given < 0) { left = 1; width = (unsigned)(-given); }
            else           { width = (unsigned)given; }
        } else {
            while (fmt[i] >= '0' && fmt[i] <= '9')
                width = width * 10 + (unsigned)(fmt[i++] - '0');
        }

        /* Precision. Only the conversions below that read it do anything with
         * it, but it is parsed whatever the specifier turns out to be: an
         * unconsumed ".2" would be printed literally and every argument after
         * it would still be in the wrong place. */
        unsigned prec = 0;
        int have_prec = 0;
        if (fmt[i] == '.') {
            ++i;
            have_prec = 1;
            if (fmt[i] == '*') {
                ++i;
                const int given = va_arg(args, int);
                prec = given < 0 ? 0 : (unsigned)given;
            } else {
                while (fmt[i] >= '0' && fmt[i] <= '9')
                    prec = prec * 10 + (unsigned)(fmt[i++] - '0');
            }
        }

        int wide = 0;
        while (fmt[i] == 'l') {
            wide = 1;
            ++i;
        }

        switch (fmt[i]) {
        case 'd':
        case 'i':
            sink_signed(sink, wide ? va_arg(args, long) : va_arg(args, int), width, pad);
            break;
        case 'u':
            sink_unsigned(sink, wide ? va_arg(args, unsigned long) : va_arg(args, unsigned int),
                          10, 0, width, pad);
            break;
        case 'o':
            sink_unsigned(sink, wide ? va_arg(args, unsigned long) : va_arg(args, unsigned int),
                          8, 0, width, pad);
            break;
        case 'x':
            sink_unsigned(sink, wide ? va_arg(args, unsigned long) : va_arg(args, unsigned int),
                          16, 0, width, pad);
            break;
        case 'X':
            sink_unsigned(sink, wide ? va_arg(args, unsigned long) : va_arg(args, unsigned int),
                          16, 1, width, pad);
            break;
        case 'p':
            sink_puts(sink, "0x");
            sink_unsigned(sink, (unsigned long)va_arg(args, void*), 16, 0, 16, '0');
            break;
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G': {
            /* Both float and double arrive as double: the default argument
             * promotions say so, and there is no way to ask for the float. */
            const double v = va_arg(args, double);
            const char spec = fmt[i];
            /* Width is applied by measuring first, which costs a second pass
             * over a handful of digits and keeps the padding in one place. */
            if (width > 0) {
                Sink measure = { NULL, 0, 0, NULL };
                sink_double(&measure, v, prec, spec, have_prec);
                if (!left)
                    for (size_t k = measure.length; k < width; ++k)
                        sink_putc(sink, pad);
                sink_double(sink, v, prec, spec, have_prec);
                if (left)
                    for (size_t k = measure.length; k < width; ++k)
                        sink_putc(sink, ' ');
            } else {
                sink_double(sink, v, prec, spec, have_prec);
            }
            break;
        }
        case 'c':
            sink_putc(sink, (char)va_arg(args, int));
            break;
        case 's': {
            const char* s = va_arg(args, const char*);
            if (s == NULL)
                s = "(null)";
            unsigned length = 0;
            while (s[length] != '\0')
                ++length;
            /* A precision on a string is a maximum, and the string need not be
             * terminated within it - which is the whole reason %.*s is used to
             * print a slice of something longer. */
            if (have_prec && prec < length)
                length = prec;
            if (!left) {
                for (unsigned k = length; k < width; ++k)
                    sink_putc(sink, ' ');
            }
            /* Exactly `length` characters, not the whole string: with a
             * precision the two differ, and that is the point of one. */
            for (unsigned k = 0; k < length; ++k)
                sink_putc(sink, s[k]);
            if (left) {
                for (unsigned k = length; k < width; ++k)
                    sink_putc(sink, ' ');
            }
            break;
        }
        case '%':
            sink_putc(sink, '%');
            break;
        default:
            sink_putc(sink, '%');
            sink_putc(sink, fmt[i]);
            break;
        }
    }
}

int vsnprintf(char* buffer, size_t size, const char* fmt, va_list args)
{
    Sink sink = { buffer, size, 0, NULL };
    format(&sink, fmt, args);
    if (buffer != NULL && size > 0)
        buffer[sink.length < size ? sink.length : size - 1] = '\0';
    return (int)sink.length;
}

int snprintf(char* buffer, size_t size, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(buffer, size, fmt, args);
    va_end(args);
    return n;
}

int vfprintf(FILE* stream, const char* fmt, va_list args)
{
    Sink sink = { NULL, 0, 0, stream };
    format(&sink, fmt, args);
    return (int)sink.length;
}

int fprintf(FILE* stream, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    const int n = vfprintf(stream, fmt, args);
    va_end(args);
    return n;
}

int printf(const char* fmt, ...)
{
    /* Through the stream rather than straight to the descriptor. It used to
     * format into a 512-byte array and write that, which silently truncated
     * anything longer and interleaved badly with fputs - two ways of writing
     * to the same descriptor, each with its own idea of what had gone out. */
    va_list args;
    va_start(args, fmt);
    const int n = vfprintf(stdout, fmt, args);
    va_end(args);
    return n;
}

/* --- reading a formatted string ---------------------------------------------
 *
 * The other direction, and the one that was missing: mount and df both split
 * /proc/mounts by hand because there was nothing to ask. Enough of scanf to be
 * worth having - %d %i %u %x %o %c %s %f %e %g %%, a width, and * to match a
 * field without storing it - and not the parts nobody uses.
 *
 * Whitespace in the format matches any run of whitespace including none, and
 * every conversion but %c and %[ skips leading whitespace first. Those are the
 * two rules that make scanf formats behave the way people expect, and getting
 * either wrong makes every format subtly wrong.
 */

static int is_space(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '\f' || c == '\v';
}

int vsscanf(const char* text, const char* format, va_list args)
{
    const char* in = text;
    int assigned = 0;

    for (const char* f = format; *f != '\0'; ++f) {
        if (is_space(*f)) {
            while (is_space(*in))
                ++in;
            continue;
        }
        if (*f != '%') {
            if (*in != *f)
                return assigned;        /* the text stopped matching */
            ++in;
            continue;
        }

        ++f;
        if (*f == '%') {
            if (*in != '%')
                return assigned;
            ++in;
            continue;
        }

        int skip = 0;
        unsigned width = 0;
        if (*f == '*') { skip = 1; ++f; }
        while (*f >= '0' && *f <= '9')
            width = width * 10 + (unsigned)(*f++ - '0');
        while (*f == 'l' || *f == 'h')  /* accepted; everything here is int */
            ++f;
        const char kind = *f;
        if (kind == '\0')
            break;

        if (kind != 'c')
            while (is_space(*in))
                ++in;
        if (*in == '\0')
            return assigned > 0 ? assigned : -1;    /* -1 is end of input */

        switch (kind) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
            const int base = (kind == 'x' || kind == 'X') ? 16
                           : kind == 'o' ? 8 : 10;
            const char* start = in;
            int negative = 0;
            if (*in == '+' || *in == '-')
                negative = *in++ == '-';
            long value = 0;
            unsigned digits = 0;
            for (;;) {
                int d;
                const char c = *in;
                if (c >= '0' && c <= '9')                    d = c - '0';
                else if (base == 16 && c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (base == 16 && c >= 'A' && c <= 'F') d = c - 'A' + 10;
                else break;
                if (d >= base)
                    break;
                if (width != 0 && (unsigned)(in - start) >= width)
                    break;
                value = value * base + d;
                ++in;
                ++digits;
            }
            if (digits == 0) {
                in = start;
                return assigned;
            }
            if (negative)
                value = -value;
            if (!skip) {
                *va_arg(args, int*) = (int)value;
                ++assigned;
            }
            break;
        }
        case 'f': case 'e': case 'E': case 'g': case 'G': {
            char* stop = 0;
            const double value = strtod(in, &stop);
            if (stop == in)
                return assigned;
            in = stop;
            if (!skip) {
                *va_arg(args, double*) = value;
                ++assigned;
            }
            break;
        }
        case 'c': {
            const unsigned many = width != 0 ? width : 1;
            char* out = skip ? 0 : va_arg(args, char*);
            for (unsigned k = 0; k < many; ++k) {
                if (*in == '\0')
                    return assigned;
                if (out != 0)
                    out[k] = *in;
                ++in;
            }
            if (out != 0)
                ++assigned;
            break;
        }
        case 's': {
            char* out = skip ? 0 : va_arg(args, char*);
            unsigned n = 0;
            while (*in != '\0' && !is_space(*in) &&
                   (width == 0 || n < width)) {
                if (out != 0)
                    out[n] = *in;
                ++n;
                ++in;
            }
            if (n == 0)
                return assigned;
            if (out != 0) {
                out[n] = '\0';
                ++assigned;
            }
            break;
        }
        default:
            return assigned;            /* a conversion this does not know */
        }
    }
    return assigned;
}

int sscanf(const char* text, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    const int n = vsscanf(text, format, args);
    va_end(args);
    return n;
}
