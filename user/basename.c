/* The last part of a path.
 *
 * With a second argument, that suffix is removed if the name ends with it -
 * `basename report.md .md` gives `report`, which is the one use that makes
 * this more than a string operation anybody could do inline.
 */

#include <stdio.h>
#include <string.h>

int main(int argc, char** argv)
{
    if (argc < 2) {
        printf("usage: basename PATH [SUFFIX]\n");
        return 1;
    }

    char work[512];
    snprintf(work, sizeof(work), "%s", argv[1]);

    /* Trailing slashes are not part of the name: `basename /usr/bin/` is bin,
     * because the slash says "directory" and not "empty last component". */
    unsigned long n = strlen(work);
    while (n > 1 && work[n - 1] == '/')
        work[--n] = '\0';

    const char* base = work;
    for (const char* p = work; *p != '\0'; ++p)
        if (*p == '/')
            base = p + 1;
    if (base[0] == '\0')
        base = "/";

    if (argc > 2) {
        const unsigned long len = strlen(base), cut = strlen(argv[2]);
        if (cut > 0 && cut < len && strcmp(base + len - cut, argv[2]) == 0) {
            char trimmed[512];
            snprintf(trimmed, sizeof(trimmed), "%.*s", (int)(len - cut), base);
            printf("%s\n", trimmed);
            return 0;
        }
    }
    printf("%s\n", base);
    return 0;
}
