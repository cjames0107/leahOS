/* Drop repeated lines - the adjacent ones only.
 *
 * Adjacent because that is all a stream lets you see without remembering the
 * whole of it, which is why this and sort are so often written together.
 */

#include <cli.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char** argv)
{
    cli_begin(argc, argv, "[-cdu] [file]", "cdu");
    const int counting = cli_flag("-c");
    const int only_repeated = cli_flag("-d");
    const int only_unique = cli_flag("-u");
    const char* path = cli_arg(0);

    FILE* in = path != 0 ? fopen(path, "r") : stdin;
    if (in == 0) {
        cli_fail("%s: cannot open", path);
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
