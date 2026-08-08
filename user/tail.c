/* tail - the last few lines of something.
 *
 * The end of a file cannot be found without having passed the middle, so this
 * keeps the last N lines in a ring as it reads and prints the ring at the end.
 * That costs the size of what is being kept rather than the size of the file,
 * which is the only way to tail something large.
 */

#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_KEEP 200
#define MAX_LINE 512

static char g_ring[MAX_KEEP][MAX_LINE];
static long g_used[MAX_KEEP];

static void last(int fd, long want)
{
    if (want > MAX_KEEP)
        want = MAX_KEEP;

    long at = 0, held = 0, len = 0;
    char buffer[1024];
    long n;

    for (long i = 0; i < want; ++i)
        g_used[i] = 0;

    while ((n = read(fd, buffer, sizeof(buffer))) > 0) {
        for (long i = 0; i < n; ++i) {
            if (len < MAX_LINE - 1)
                g_ring[at][len++] = buffer[i];
            if (buffer[i] == '\n') {
                g_used[at] = len;
                at = (at + 1) % want;
                if (held < want) ++held;
                len = 0;
            }
        }
    }
    /* A last line with no newline is still a line. */
    if (len > 0) {
        g_used[at] = len;
        at = (at + 1) % want;
        if (held < want) ++held;
    }

    for (long i = 0; i < held; ++i) {
        const long slot = (at + want - held + i) % want;
        write(1, g_ring[slot], (unsigned long)g_used[slot]);
    }
}

/* Keep printing whatever is appended, until somebody presses Ctrl-C.
 *
 * A file cannot be polled: it is always ready, because a read of it returns
 * immediately whether or not anything has been added. So this watches the size
 * instead and sleeps between looks - which is what -f has always been
 * underneath, on every system that does not have a change notification
 * mechanism, and this one does not.
 *
 * A file that has become shorter has been rewritten rather than appended to,
 * so following starts again from the beginning of it - which is what a log
 * that has just been rotated looks like.
 */
static void follow(const char* path, long from)
{
    long at = from;
    for (;;) {
        msleep(300);

        struct stat st;
        if (stat(path, &st) != 0)
            continue;               /* gone for the moment; it may come back */

        if ((long)st.st_size < at)
            at = 0;                 /* truncated: start over */
        if ((long)st.st_size == at)
            continue;

        const int fd = open(path, O_RDONLY);
        if (fd < 0)
            continue;
        lseek(fd, at, SEEK_SET);
        char buffer[4096];
        long n;
        while ((n = read(fd, buffer, sizeof(buffer))) > 0) {
            write(1, buffer, (unsigned long)n);
            at += n;
        }
        close(fd);
    }
}

int main(int argc, char** argv)
{
    long want = 10;
    int following = 0;
    int i = 1;

    for (; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; ) {
        if (argv[i][1] == 'n') {
            if (argv[i][2] != '\0') { want = atoi_simple(&argv[i][2]); ++i; }
            else if (i + 1 < argc) { want = atoi_simple(argv[i + 1]); i += 2; }
            else ++i;
        } else if (argv[i][1] == 'f') {
            following = 1;
            ++i;
        } else {
            fprintf(stderr, "tail: %s: not an option here\n", argv[i]);
            return 1;
        }
    }
    if (want < 0)
        want = 0;

    if (i >= argc) {
        last(0, want);
        return 0;
    }
    const int many = (argc - i) > 1;
    if (following && many) {
        /* Following several at once means interleaving them and reprinting a
         * header whenever the source changes, which is a different program. */
        fprintf(stderr, "tail: -f follows one file at a time\n");
        return 1;
    }
    int status = 0;
    for (int k = 0; i < argc; ++i, ++k) {
        const int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "tail: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }
        if (many) {
            if (k > 0) printf("\n");
            printf("==> %s <==\n", argv[i]);
        }
        last(fd, want);
        const long at = lseek(fd, 0, SEEK_CUR);
        close(fd);

        if (following)
            follow(argv[i], at);        /* never returns; Ctrl-C ends it */
    }
    return status;
}
