/* grep - find lines containing a pattern.
 *
 * Fixed strings, not regular expressions - with one exception: `*` and `?`
 * match the way they do in a filename, because that is the glob every other
 * tool here uses and having two pattern languages in one system is worse than
 * having a simple one. A pattern with no wildcard in it is a plain substring
 * search, which is what almost every use is.
 */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_ignore_case, g_invert, g_numbers, g_count_only, g_recursive;
static int g_names_only, g_show_name;
static long g_total_matches;

static int fold(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* Does `text` start with a match for `pattern`, allowing * and ?. Recursive on
 * `*` only, and the recursion is bounded by the length of the text. */
static int match_here(const char* pattern, const char* text)
{
    for (;;) {
        if (*pattern == '\0')
            return 1;                   /* the pattern ran out: a match */
        if (*pattern == '*') {
            ++pattern;
            /* Try the shortest expansion first, then longer ones. */
            for (const char* t = text;; ++t) {
                if (match_here(pattern, t))
                    return 1;
                if (*t == '\0')
                    return 0;
            }
        }
        if (*text == '\0')
            return 0;
        if (*pattern != '?') {
            int a = (unsigned char)*pattern, b = (unsigned char)*text;
            if (g_ignore_case) { a = fold(a); b = fold(b); }
            if (a != b)
                return 0;
        }
        ++pattern;
        ++text;
    }
}

/* Anywhere in the line, which is what grep means - so the search slides along
 * the text rather than anchoring at the front. */
static int line_matches(const char* pattern, const char* line)
{
    for (const char* at = line;; ++at) {
        if (match_here(pattern, at))
            return 1;
        if (*at == '\0')
            return 0;
    }
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
        printf("grep: %s: cannot open\n", path);
        return 1;
    }
    if (info.st_type == S_IFDIR) {
        if (g_recursive) {
            search_dir(path, pattern);
            return 0;
        }
        printf("grep: %s: is a directory\n", path);
        return 1;
    }
    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("grep: %s: cannot open\n", path);
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
        printf("usage: grep [-ivncrl] pattern [file...]\n");
        printf("  * and ? match as they do in a filename\n");
        return 2;
    }
    const char* pattern = argv[i++];

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
