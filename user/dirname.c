/* Everything but the last part of a path.
 *
 * A path with no slash in it is in the current directory, so the answer is
 * "." - not the empty string, which would turn `cd $(dirname x)` into `cd`
 * and send somebody home.
 */

#include <cli.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char** argv)
{
    cli_begin(argc, argv, "PATH", "");
    if (cli_argc() < 1)
        cli_usage();

    char work[512];
    snprintf(work, sizeof(work), "%s", cli_arg(0));

    unsigned long n = strlen(work);
    while (n > 1 && work[n - 1] == '/')
        work[--n] = '\0';

    while (n > 0 && work[n - 1] != '/')
        --n;
    if (n == 0) {
        printf(".\n");
        return 0;
    }
    while (n > 1 && work[n - 1] == '/')
        --n;
    work[n] = '\0';
    printf("%s\n", work);
    return 0;
}
