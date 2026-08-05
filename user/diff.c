/* diff - what changed between two files.
 *
 * The interesting part of a diff is deciding which lines *correspond*, and the
 * answer is the longest common subsequence: the most lines that appear in both
 * files in the same order. Everything not in it was added or removed.
 *
 * The table is O(n*m), which is the textbook algorithm and the reason there is
 * a line limit. Real diffs use Myers' algorithm, which finds the same answer in
 * O((n+m)d) where d is the size of the difference - a much better bound when
 * the files are similar, which they usually are. That is a bigger piece of work
 * and this is a tool for looking at two versions of a config file.
 */

#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MAX_LINES 1200
#define POOL      (512u * 1024u)

static char g_pool[2][POOL];
static unsigned long g_pool_at[2];
static char* g_line[2][MAX_LINES];
static int   g_count[2];
static int   g_truncated[2];

/* The LCS table. short is enough: the counts cannot exceed MAX_LINES. */
static short g_lcs[MAX_LINES + 1][MAX_LINES + 1];

static int load(const char* path, int which)
{
    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "diff: %s: %s\n", path, strerror(errno));
        return -1;
    }
    char buffer[1024], line[1024];
    unsigned long len = 0;
    long n;
    while ((n = read(fd, buffer, sizeof(buffer))) > 0) {
        for (long i = 0; i < n; ++i) {
            if (buffer[i] != '\n') {
                if (len < sizeof(line) - 1)
                    line[len++] = buffer[i];
                continue;
            }
            line[len] = '\0';
            if (g_count[which] < MAX_LINES &&
                g_pool_at[which] + len + 1 <= POOL) {
                char* copy = &g_pool[which][g_pool_at[which]];
                memcpy(copy, line, len + 1);
                g_pool_at[which] += len + 1;
                g_line[which][g_count[which]++] = copy;
            } else {
                g_truncated[which] = 1;
            }
            len = 0;
        }
    }
    if (len > 0 && g_count[which] < MAX_LINES &&
        g_pool_at[which] + len + 1 <= POOL) {
        line[len] = '\0';
        char* copy = &g_pool[which][g_pool_at[which]];
        memcpy(copy, line, len + 1);
        g_pool_at[which] += len + 1;
        g_line[which][g_count[which]++] = copy;
    }
    close(fd);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc != 3) {
        printf("usage: diff <file1> <file2>\n");
        return 2;
    }
    if (load(argv[1], 0) != 0 || load(argv[2], 1) != 0)
        return 2;
    if (g_truncated[0] || g_truncated[1])
        printf("diff: comparing the first %d lines only\n", MAX_LINES);

    const int n = g_count[0], m = g_count[1];

    /* Filled from the end backwards, so g_lcs[i][j] is the length of the
     * longest common subsequence of the two tails starting at i and j. */
    for (int i = n - 1; i >= 0; --i)
        for (int j = m - 1; j >= 0; --j)
            g_lcs[i][j] = (short)(strcmp(g_line[0][i], g_line[1][j]) == 0
                ? g_lcs[i + 1][j + 1] + 1
                : (g_lcs[i + 1][j] >= g_lcs[i][j + 1]
                   ? g_lcs[i + 1][j] : g_lcs[i][j + 1]));

    /* Walk forwards through the table, which turns it back into a sequence of
     * removals, additions and matches in file order. */
    int i = 0, j = 0, differences = 0;
    while (i < n && j < m) {
        if (strcmp(g_line[0][i], g_line[1][j]) == 0) {
            ++i; ++j;
        } else if (g_lcs[i + 1][j] >= g_lcs[i][j + 1]) {
            /* The increment is its own statement: reading i and incrementing
             * it in one argument list leaves the order unspecified, and the
             * line number printed would be whichever the compiler felt like. */
            printf("-%d: %s\n", i + 1, g_line[0][i]);
            ++i;
            ++differences;
        } else {
            printf("+%d: %s\n", j + 1, g_line[1][j]);
            ++j;
            ++differences;
        }
    }
    while (i < n) { printf("-%d: %s\n", i + 1, g_line[0][i]); ++i; ++differences; }
    while (j < m) { printf("+%d: %s\n", j + 1, g_line[1][j]); ++j; ++differences; }

    /* Silence means the same, which is the convention every diff follows and
     * the reason the exit status is what scripts test. */
    return differences > 0 ? 1 : 0;
}
