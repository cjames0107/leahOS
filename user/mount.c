/* What is mounted where.
 *
 * Reads /proc/mounts rather than knowing anything itself, which is the point:
 * the filesystem server owns the table, and a command carrying its own copy
 * would be a second answer to a question that has one right one.
 *
 * It does not mount anything. There is one block filesystem here and no way to
 * add a second yet - every superblock field in the server is a singleton, and
 * a second device means all of them per-mount - so an option to try would only
 * ever fail.
 */

#include <stdio.h>
#include <string.h>

/* Up to four whitespace-separated fields. Written out rather than scanf'd
 * because there is no sscanf here, and one loop is smaller than one. */
static int split(char* line, char* field[], int max)
{
    int n = 0;
    char* at = line;
    while (*at != '\0' && n < max) {
        while (*at == ' ' || *at == '\t' || *at == '\n')
            *at++ = '\0';
        if (*at == '\0')
            break;
        field[n++] = at;
        while (*at != '\0' && *at != ' ' && *at != '\t' && *at != '\n')
            ++at;
        /* Ended here, so end it here. Without this the last field on a line
         * keeps its newline and prints in the middle of the format rather
         * than at the end of it. */
        if (*at != '\0' && n == max)
            *at = '\0';
    }
    return n;
}

int main(void)
{
    FILE* in = fopen("/proc/mounts", "r");
    if (in == 0) {
        fprintf(stderr, "mount: /proc/mounts is not there\n");
        return 1;
    }

    char line[256];
    while (fgets(line, sizeof(line), in) != 0) {
        char* field[4];
        if (split(line, field, 4) < 4)
            continue;
        printf("%s on %s type %s (%s)\n", field[0], field[1],
               field[2], field[3]);
    }
    fclose(in);
    return 0;
}
