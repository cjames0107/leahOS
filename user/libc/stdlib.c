#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/syscall.h>

_Noreturn void exit(int status)
{
    /* Anything still sitting in a stream's buffer goes out first. Without
     * this, a program that printed without a trailing newline produced
     * nothing at all: stdout is line buffered, main returned, crt0 called
     * here, and the buffer went with the address space. That was invisible
     * until printf started going through the streams. */
    fflush(0);
    __syscall(SYS_exit, status, 0, 0, 0, 0);
    __builtin_unreachable();
}

/* A bump allocator over sbrk. The break grows a page at a time as demand
 * exceeds it, contiguously, so no space is wasted between growths. free is a
 * no-op - real reclamation is a job for a later allocator; this is enough for
 * programs that allocate a bounded amount. */
static char* g_top = 0;      /* next allocation */
static char* g_end = 0;      /* current program break */

void* malloc(size_t size)
{
    if (g_top == 0)
        g_top = g_end = (char*)sbrk(0);

    size = (size + 15) & ~(size_t)15;   /* 16-byte alignment */

    if (g_top + size > g_end) {
        size_t need = (size + 4095) & ~(size_t)4095;
        if (need < 65536)
            need = 65536;
        if (sbrk((long)need) == (void*)-1)
            return NULL;
        g_end += need;
    }

    void* block = g_top;
    g_top += size;
    return block;
}

void free(void* pointer)
{
    (void)pointer;
}

void* calloc(size_t count, size_t size)
{
    const size_t total = count * size;
    /* Guard against the multiply wrapping around. */
    if (size != 0 && total / size != count)
        return NULL;
    void* block = malloc(total);
    if (block != NULL)
        memset(block, 0, total);
    return block;
}

int atoi_simple(const char* text)
{
    int value = 0;
    for (; *text >= '0' && *text <= '9'; ++text)
        value = value * 10 + (*text - '0');
    return value;
}

/* Text to a double.
 *
 * Digits are accumulated as an integer and scaled once at the end, rather than
 * multiplied in as they arrive: adding 0.1 nine times is not 0.9, and the same
 * error creeps into a value built a digit at a time. Scaling once means one
 * rounding rather than one per digit.
 *
 * Not correctly rounded for the hard cases - a decimal exactly half way
 * between two doubles can go either way - which needs arbitrary-precision
 * arithmetic to fix and matters to nothing here.
 */
double strtod(const char* text, char** end)
{
    const char* at = text;
    int negative = 0;

    while (*at == ' ' || *at == '\t' || *at == '\n' || *at == '\r')
        ++at;
    if (*at == '+' || *at == '-')
        negative = *at++ == '-';

    double mantissa = 0.0;
    int digits = 0, exponent = 0;
    for (; *at >= '0' && *at <= '9'; ++at) {
        mantissa = mantissa * 10.0 + (double)(*at - '0');
        ++digits;
    }
    if (*at == '.') {
        ++at;
        for (; *at >= '0' && *at <= '9'; ++at) {
            mantissa = mantissa * 10.0 + (double)(*at - '0');
            ++digits;
            --exponent;
        }
    }
    if (digits == 0) {
        if (end != 0)
            *end = (char*)text;         /* nothing that looked like a number */
        return 0.0;
    }

    if (*at == 'e' || *at == 'E') {
        const char* mark = at++;
        int esign = 0, value = 0, edigits = 0;
        if (*at == '+' || *at == '-')
            esign = *at++ == '-';
        for (; *at >= '0' && *at <= '9'; ++at) {
            if (value < 100000)
                value = value * 10 + (*at - '0');
            ++edigits;
        }
        if (edigits == 0)
            at = mark;                  /* an e with no exponent is not ours */
        else
            exponent += esign ? -value : value;
    }

    /* Scaled by repeated squaring, so a large exponent costs a handful of
     * multiplications rather than one per power of ten. */
    double scale = 1.0, base = 10.0;
    int power = exponent < 0 ? -exponent : exponent;
    while (power > 0) {
        if ((power & 1) != 0)
            scale *= base;
        base *= base;
        power >>= 1;
    }
    double result = exponent < 0 ? mantissa / scale : mantissa * scale;

    if (end != 0)
        *end = (char*)at;
    return negative ? -result : result;
}
