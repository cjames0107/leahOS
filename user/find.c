/* find - walk a directory tree and name what is in it.
 *
 * Enough of find to be useful and not one option more: where to start, an
 * optional name pattern, and an optional type. The pattern language is the
 * same * and ? that grep and the shell use.
 */

#include <errno.h>
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

static void walk(const char* path, unsigned type, int depth)
{
    if ((g_want_type < 0 || (unsigned)g_want_type == type) &&
        match(g_pattern, base_name(path))) {
        printf("%s\n", path);
        ++g_found;
    }
    if (type != S_IFDIR || depth >= g_max_depth)
        return;

    /* One buffer per level of the walk, on the stack, because the tree is
     * walked depth first and the parent's listing has to survive the child's.
     * Sixty-four entries at a time keeps that frame small; a directory with
     * more is read in several passes by getdents' own contract. */
    struct dirent entries[64];
    const int n = getdents(path, entries, 64);
    if (n < 0)
        return;
    for (int i = 0; i < n; ++i) {
        const char* name = entries[i].d_name;
        if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
            continue;
        char child[256];
        const unsigned long len = strlen(path);
        snprintf(child, sizeof(child), "%s%s%s", path,
                 (len > 0 && path[len - 1] == '/') ? "" : "/", name);
        walk(child, entries[i].d_type, depth + 1);
    }
}

int main(int argc, char** argv)
{
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
            else { printf("find: -type takes f or d\n"); return 2; }
        } else {
            printf("usage: find [path] [-name pattern] [-type f|d]\n");
            return 2;
        }
    }

    struct stat info;
    if (stat(start, &info) != 0) {
        fprintf(stderr, "find: %s: %s\n", start, strerror(errno));
        return 1;
    }
    walk(start, info.st_type, 0);
    return g_found > 0 ? 0 : 1;
}
