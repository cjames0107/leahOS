/* grep - find lines matching a pattern.
 *
 * Regular expressions, now that libc has an engine for them. This searched for
 * a fixed string with glob wildcards for a long time, and said so in its
 * manual, because a half-built regex that silently mishandles a pattern is
 * worse than a substring search that is honest about being one. The engine is
 * in <regex.h> and is shared, so there is still only one pattern language in
 * the system for this kind of matching.
 *
 * -F is here for the cases where the pattern is data rather than a pattern:
 * searching for "a[1]" should not need it spelled "a\[1\]".
 */

#include <fcntl.h>
#include <errno.h>
#include <regex.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_ignore_case, g_invert, g_numbers, g_count_only, g_recursive;
static int g_names_only, g_show_name, g_fixed;
static long g_total_matches;
static struct regex* g_re;

static int fold(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* A plain substring search, for -F. Kept rather than expressed as a pattern
 * because escaping every special character to search for a literal is exactly
 * the awkwardness -F exists to remove. */
static int contains(const char* needle, const char* hay)
{
    for (const char* at = hay;; ++at) {
        const char* a = needle;
        const char* b = at;
        while (*a != '\0') {
            int x = (unsigned char)*a, y = (unsigned char)*b;
            if (g_ignore_case) { x = fold(x); y = fold(y); }
            if (y == '\0' || x != y)
                break;
            ++a;
            ++b;
        }
        if (*a == '\0')
            return 1;
        if (*at == '\0')
            return 0;
    }
}

static int line_matches(const char* pattern, const char* line)
{
    if (g_fixed)
        return contains(pattern, line);
    /* regex_search already tries every starting position, which is what
     * "anywhere in the line" means and what grep has always meant. */
    return regex_search(g_re, line, 0, 0);
}

static void search_fd(int fd, const char* pattern, const char* name)
{
    char buffer[1024], line[2048];
    unsigned long len = 0;
    long n, number = 0, matches = 0;
    int truncated = 0;

    for (;;) {
        n = read(fd, buffer, sizeof(buffer));
        const int done = (n <= 0);
        for (long i = 0; !done && i < n; ++i) {
            if (buffer[i] != '\n') {
                if (len < sizeof(line) - 1)
                    line[len++] = buffer[i];
                else
                    truncated = 1;
                continue;
            }
            line[len] = '\0';
            ++number;
            if (line_matches(pattern, line) != g_invert) {
                ++matches;
                ++g_total_matches;
                if (g_names_only) { printf("%s\n", name); return; }
                if (!g_count_only) {
                    if (g_show_name) printf("%s:", name);
                    if (g_numbers)   printf("%ld:", number);
                    printf("%s%s\n", line, truncated ? " [line truncated]" : "");
                }
            }
            len = 0;
            truncated = 0;
        }
        if (done)
            break;
    }
    /* A final line with no newline still counts. */
    if (len > 0) {
        line[len] = '\0';
        ++number;
        if (line_matches(pattern, line) != g_invert) {
            ++matches;
            ++g_total_matches;
            if (g_names_only) { printf("%s\n", name); return; }
            if (!g_count_only) {
                if (g_show_name) printf("%s:", name);
                if (g_numbers)   printf("%ld:", number);
                printf("%s\n", line);
            }
        }
    }
    if (g_count_only) {
        if (g_show_name) printf("%s:", name);
        printf("%ld\n", matches);
    }
}

static int search_path(const char* path, const char* pattern);

static void search_dir(const char* path, const char* pattern)
{
    static struct dirent entries[128];
    const int n = getdents(path, entries, 128);
    if (n < 0)
        return;
    for (int i = 0; i < n; ++i) {
        if (entries[i].d_name[0] == '.')
            continue;                   /* including . and .. */
        char child[256];
        snprintf(child, sizeof(child), "%s%s%s", path,
                 path[strlen(path) - 1] == '/' ? "" : "/", entries[i].d_name);
        search_path(child, pattern);
    }
}

static int search_path(const char* path, const char* pattern)
{
    struct stat info;
    if (stat(path, &info) != 0) {
        fprintf(stderr, "grep: %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (info.st_type == S_IFDIR) {
        if (g_recursive) {
            search_dir(path, pattern);
            return 0;
        }
        fprintf(stderr, "grep: %s: is a directory\n", path);
        return 1;
    }
    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "grep: %s: %s\n", path, strerror(errno));
        return 1;
    }
    search_fd(fd, pattern, path);
    close(fd);
    return 0;
}

int main(int argc, char** argv)
{
    int i = 1;
    for (; i < argc; ++i) {
        if (argv[i][0] != '-' || argv[i][1] == '\0')
            break;
        for (int k = 1; argv[i][k] != '\0'; ++k) {
            switch (argv[i][k]) {
            case 'i': g_ignore_case = 1; break;
            case 'F': g_fixed = 1; break;
            case 'v': g_invert = 1; break;
            case 'n': g_numbers = 1; break;
            case 'c': g_count_only = 1; break;
            case 'r': g_recursive = 1; break;
            case 'l': g_names_only = 1; break;
            default:
                printf("grep: unknown option -%c\n", argv[i][k]);
                return 2;
            }
        }
    }
    if (i >= argc) {
        printf("usage: grep [-ivncrlF] PATTERN [FILE...]\n");
        printf("  the pattern is a regular expression; -F for a literal one\n");
        return 2;
    }
    const char* pattern = argv[i++];

    if (!g_fixed) {
        const char* error = 0;
        g_re = regex_compile(pattern, g_ignore_case, &error);
        if (g_re == 0) {
            /* Said plainly and once, before anything is read: a pattern that
             * cannot be compiled is not a search that found nothing. */
            fprintf(stderr, "grep: %s: %s\n", pattern, error);
            return 2;
        }
    }

    if (i >= argc) {
        search_fd(0, pattern, "(standard input)");
        return g_total_matches > 0 ? 0 : 1;
    }
    /* The name goes on each line when there is more than one file to tell
     * apart, which is what makes a recursive search readable. */
    g_show_name = (argc - i > 1) || g_recursive;

    int status = 0;
    for (; i < argc; ++i)
        if (search_path(argv[i], pattern) != 0)
            status = 2;
    if (status != 0)
        return status;
    return g_total_matches > 0 ? 0 : 1;
}
