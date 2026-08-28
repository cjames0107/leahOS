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

#include <cli.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/statfs.h>

int main(int argc, char** argv)
{
    cli_begin(argc, argv,
              "                 list what is mounted\n"
              "       mount DISK AT      attach disk DISK at AT\n"
              "       mount -u AT        detach whatever is at AT", "u");

    if (cli_flag("-u")) {
        if (cli_argc() != 1)
            cli_usage();
        if (fs_umount(cli_arg(0)) != 0) {
            cli_fail("%s: nothing to detach there", cli_arg(0));
            return 1;
        }
        return 0;
    }
    if (cli_argc() == 2) {
        const int disk = atoi_simple(cli_arg(0));
        if (disk < 0)
            cli_usage();
        if (fs_mount((unsigned)disk, cli_arg(1)) != 0) {
            cli_fail("disk %d will not attach at %s", disk, cli_arg(1));
            return 1;
        }
        return 0;
    }
    if (cli_argc() != 0)
        cli_usage();

    FILE* in = fopen("/proc/mounts", "r");
    if (in == 0) {
        cli_fail("/proc/mounts is not there");
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
