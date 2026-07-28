#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>

_Noreturn void exit(int status);

/* A bump allocator over a fixed arena for now: malloc never reclaims and free
 * is a no-op. It is enough for programs that allocate a bounded amount, and it
 * will be replaced once the kernel offers a brk/mmap to grow the heap. */
void* malloc(size_t size);
void  free(void* pointer);
void* calloc(size_t count, size_t size);

/* Minimal decimal parse; no sign, no errors. Enough for a command line. */
int atoi_simple(const char* text);

#endif /* _STDLIB_H */
