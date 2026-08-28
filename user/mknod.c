#include <cli.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Make a device node.
 *
 * The one command that creates a file whose contents are a claim rather than
 * data: the two numbers say which driver answers, and nothing checks that a
 * driver by that name exists. That is not an oversight. A device node is made
 * before the driver is loaded as often as after, and a system that refused to
 * name a device it could not currently reach would be unable to describe its
 * own disks while repairing them.
 *
 * Root only, enforced by the filesystem server rather than here - a check in
 * this program would be advice, since anything may open the same server.
 */
int main(int argc, char** argv)
{
    cli_begin(argc, argv,
              "NAME TYPE MAJOR MINOR   (TYPE is c for character, b for block)",
              "");
    if (cli_argc() != 4)
        cli_usage();

    const char* path = cli_arg(0);
    const char* type = cli_arg(1);

    unsigned kind;
    if (strcmp(type, "c") == 0 || strcmp(type, "u") == 0)
        kind = S_IFCHR;
    else if (strcmp(type, "b") == 0)
        kind = S_IFBLK;
    else {
        cli_fail("%s: type is c or b", type);
        return 1;
    }

    const long maj = atoi_simple(cli_arg(2));
    const long min = atoi_simple(cli_arg(3));
    if (maj < 0 || maj > 255 || min < 0 || min > 255) {
        cli_fail("major and minor are 0 to 255");
        return 1;
    }

    if (mknod(path, kind, kind == S_IFCHR ? 0666 : 0660,
              makedev((unsigned)maj, (unsigned)min)) != 0) {
        cli_fail("%s: cannot create it", path);
        return 1;
    }
    return 0;
}
