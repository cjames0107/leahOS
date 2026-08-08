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
        char what[64], at[64], kind[32], how[16];
        if (sscanf(line, "%63s %63s %31s %15s", what, at, kind, how) != 4)
            continue;

        struct statfs fs;
        if (statfs(at, &fs) != 0 || fs.f_blocks == 0) {
            printf("%-12s %10s %10s %10s %5s  %s\n",
                   what, "-", "-", "-", "-", at);
            continue;
        }

        const unsigned long kb = (unsigned long)(fs.f_bsize / 1024);
        const unsigned long total = (unsigned long)fs.f_blocks * kb;
        const unsigned long free_kb = (unsigned long)fs.f_bfree * kb;
        const unsigned long used = total - free_kb;
        const unsigned long percent = total > 0 ? (used * 100 + total / 2) / total : 0;

        printf("%-12s %10lu %10lu %10lu %4lu%%  %s\n",
               what, total, used, free_kb, percent, at);
    }
    fclose(in);
    return 0;
}
