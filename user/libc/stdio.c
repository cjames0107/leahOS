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
} Sink;

static void sink_putc(Sink* sink, char c)
{
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

static void format(Sink* sink, const char* fmt, va_list args)
{
    for (size_t i = 0; fmt[i] != '\0'; ++i) {
        if (fmt[i] != '%') {
            sink_putc(sink, fmt[i]);
            continue;
        }

        ++i;
        char pad = ' ';
        if (fmt[i] == '0') {
            pad = '0';
            ++i;
        }

        unsigned width = 0;
        while (fmt[i] >= '0' && fmt[i] <= '9')
            width = width * 10 + (unsigned)(fmt[i++] - '0');

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
        case 'c':
            sink_putc(sink, (char)va_arg(args, int));
            break;
        case 's': {
            const char* s = va_arg(args, const char*);
            sink_puts(sink, s != NULL ? s : "(null)");
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

int putchar(int c)
{
    char ch = (char)c;
    write(1, &ch, 1);
    return c;
}

int puts(const char* str)
{
    write(1, str, strlen(str));
    putchar('\n');
    return 0;
}

int vsnprintf(char* buffer, size_t size, const char* fmt, va_list args)
{
    Sink sink = { buffer, size, 0 };
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

int printf(const char* fmt, ...)
{
    // Format into a stack buffer, then hand the whole thing to one write. A
    // per-character write syscall would work but would be needlessly chatty.
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    const size_t len = (size_t)n < sizeof(buffer) - 1 ? (size_t)n : sizeof(buffer) - 1;
    write(1, buffer, len);
    return n;
}
