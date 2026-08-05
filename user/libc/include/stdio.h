#ifndef _STDIO_H
#define _STDIO_H

#include <stddef.h>
#include <stdarg.h>

/* Buffered input and output.
 *
 * Every program here used to do its own: a raw read() into its own array, its
 * own scan for newlines, its own partial-line state. I wrote that same loop
 * four times in one afternoon - in wc, head, sort and less - and each copy had
 * its own idea of what a line longer than the buffer meant.
 *
 * A FILE is a descriptor, a buffer, and where we are in it. That is all it has
 * ever been, and it is the reason fgets exists.
 *
 * Buffering follows the usual rule, for the usual reason: a file in full
 * because nobody is watching it, a terminal by the line because somebody is,
 * and standard error not at all - the message that matters most is the one
 * printed just before a crash.
 */

typedef struct FILE FILE;

extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;

#define EOF (-1)
#define BUFSIZ 4096

/* setvbuf modes. */
#define _IOFBF 0        /* fully buffered */
#define _IOLBF 1        /* line buffered */
#define _IONBF 2        /* not buffered */

/* fseek origins - the same three lseek uses. */
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

/* Modes: "r", "w", "a", and any of those with "+" for both directions. A "b"
 * is accepted and ignored, there being no text mode for it to differ from. */
FILE* fopen(const char* path, const char* mode);

/* Wrap a descriptor this program already has. fclose still closes it, which is
 * what every other UNIX does and what callers expect. */
FILE* fdopen(int fd, const char* mode);

int   fclose(FILE* stream);
int   fflush(FILE* stream);     /* null flushes every stream */

size_t fread(void* into, size_t size, size_t count, FILE* stream);
size_t fwrite(const void* from, size_t size, size_t count, FILE* stream);

int   fgetc(FILE* stream);
int   getc(FILE* stream);
int   getchar(void);
int   ungetc(int c, FILE* stream);

/* Up to `max - 1` characters, stopping after a newline, which is kept.
 * Returns `into`, or null at the end of the file with nothing read. */
char* fgets(char* into, int max, FILE* stream);

int   fputc(int c, FILE* stream);
int   putc(int c, FILE* stream);
int   putchar(int c);
int   fputs(const char* text, FILE* stream);
int   puts(const char* text);   /* adds a newline; fputs does not */

int   fprintf(FILE* stream, const char* format, ...)
      __attribute__((format(printf, 2, 3)));
int   vfprintf(FILE* stream, const char* format, va_list args);

long  fseek(FILE* stream, long offset, int whence);
long  ftell(FILE* stream);
void  rewind(FILE* stream);

int   feof(FILE* stream);
int   ferror(FILE* stream);
void  clearerr(FILE* stream);
int   fileno(FILE* stream);

int   setvbuf(FILE* stream, char* buffer, int mode, size_t size);

/* Supports %s %c %% and %d %i %u %x %p %f %e %g, with an optional width, a
 * precision, and the l/ll length modifiers. */
int printf(const char* format, ...) __attribute__((format(printf, 1, 2)));
int vsnprintf(char* buffer, size_t size, const char* format, va_list args);
int snprintf(char* buffer, size_t size, const char* format, ...)
    __attribute__((format(printf, 3, 4)));

#endif /* _STDIO_H */
