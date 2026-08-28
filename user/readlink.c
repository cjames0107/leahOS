/* Where a symbolic link points.
 *
 * Prints the target exactly as it was written, relative or not. Resolving it
 * against the link's own directory is the reader's job, because a target is
 * text and only means a path once somebody decides where to read it from.
 */

#include <cli.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char** argv)
{
    cli_begin(argc, argv, "NAME...", "");
    if (cli_argc() < 1)
        cli_usage();

    int failed = 0;
    for (int i = 0; i < cli_argc(); ++i) {
        const char* name = cli_arg(i);
        char target[512];
        const long n = readlink(name, target, sizeof(target) - 1);
        if (n < 0) {
            cli_fail("%s: %s", name, strerror(errno));
            failed = 1;
            continue;
        }
        target[n] = '\0';
        printf("%s\n", target);
    }
    return failed;
}
