/* FILE streams.
 *
 * A descriptor, a buffer, and where we are in it. The whole point is that a
 * program asking for one line does not have to know that the disk deals in
 * blocks, and that a program writing one character does not cause a syscall.
 *
 * One buffer serves both directions rather than two. A stream is reading or
 * writing at any moment, never both at once, and switching between them
 * flushes - which is exactly the rule C states for a "+" mode and the reason
 * it states it.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MODE_READ  1
#define MODE_WRITE 2

struct FILE {
    int   fd;
    int   flags;            /* MODE_READ | MODE_WRITE */
    int   buffering;        /* _IOFBF, _IOLBF, _IONBF */

    unsigned char* buffer;
    size_t         size;
    size_t         at;      /* how far into the buffer we are */
    size_t         held;    /* how much of it is real, when reading */

    int   writing;          /* the buffer holds output rather than input */
    int   at_end;
    int   failed;
    int   pushed;           /* ungetc's one character, or -1 */
    int   owns_buffer;
    int   in_use;
};

/* A fixed set rather than malloc: the number of streams a program here has is
 * small and known, and a table makes fflush(0) at exit possible without a list
 * to walk. */
#define STREAM_MAX 16
static struct FILE g_streams[STREAM_MAX];
static unsigned char g_standard_buffers[3][BUFSIZ];

static struct FILE g_stdin, g_stdout, g_stderr;
FILE* stdin  = &g_stdin;
FILE* stdout = &g_stdout;
FILE* stderr = &g_stderr;

static int g_ready;

static void set_up(struct FILE* s, int fd, int flags, int buffering,
                   unsigned char* buffer)
{
    memset(s, 0, sizeof(*s));
    s->fd = fd;
    s->flags = flags;
    s->buffering = buffering;
    s->buffer = buffer;
    s->size = BUFSIZ;
    s->pushed = -1;
    s->in_use = 1;
}

static void start(void)
{
    if (g_ready)
        return;
    g_ready = 1;
    /* Standard output is line buffered rather than block buffered because a
     * terminal is somebody watching. Nothing here can ask whether a descriptor
     * is a terminal - there is no isatty - so the choice is made by which
     * descriptor it is, which is right far more often than it is wrong. */
    set_up(&g_stdin,  0, MODE_READ,  _IOLBF, g_standard_buffers[0]);
    set_up(&g_stdout, 1, MODE_WRITE, _IOLBF, g_standard_buffers[1]);
    set_up(&g_stderr, 2, MODE_WRITE, _IONBF, g_standard_buffers[2]);
}

/* --- the two directions ------------------------------------------------------ */

/* Push whatever is buffered for output at the descriptor. */
static int drain(struct FILE* s)
{
    if (!s->writing || s->at == 0)
        return 0;
    size_t done = 0;
    while (done < s->at) {
        const long n = write(s->fd, s->buffer + done, s->at - done);
        if (n <= 0) {
            s->failed = 1;
            /* What did not go out is dropped: keeping it would mean a later
             * flush retrying a write that already failed once, and the caller
             * has been told through ferror either way. */
            s->at = 0;
            return EOF;
        }
        done += (size_t)n;
    }
    s->at = 0;
    return 0;
}

/* Fill the buffer from the descriptor. Returns how much arrived. */
static size_t fill(struct FILE* s)
{
    if (s->writing) {
        if (drain(s) != 0)
            return 0;
        s->writing = 0;
    }
    s->at = s->held = 0;
    const long n = read(s->fd, s->buffer, s->size);
    if (n < 0) {
        s->failed = 1;
        return 0;
    }
    if (n == 0) {
        s->at_end = 1;
        return 0;
    }
    s->held = (size_t)n;
    return s->held;
}

static int to_writing(struct FILE* s)
{
    if (!s->writing) {
        /* Anything read ahead of where the caller thinks it is has to be given
         * back, or the write lands past it. */
        if (s->held > s->at)
            lseek(s->fd, -(long)(s->held - s->at), SEEK_CUR);
        s->at = s->held = 0;
        s->writing = 1;
    }
    return 0;
}

/* --- opening and closing ------------------------------------------------------ */

