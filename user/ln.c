/* Make a name that points at another name.
 *
 * Only symbolic links. A hard link is a second directory entry for one inode,
 * which the filesystem underneath could do - the link count is already there
 * and already maintained - but the two are not variations of one idea, and
 * offering `ln` without `-s` when only `-s` works would be worse than offering
 * neither. So this asks for the flag, and says why when it is missing.
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
        printf("usage: ln -s TARGET NAME\n");
        return 1;
    }
    if (!symbolic) {
        fprintf(stderr, "ln: only symbolic links are made here; use -s\n");
        return 1;
    }

    char destination[256];
    resolve_destination(argv[at], argv[at + 1], destination,
                        sizeof(destination));

    if (symlink(argv[at], destination) != 0) {
        fprintf(stderr, "ln: %s: %s\n", destination, strerror(errno));
        return 1;
    }
    return 0;
}
