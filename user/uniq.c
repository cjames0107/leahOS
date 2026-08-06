/* Drop repeated lines - the adjacent ones only.
 *
 * Adjacent because that is all a stream lets you see without remembering the
 * whole of it, which is why this and sort are so often written together.
 */

#include <stdio.h>
#include <string.h>

int main(int argc, char** argv)
{
    int counting = 0, only_repeated = 0, only_unique = 0;
    const char* path = 0;

    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            for (int c = 1; argv[i][c] != '\0'; ++c) {
                switch (argv[i][c]) {
                case 'c': counting = 1; break;
                case 'd': only_repeated = 1; break;
                case 'u': only_unique = 1; break;
                default:
                    fprintf(stderr, "uniq: -%c: not an option here\n",
                            argv[i][c]);
                    return 1;
                }
            }
        } else {
            path = argv[i];
        }
    }

    FILE* in = path != 0 ? fopen(path, "r") : stdin;
    if (in == 0) {
        fprintf(stderr, "uniq: %s: cannot open\n", path);
        return 1;
    }

    char line[1024], held[1024];
    int holding = 0;
    unsigned long seen = 0;

    for (;;) {
        const int more = fgets(line, sizeof(line), in) != 0;
        if (more) {
            const unsigned long n = strlen(line);
            if (n > 0 && line[n - 1] == '\n')
                line[n - 1] = '\0';
            if (holding && strcmp(line, held) == 0) {
                ++seen;
                continue;
            }
        }
        /* The held line is printed when the next different one arrives, and
         * at the end - which is the only moment its count is known. */
        if (holding && (!only_repeated || seen > 1) &&
            (!only_unique || seen == 1)) {
            if (counting)
                printf("%4lu %s\n", seen, held);
            else
                printf("%s\n", held);
        }
        if (!more)
            break;
        snprintf(held, sizeof(held), "%s", line);
        holding = 1;
        seen = 1;
    }

    if (path != 0)
        fclose(in);
    return 0;
}
