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

int main(int argc, char** argv)
{
    long want = 10;
    int i = 1;
    if (i < argc && argv[i][0] == '-' && argv[i][1] == 'n') {
        if (argv[i][2] != '\0') { want = atoi_simple(&argv[i][2]); ++i; }
        else if (i + 1 < argc) { want = atoi_simple(argv[i + 1]); i += 2; }
    }
    if (want <= 0)
        return 0;

    if (i >= argc) {
        last(0, want);
        return 0;
    }
    const int many = (argc - i) > 1;
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
        close(fd);
    }
    return status;
}
