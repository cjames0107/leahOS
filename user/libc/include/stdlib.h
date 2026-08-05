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

/* The environment: a vector of "NAME=value", ending in a null pointer.
 *
 * `environ` can move - the first setenv copies the kernel's vector, which is on
 * the stack and cannot grow, into memory of our own. Nothing should hold a
 * copy of it across a change. */
extern char** environ;

char* getenv(const char* name);
int   setenv(const char* name, const char* value, int overwrite);
int   unsetenv(const char* name);
int   putenv(char* entry);      /* keeps the caller's string, as it always has */
int   clearenv(void);

#endif /* _STDLIB_H */
