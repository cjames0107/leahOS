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
#include <cli.h>
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

static int search_file(const char* path, const char* pattern)
{
    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        cli_fail("%s: %s", path, strerror(errno));
        return 1;
    }
    search_fd(fd, pattern, path);
    close(fd);
    return 0;
}

/* One entry of a recursive search. Only regular files are opened: a search
 * that read a fifo would sit there waiting for a writer that is not coming,
 * and one that read a device would search the device. */
static int found_by_walk(const char* path, unsigned type, void* user)
{
    if (type == S_IFREG)
        search_file(path, (const char*)user);
    return 0;                           /* all of them, not the first */
}

static int search_path(const char* path, const char* pattern)
{
    struct stat info;
    if (stat(path, &info) != 0) {
        cli_fail("%s: %s", path, strerror(errno));
        return 1;
    }
    if (info.st_type == S_IFDIR) {
        if (g_recursive) {
            cli_walk(path, 64, found_by_walk, (void*)pattern);
            return 0;
        }
        cli_fail("%s: is a directory", path);
        return 1;
    }
    return search_file(path, pattern);
}

int main(int argc, char** argv)
{
    cli_begin(argc, argv,
              "[-ivncrlF] PATTERN [FILE...]\n"
              "  the pattern is a regular expression; -F for a literal one",
              "iFvncrl");
    g_ignore_case = cli_flag("-i");
    g_fixed       = cli_flag("-F");
    g_invert      = cli_flag("-v");
    g_numbers     = cli_flag("-n");
    g_count_only  = cli_flag("-c");
    g_recursive   = cli_flag("-r");
    g_names_only  = cli_flag("-l");

    if (cli_argc() < 1)
        cli_usage();
    const char* pattern = cli_arg(0);

    if (!g_fixed) {
        const char* error = 0;
        g_re = regex_compile(pattern, g_ignore_case, &error);
        if (g_re == 0) {
            /* Said plainly and once, before anything is read: a pattern that
             * cannot be compiled is not a search that found nothing. */
            cli_fail("%s: %s", pattern, error);
            return 2;
        }
    }

    if (cli_argc() < 2) {
        search_fd(0, pattern, "(standard input)");
        return g_total_matches > 0 ? 0 : 1;
    }
    /* The name goes on each line when there is more than one file to tell
     * apart, which is what makes a recursive search readable. */
    g_show_name = (cli_argc() > 2) || g_recursive;

    int status = 0;
    for (int i = 1; i < cli_argc(); ++i)
        if (search_path(cli_arg(i), pattern) != 0)
            status = 2;
    if (status != 0)
        return status;
    return g_total_matches > 0 ? 0 : 1;
}
