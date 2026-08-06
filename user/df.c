/* How much room is left.
 *
 * The mount table comes from /proc/mounts, and the numbers from the filesystem
 * server, which is the only thing that knows them. A filesystem that is not
 * storage - /proc, /dev - has no numbers to give and says so rather than
 * printing zeros, which would read as "full".
 */

#include <stdio.h>
#include <string.h>
#include <sys/statfs.h>

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
        fprintf(stderr, "df: /proc/mounts is not there\n");
        return 1;
    }

    printf("%-12s %10s %10s %10s %5s  %s\n",
           "filesystem", "1K-blocks", "used", "free", "use%", "on");

    char line[256];
    while (fgets(line, sizeof(line), in) != 0) {
        char* field[4];
        if (split(line, field, 4) < 4)
            continue;

        struct statfs fs;
        if (statfs(field[1], &fs) != 0 || fs.f_blocks == 0) {
            printf("%-12s %10s %10s %10s %5s  %s\n",
                   field[0], "-", "-", "-", "-", field[1]);
            continue;
        }

        const unsigned long kb = (unsigned long)(fs.f_bsize / 1024);
        const unsigned long total = (unsigned long)fs.f_blocks * kb;
        const unsigned long free_kb = (unsigned long)fs.f_bfree * kb;
        const unsigned long used = total - free_kb;
        const unsigned long percent = total > 0 ? (used * 100 + total / 2) / total : 0;

        printf("%-12s %10lu %10lu %10lu %4lu%%  %s\n",
               field[0], total, used, free_kb, percent, field[1]);
    }
    fclose(in);
    return 0;
}
