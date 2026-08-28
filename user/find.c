/* find - walk a directory tree and name what is in it.
 *
 * Enough of find to be useful and not one option more: where to start, an
 * optional name pattern, and an optional type. The pattern language is the
 * same * and ? that grep and the shell use.
 */

#include <errno.h>
#include <cli.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char* g_pattern;
static int g_want_type = -1;            /* S_IFREG, S_IFDIR, or -1 for both */
static long g_found;
static int g_max_depth = 64;

static int match(const char* pattern, const char* text)
{
    if (pattern == 0)
        return 1;
    for (;;) {
        if (*pattern == '\0')
            return *text == '\0';
        if (*pattern == '*') {
            ++pattern;
            for (const char* t = text;; ++t) {
                if (match(pattern, t))
                    return 1;
                if (*t == '\0')
                    return 0;
            }
        }
        if (*text == '\0')
            return 0;
        if (*pattern != '?' && *pattern != *text)
            return 0;
        ++pattern;
        ++text;
    }
}

/* The last component of a path, which is what a name pattern is matched
 * against - `find / -name '*.PNG'` should not care what the directories are
 * called. */
static const char* base_name(const char* path)
{
    const char* last = path;
    for (const char* p = path; *p != '\0'; ++p)
        if (*p == '/')
            last = p + 1;
    return last;
}

/* One entry: printed if it passes both tests. cli_walk does the descending,
 * the depth limit and the "." and ".." skipping, which is the part that was
 * written out again in every program that walks a tree. */
static int seen(const char* path, unsigned type, void* user)
{
    (void)user;
    if ((g_want_type < 0 || (unsigned)g_want_type == type) &&
        match(g_pattern, base_name(path))) {
        printf("%s\n", path);
        ++g_found;
    }
    return 0;                           /* every match, not the first */
}

int main(int argc, char** argv)
{
    /* find's arguments are words, not letters - -name and -type are tests and
     * take values - so the library is told to leave them alone. */
    cli_begin(argc, argv, "[path] [-name pattern] [-type f|d]", 0);

    const char* start = ".";
    int i = 1;

    if (i < argc && argv[i][0] != '-')
        start = argv[i++];

    for (; i < argc; ++i) {
        if (strcmp(argv[i], "-name") == 0 && i + 1 < argc) {
            g_pattern = argv[++i];
        } else if (strcmp(argv[i], "-type") == 0 && i + 1 < argc) {
            ++i;
            if (argv[i][0] == 'f')      g_want_type = S_IFREG;
            else if (argv[i][0] == 'd') g_want_type = S_IFDIR;
            else cli_die("-type takes f or d");
        } else {
            cli_usage();
        }
    }

    struct stat info;
    if (stat(start, &info) != 0) {
        cli_fail("%s: %s", start, strerror(errno));
        return 1;
    }
    /* The starting point is a result too - `find . -type d` names the
     * directory it was pointed at - and the walk reports what is inside a
     * root, not the root, so it is tested here. */
    seen(start, info.st_type, 0);
    if (info.st_type == S_IFDIR)
        cli_walk(start, g_max_depth, seen, 0);
    return g_found > 0 ? 0 : 1;
}