static int flags_from_mode(const char* mode, int* open_flags)
{
    if (mode == 0 || mode[0] == '\0') {
        errno = EINVAL;
        return 0;
    }
    int plus = 0;
    for (const char* m = mode + 1; *m != '\0'; ++m)
        if (*m == '+')
            plus = 1;

    switch (mode[0]) {
    case 'r':
        *open_flags = plus ? (O_RDONLY | O_WRONLY) : O_RDONLY;
        return plus ? (MODE_READ | MODE_WRITE) : MODE_READ;
    case 'w':
        *open_flags = O_WRONLY | O_CREAT | O_TRUNC;
        return plus ? (MODE_READ | MODE_WRITE) : MODE_WRITE;
    case 'a':
        *open_flags = O_WRONLY | O_CREAT | O_APPEND;
        return plus ? (MODE_READ | MODE_WRITE) : MODE_WRITE;
    default:
        errno = EINVAL;
        return 0;
    }
}

static struct FILE* free_stream(void)
{
    for (int i = 0; i < STREAM_MAX; ++i)
        if (!g_streams[i].in_use)
            return &g_streams[i];
    errno = EMFILE;
    return 0;
}

FILE* fdopen(int fd, const char* mode)
{
    start();
    int open_flags = 0;
    const int flags = flags_from_mode(mode, &open_flags);
    if (flags == 0)
        return 0;

    struct FILE* s = free_stream();
    if (s == 0)
        return 0;
    unsigned char* buffer = (unsigned char*)malloc(BUFSIZ);
    if (buffer == 0) {
        errno = ENOMEM;
        return 0;
    }
    set_up(s, fd, flags, _IOFBF, buffer);
    s->owns_buffer = 1;
    return s;
}

FILE* fopen(const char* path, const char* mode)
{
    start();
    int open_flags = 0;
    const int flags = flags_from_mode(mode, &open_flags);
    if (flags == 0)
        return 0;

    const int fd = open(path, open_flags);
    if (fd < 0)
        return 0;                       /* open set errno */

    FILE* s = fdopen(fd, mode);
    if (s == 0) {
        const int saved = errno;
        close(fd);
        errno = saved;                  /* close must not overwrite the reason */
        return 0;
    }
    return s;
}

int fclose(FILE* s)
{
    start();
    if (s == 0) {
        errno = EBADF;
        return EOF;
    }
    const int flushed = fflush(s);
    const int closed = close(s->fd);
    s->in_use = 0;
    /* Standard streams keep their static buffers; the rest gave them back to
     * an allocator whose free is a no-op, so this only marks them unused. */
    if (s->owns_buffer)
        s->buffer = 0;
    return (flushed != 0 || closed != 0) ? EOF : 0;
}

int fflush(FILE* s)
{
    start();
    if (s != 0)
        return drain(s);
    /* Null means all of them, which is what a program does before it exits. */
    int worst = 0;
    if (drain(&g_stdout) != 0) worst = EOF;
    if (drain(&g_stderr) != 0) worst = EOF;
    for (int i = 0; i < STREAM_MAX; ++i)
        if (g_streams[i].in_use && drain(&g_streams[i]) != 0)
            worst = EOF;
    return worst;
}

/* --- reading ------------------------------------------------------------------ */

int fgetc(FILE* s)
{
    start();
    if (s == 0 || (s->flags & MODE_READ) == 0) {
        errno = EBADF;
        return EOF;
    }
    if (s->pushed >= 0) {
        const int c = s->pushed;
        s->pushed = -1;
        return c;
    }
    if (s->at >= s->held && fill(s) == 0)
        return EOF;
    return s->buffer[s->at++];
}

int getc(FILE* s)     { return fgetc(s); }
int getchar(void)     { start(); return fgetc(stdin); }

int ungetc(int c, FILE* s)
{
    start();
    /* One character, which is all C promises and all anything here needs. */
    if (s == 0 || c == EOF || s->pushed >= 0)
        return EOF;
    s->pushed = c & 0xFF;
    s->at_end = 0;
    return s->pushed;
}

size_t fread(void* into, size_t size, size_t count, FILE* s)
{
    start();
    if (s == 0 || into == 0 || size == 0)
        return 0;
    unsigned char* out = (unsigned char*)into;
    size_t want = size * count, done = 0;

    while (done < want) {
        if (s->pushed >= 0) {
            out[done++] = (unsigned char)s->pushed;
            s->pushed = -1;
            continue;
        }
        if (s->at >= s->held && fill(s) == 0)
            break;
        size_t n = s->held - s->at;
        if (n > want - done)
            n = want - done;
        memcpy(out + done, s->buffer + s->at, n);
        s->at += n;
        done += n;
    }
    return done / size;
}

