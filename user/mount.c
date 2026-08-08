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

int main(void)
{
    FILE* in = fopen("/proc/mounts", "r");
    if (in == 0) {
        fprintf(stderr, "mount: /proc/mounts is not there\n");
        return 1;
    }

    char line[256];
    while (fgets(line, sizeof(line), in) != 0) {
        char what[64], at[64], kind[32], how[16];
        if (sscanf(line, "%63s %63s %31s %15s", what, at, kind, how) != 4)
            continue;
        printf("%s on %s type %s (%s)\n", what, at, kind, how);
    }
    fclose(in);
    return 0;
}
