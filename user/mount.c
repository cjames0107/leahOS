/* What is mounted where.
 *
 * Reads /proc/mounts rather than knowing anything itself, which is the point:
 * the filesystem server owns the table, and a command carrying its own copy
 * would be a second answer to a question that has one right one.
 *
 * With arguments it does mount something. The disk is a number rather than a
 * name because a number is all the block driver knows about itself: zero is
 * the one the system booted from.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/statfs.h>

static int usage(void)
{
    printf("usage: mount              list what is mounted\n");
    printf("       mount DISK AT      attach disk DISK at AT\n");
    printf("       mount -u AT        detach whatever is at AT\n");
    return 2;
}

int main(int argc, char** argv)
{
    if (argc == 3 && strcmp(argv[1], "-u") == 0) {
        if (fs_umount(argv[2]) != 0) {
            fprintf(stderr, "mount: %s: nothing to detach there\n", argv[2]);
            return 1;
        }
        return 0;
    }
    if (argc == 3) {
        const int disk = atoi_simple(argv[1]);
        if (disk < 0)
            return usage();
        if (fs_mount((unsigned)disk, argv[2]) != 0) {
            fprintf(stderr, "mount: disk %d will not attach at %s\n",
                    disk, argv[2]);
            return 1;
        }
        return 0;
    }
    if (argc != 1)
        return usage();

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