char* fgets(char* into, int max, FILE* s)
{
    start();
    if (into == 0 || max <= 0 || s == 0)
        return 0;
    int n = 0;
    while (n < max - 1) {
        const int c = fgetc(s);
        if (c == EOF)
            break;
        into[n++] = (char)c;
        if (c == '\n')
            break;
    }
    if (n == 0)
        return 0;                       /* nothing at all: end of the file */
    into[n] = '\0';
    return into;
}

/* --- writing ------------------------------------------------------------------ */

int fputc(int c, FILE* s)
{
    start();
    if (s == 0 || (s->flags & MODE_WRITE) == 0) {
        errno = EBADF;
        return EOF;
    }
    to_writing(s);

    if (s->buffering == _IONBF) {
        const unsigned char byte = (unsigned char)c;
        if (write(s->fd, &byte, 1) != 1) {
            s->failed = 1;
            return EOF;
        }
        return (unsigned char)c;
    }

    s->buffer[s->at++] = (unsigned char)c;
    if (s->at >= s->size || (s->buffering == _IOLBF && c == '\n')) {
        if (drain(s) != 0)
            return EOF;
    }
    return (unsigned char)c;
}

int putc(int c, FILE* s) { return fputc(c, s); }
int putchar(int c)       { start(); return fputc(c, stdout); }

size_t fwrite(const void* from, size_t size, size_t count, FILE* s)
{
    start();
    if (s == 0 || from == 0 || size == 0)
        return 0;
    const unsigned char* in = (const unsigned char*)from;
    const size_t want = size * count;

    if (s->buffering == _IONBF || want >= s->size) {
        /* Bigger than the buffer: send it straight out rather than shuffling
         * it through in pieces. The buffer goes first, to keep the order. */
        to_writing(s);
        if (drain(s) != 0)
            return 0;
        size_t done = 0;
        while (done < want) {
            const long n = write(s->fd, in + done, want - done);
            if (n <= 0) { s->failed = 1; break; }
            done += (size_t)n;
        }
        return done / size;
    }

    for (size_t i = 0; i < want; ++i)
        if (fputc(in[i], s) == EOF)
            return i / size;
    return count;
}

int fputs(const char* text, FILE* s)
{
    start();
    if (text == 0)
        return EOF;
    const size_t n = strlen(text);
    return fwrite(text, 1, n, s) == n ? (int)n : EOF;
}

int puts(const char* text)
{
    start();
    if (fputs(text, stdout) == EOF)
        return EOF;
    return fputc('\n', stdout) == EOF ? EOF : 0;
}

/* --- position and state -------------------------------------------------------- */

long fseek(FILE* s, long offset, int whence)
{
    start();
    if (s == 0) {
        errno = EBADF;
        return -1;
    }
    if (s->writing && drain(s) != 0)
        return -1;
    /* Anything read ahead has to be accounted for, or a seek relative to the
     * current position lands wherever the buffer happened to end. */
    if (!s->writing && whence == SEEK_CUR && s->held > s->at)
        offset -= (long)(s->held - s->at);
    s->at = s->held = 0;
    s->writing = 0;
    s->pushed = -1;
    s->at_end = 0;
    return lseek(s->fd, offset, whence) < 0 ? -1 : 0;
}

long ftell(FILE* s)
{
    start();
    if (s == 0) {
        errno = EBADF;
        return -1;
    }
    const long at = lseek(s->fd, 0, SEEK_CUR);
    if (at < 0)
        return -1;
    /* Where the caller is, not where the descriptor is: reading ahead puts
     * those apart by whatever is still in the buffer. */
    if (s->writing)
        return at + (long)s->at;
    return at - (long)(s->held - s->at) - (s->pushed >= 0 ? 1 : 0);
}

void rewind(FILE* s) { fseek(s, 0, SEEK_SET); clearerr(s); }

int feof(FILE* s)    { return s != 0 && s->at_end && s->at >= s->held; }
int ferror(FILE* s)  { return s != 0 && s->failed; }
void clearerr(FILE* s) { if (s != 0) { s->failed = 0; s->at_end = 0; } }
int fileno(FILE* s)  { if (s == 0) { errno = EBADF; return -1; } return s->fd; }

int setvbuf(FILE* s, char* buffer, int mode, size_t size)
{
    start();
    if (s == 0 || (mode != _IOFBF && mode != _IOLBF && mode != _IONBF)) {
        errno = EINVAL;
        return -1;
    }
    if (drain(s) != 0)
        return -1;
    s->buffering = mode;
    if (buffer != 0 && size > 0) {
        s->buffer = (unsigned char*)buffer;
        s->size = size;
        s->owns_buffer = 0;
    }
    return 0;
}
