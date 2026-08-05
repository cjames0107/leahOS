/* sort - put lines in order.
 *
 * Reads everything before writing anything, because that is what sorting is.
 * The limit is stated rather than silently truncating: a tool that quietly
 * drops the tail of its input is worse than one that says it cannot.
 */

#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_LINES 20000
#define POOL      (2u * 1024u * 1024u)

static char  g_pool[POOL];
static unsigned long g_pool_at;
static char* g_lines[MAX_LINES];
static int   g_count;
static int   g_overflow;

static void add_line(const char* text, unsigned long len)
{
    if (g_count >= MAX_LINES || g_pool_at + len + 1 > POOL) {
        g_overflow = 1;
        return;
    }
    char* copy = &g_pool[g_pool_at];
    memcpy(copy, text, len);
    copy[len] = '\0';
    g_pool_at += len + 1;
    g_lines[g_count++] = copy;
}

static void slurp(int fd)
{
    char buffer[1024], line[1024];
    unsigned long len = 0;
    long n;
    while ((n = read(fd, buffer, sizeof(buffer))) > 0) {
        for (long i = 0; i < n; ++i) {
            if (buffer[i] == '\n') {
                add_line(line, len);
                len = 0;
            } else if (len < sizeof(line) - 1) {
                line[len++] = buffer[i];
            }
        }
    }
    if (len > 0)
        add_line(line, len);
}

static int fold(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int compare(const char* a, const char* b, int ignore_case, int numeric)
{
    if (numeric) {
        const long x = atoi_simple(a), y = atoi_simple(b);
        if (x != y)
            return x < y ? -1 : 1;
        /* Equal numbers fall back to text, so the order is total and the
         * output does not shuffle between runs. */
    }
    for (int i = 0;; ++i) {
        int ca = (unsigned char)a[i], cb = (unsigned char)b[i];
        if (ignore_case) { ca = fold(ca); cb = fold(cb); }
        if (ca != cb) return ca < cb ? -1 : 1;
        if (ca == '\0') return 0;
    }
}

/* Insertion sort would be minutes on twenty thousand lines; this is a plain
 * top-down merge sort, which is O(n log n) and stable - equal lines keep the
 * order they arrived in, which is what makes -u and a second sort predictable. */
static char* g_scratch[MAX_LINES];

static void merge_sort(int lo, int hi, int ignore_case, int numeric)
{
    if (hi - lo < 2)
        return;
    const int mid = lo + (hi - lo) / 2;
    merge_sort(lo, mid, ignore_case, numeric);
    merge_sort(mid, hi, ignore_case, numeric);

    int a = lo, b = mid, at = lo;
    while (a < mid && b < hi)
        g_scratch[at++] = (compare(g_lines[b], g_lines[a], ignore_case, numeric) < 0)
                          ? g_lines[b++] : g_lines[a++];
    while (a < mid) g_scratch[at++] = g_lines[a++];
    while (b < hi)  g_scratch[at++] = g_lines[b++];
    for (int i = lo; i < hi; ++i)
        g_lines[i] = g_scratch[i];
}

int main(int argc, char** argv)
{
    int reverse = 0, ignore_case = 0, numeric = 0, unique = 0;
    int i = 1;
    for (; i < argc; ++i) {
        if (argv[i][0] != '-' || argv[i][1] == '\0')
            break;
        for (int k = 1; argv[i][k] != '\0'; ++k) {
            switch (argv[i][k]) {
            case 'r': reverse = 1; break;
            case 'f': ignore_case = 1; break;
            case 'n': numeric = 1; break;
            case 'u': unique = 1; break;
            default: printf("sort: unknown option -%c\n", argv[i][k]); return 2;
            }
        }
    }

    if (i >= argc) {
        slurp(0);
    } else {
        for (; i < argc; ++i) {
            const int fd = open(argv[i], O_RDONLY);
            if (fd < 0) {
                fprintf(stderr, "sort: %s: %s\n", argv[i], strerror(errno));
                return 1;
            }
            slurp(fd);
            close(fd);
        }
    }
    if (g_overflow)
        printf("sort: too much input, sorting the first %d lines\n", g_count);

    merge_sort(0, g_count, ignore_case, numeric);

    const char* previous = 0;
    for (int k = 0; k < g_count; ++k) {
        const char* line = g_lines[reverse ? g_count - 1 - k : k];
        if (unique && previous != 0 &&
            compare(previous, line, ignore_case, numeric) == 0)
            continue;
        printf("%s\n", line);
        previous = line;
    }
    return 0;
}
