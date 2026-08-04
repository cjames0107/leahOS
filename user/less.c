/* less - read a file a screenful at a time.
 *
 * A pager needs two things at once: the text on its output, and keystrokes
 * from the person reading it. There is no /dev/tty here to open separately, so
 * this reads keys from standard input - which means it pages a *file*, named
 * on the command line, and cannot page a pipe. `foo | less` would have the
 * pipe where the keyboard should be.
 *
 * The screen size is not something a program can ask for on this system, so it
 * is the terminal's 80x24 unless -N says otherwise.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_LINES 20000
#define POOL      (2u * 1024u * 1024u)

static char  g_pool[POOL];
static unsigned long g_pool_at;
static char* g_line[MAX_LINES];
static int   g_count;
static int   g_truncated;

static int g_rows = 23;                 /* one row kept for the prompt */

static void add(const char* text, unsigned long len)
{
    if (g_count >= MAX_LINES || g_pool_at + len + 1 > POOL) {
        g_truncated = 1;
        return;
    }
    char* copy = &g_pool[g_pool_at];
    memcpy(copy, text, len);
    copy[len] = '\0';
    g_pool_at += len + 1;
    g_line[g_count++] = copy;
}

static void load(int fd)
{
    char buffer[1024], line[1024];
    unsigned long len = 0;
    long n;
    while ((n = read(fd, buffer, sizeof(buffer))) > 0) {
        for (long i = 0; i < n; ++i) {
            if (buffer[i] == '\n') { add(line, len); len = 0; }
            else if (len < sizeof(line) - 1) line[len++] = buffer[i];
        }
    }
    if (len > 0)
        add(line, len);
}

static void show(int top, const char* name)
{
    for (int i = 0; i < g_rows; ++i) {
        const int at = top + i;
        if (at >= g_count)
            break;
        printf("%s\n", g_line[at]);
    }
    const int last = top + g_rows;
    if (last >= g_count)
        printf("%s (END) - q to quit ", name);
    else
        printf("%s %d%% - space, b, q ", name,
               g_count > 0 ? (last * 100) / g_count : 100);
}

int main(int argc, char** argv)
{
    const char* path = 0;
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] == '-' && argv[i][1] == 'N' && i + 1 < argc)
            g_rows = atoi_simple(argv[++i]) - 1;
        else
            path = argv[i];
    }
    if (path == 0) {
        printf("usage: less [-N rows] <file>\n");
        printf("  a file, not a pipe: the keyboard is on standard input\n");
        return 2;
    }
    if (g_rows < 2)
        g_rows = 23;

    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("less: %s: cannot open\n", path);
        return 1;
    }
    load(fd);
    close(fd);
    if (g_truncated)
        printf("less: showing the first %d lines only\n", g_count);

    int top = 0;
    show(top, path);
    for (;;) {
        char key;
        if (read(0, &key, 1) != 1)
            break;
        if (key == 'q' || key == 'Q')
            break;
        else if (key == ' ' || key == 'f')  top += g_rows;
        else if (key == 'b')                top -= g_rows;
        else if (key == '\n' || key == 'j') top += 1;
        else if (key == 'k')                top -= 1;
        else if (key == 'g')                top = 0;
        else if (key == 'G')                top = g_count - g_rows;
        else continue;

        if (top > g_count - g_rows) top = g_count - g_rows;
        if (top < 0) top = 0;
        printf("\n");
        show(top, path);
    }
    printf("\n");
    return 0;
}
