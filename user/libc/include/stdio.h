#ifndef _STDIO_H
#define _STDIO_H

#include <stddef.h>
#include <stdarg.h>

int putchar(int c);
int puts(const char* str);

/* Supports %s %c %% and %d %i %u %x %p, with an optional zero-padded width
 * (e.g. %08x) and the l/ll length modifiers. Writes to stdout via the write
 * syscall - there are no FILE streams yet. */
int printf(const char* format, ...) __attribute__((format(printf, 1, 2)));
int vsnprintf(char* buffer, size_t size, const char* format, va_list args);
int snprintf(char* buffer, size_t size, const char* format, ...)
    __attribute__((format(printf, 3, 4)));

#endif /* _STDIO_H */
