/* See <cli.h>. The parts of a command-line program that are not the program. */

#include <cli.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CLI_MAX_ARGS 64

static const char* g_name = "program";
static const char* g_usage;
static char*       g_argv[CLI_MAX_ARGS];
static int         g_argc;
static char*       g_rest[CLI_MAX_ARGS];
static int         g_rest_n;
static int         g_split;     /* the options have been separated out */

void cli_begin(int argc, char** argv, const char* usage)
{
    g_usage = usage;
    if (argc > 0 && argv[0] != 0 && argv[0][0] != '\0') {
        /* The last component: a program invoked as /bin/ls should say "ls",
         * because that is what the person typed and what they will search for
         * when they look the message up. */
        const char* p = argv[0];
        for (const char* q = argv[0]; *q != '\0'; ++q)
            if (*q == '/' && q[1] != '\0')
                p = q + 1;
        g_name = p;
    }
    g_argc = 0;
    for (int i = 1; i < argc && g_argc < CLI_MAX_ARGS; ++i)
        g_argv[g_argc++] = argv[i];
    g_split = 0;
    g_rest_n = 0;
}

int cli_fail(const char* fmt, ...)
{
    fprintf(stderr, "%s: ", g_name);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
    return -1;
}

void cli_die(const char* fmt, ...)
{
    fprintf(stderr, "%s: ", g_name);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
    exit(1);
}

void cli_usage(void)
{
    if (g_usage != 0)
        fprintf(stderr, "usage: %s %s\n", g_name, g_usage);
    else
        fprintf(stderr, "usage: %s\n", g_name);
    exit(1);
}

/* Everything that is not an option, in order.
 *
 * Done once and remembered, because a program asking for its third argument
 * after having asked for two flags must get the same answer as one that asked
 * in the other order. "--" ends the options, so a file really called -n is
 * still reachable.
 */
static void split_args(void)
{
    if (g_split)
        return;
    g_split = 1;
    g_rest_n = 0;
    int only_files = 0;
    for (int i = 0; i < g_argc; ++i) {
        const char* a = g_argv[i];
        if (!only_files && a[0] == '-' && a[1] == '-' && a[2] == '\0') {
            only_files = 1;
            continue;
        }
        if (only_files || a[0] != '-' || a[1] == '\0') {
            if (g_rest_n < CLI_MAX_ARGS)
                g_rest[g_rest_n++] = g_argv[i];
        }
    }
}

int cli_flag(const char* name)
{
    if (name == 0 || name[0] != '-')
        return 0;
    const char letter = name[1];
    for (int i = 0; i < g_argc; ++i) {
        const char* a = g_argv[i];
        if (a[0] == '-' && a[1] == '-' && a[2] == '\0')
            break;                      /* everything after -- is a filename */
        if (a[0] != '-' || a[1] == '\0')
            continue;
        if (strcmp(a, name) == 0)
            return 1;
        /* Inside a run: -la holds -l and -a. Only for single letters, which is
         * the only form that can be run together without ambiguity. */
        if (name[2] == '\0')
            for (int k = 1; a[k] != '\0'; ++k)
                if (a[k] == letter)
                    return 1;
    }
    return 0;
}

const char* cli_value(const char* name, const char* fallback)
{
    if (name == 0)
        return fallback;
    const unsigned n = (unsigned)strlen(name);
    for (int i = 0; i < g_argc; ++i) {
        const char* a = g_argv[i];
        if (a[0] == '-' && a[1] == '-' && a[2] == '\0')
            break;
        if (strncmp(a, name, n) != 0)
            continue;
        if (a[n] != '\0')
            return &a[n];               /* -n10 */
        if (i + 1 < g_argc)
            return g_argv[i + 1];       /* -n 10 */
    }
    return fallback;
}

long cli_number(const char* name, long fallback)
{
    const char* text = cli_value(name, 0);
    if (text == 0 || text[0] == '\0')
        return fallback;
    return atoi_simple(text);
}

int cli_argc(void)
{
    split_args();
    return g_rest_n;
}

const char* cli_arg(int index)
{
    split_args();
    /* The value of an option is not a positional argument. "-n 10 file" has
     * one file, and a program that read "10" as its first filename would open
     * something that is not there and blame the person for it. */
    if (index < 0 || index >= g_rest_n)
        return 0;
    return g_rest[index];
}

/* --- files ---------------------------------------------------------------- */

long cli_read_file(const char* path, char* out, unsigned long max)
{
    if (path == 0 || out == 0 || max == 0)
        return -1;
    const int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    unsigned long got = 0;
    for (;;) {
        const long n = read(fd, out + got, max - 1 - got);
        if (n < 0) { close(fd); return -1; }
        if (n == 0)
            break;
        got += (unsigned long)n;
        if (got + 1 >= max) {
            /* It did not fit. Reported as a failure rather than as a short
             * read: a caller handed half a file has no way to know it. */
            close(fd);
            return -1;
        }
    }
    close(fd);
    out[got] = '\0';
    return (long)got;
}

int cli_write_file(const char* path, const void* data, unsigned long bytes)
{
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return -1;
    const char* p = (const char*)data;
    unsigned long done = 0;
    while (done < bytes) {
        const long n = write(fd, p + done, bytes - done);
        if (n <= 0) { close(fd); return -1; }
        done += (unsigned long)n;
    }
    close(fd);
    return 0;
}

/* --- walking -------------------------------------------------------------- */

static int walk_from(const char* dir, int depth, int max_depth,
                     cli_walk_fn fn, void* user)
{
    if (depth > max_depth)
        return 0;
    /* Its own array per level. A shared scratch buffer reads the parent's
     * entries after a child has overwritten them, which looks like a corrupt
     * filesystem and is not. */
    struct dirent here[64];
    const int n = getdents(dir, here, 64);
    if (n < 0)
        return 0;
    for (int i = 0; i < n; ++i) {
        if (here[i].d_name[0] == '.')
            continue;                   /* . and .. and the hidden ones */
        char full[512];
        if (strcmp(dir, "/") == 0)
            snprintf(full, sizeof(full), "/%s", here[i].d_name);
        else
            snprintf(full, sizeof(full), "%s/%s", dir, here[i].d_name);
        const int is_dir = here[i].d_type == S_IFDIR;
        const int stop = fn(full, is_dir, user);
        if (stop != 0)
            return stop;
        if (is_dir) {
            const int deeper = walk_from(full, depth + 1, max_depth, fn, user);
            if (deeper != 0)
                return deeper;
        }
    }
    return 0;
}

int cli_walk(const char* root, int max_depth, cli_walk_fn fn, void* user)
{
    if (root == 0 || fn == 0)
        return -1;
    if (max_depth <= 0)
        max_depth = 16;
    return walk_from(root, 0, max_depth, fn, user);
}

void cli_human(unsigned long long bytes, char* out, unsigned long max)
{
    if (out == 0 || max == 0)
        return;
    if (bytes >= (1ull << 30))
        snprintf(out, max, "%llu.%llu GiB", bytes >> 30,
                 ((bytes >> 20) % 1024) * 10 / 1024);
    else if (bytes >= (1ull << 20))
        snprintf(out, max, "%llu.%llu MiB", bytes >> 20,
                 ((bytes >> 10) % 1024) * 10 / 1024);
    else if (bytes >= 1024)
        snprintf(out, max, "%llu.%llu KiB", bytes >> 10,
                 (bytes % 1024) * 10 / 1024);
    else
        /* Below a kilobyte the exact number is shorter and says more than
         * "0.9 KiB" does. */
        snprintf(out, max, "%llu bytes", bytes);
}
