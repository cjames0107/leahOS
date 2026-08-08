/* Make a second name for something.
 *
 * Two quite different things wearing one command, as everywhere:
 *
 *   a hard link is another directory entry for the same inode. There is no
 *   original and no copy; the file goes away when the last name does.
 *
 *   a symbolic link is a small file holding a path. It can point at a
 *   directory, at another filesystem, or at nothing at all, and it stops
 *   working if what it names is moved.
 *
 * The default is the hard one, because that is what `ln` has always meant.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* `ln -s target dir` means `ln -s target dir/<last part of target>`, which is
 * what makes `ln -s /usr/share/doc/readme.md .` do the obvious thing. */
static void resolve_destination(const char* target, const char* given,
                                char* out, unsigned long max)
{
    struct stat st;
    if (stat(given, &st) != 0 || st.st_type != S_IFDIR) {
        snprintf(out, max, "%s", given);
        return;
    }
    const char* base = target;
    for (const char* p = target; *p != '\0'; ++p)
        if (*p == '/' && p[1] != '\0')
            base = p + 1;
    snprintf(out, max, "%s/%s", given, base);
}

int main(int argc, char** argv)
{
    int symbolic = 0, at = 1;

    for (; at < argc && argv[at][0] == '-' && argv[at][1] != '\0'; ++at) {
        for (int c = 1; argv[at][c] != '\0'; ++c) {
            if (argv[at][c] == 's') {
                symbolic = 1;
            } else {
                fprintf(stderr, "ln: -%c: not an option here\n", argv[at][c]);
                return 1;
            }
        }
    }

    if (argc - at != 2) {
        printf("usage: ln [-s] TARGET NAME\n");
        printf("  without -s, another name for the same file\n");
        return 1;
    }
    char destination[256];
    resolve_destination(argv[at], argv[at + 1], destination,
                        sizeof(destination));

    const int failed = symbolic ? symlink(argv[at], destination)
                                : link(argv[at], destination);
    if (failed != 0) {
        fprintf(stderr, "ln: %s: %s\n", destination, strerror(errno));
        return 1;
    }
    return 0;
}
