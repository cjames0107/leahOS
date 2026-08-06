/* Everything but the last part of a path.
 *
 * A path with no slash in it is in the current directory, so the answer is
 * "." - not the empty string, which would turn `cd $(dirname x)` into `cd`
 * and send somebody home.
 */

#include <stdio.h>
#include <string.h>

int main(int argc, char** argv)
{
    if (argc < 2) {
        printf("usage: dirname PATH\n");
        return 1;
    }

    char work[512];
    snprintf(work, sizeof(work), "%s", argv[1]);

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
