/* Where a symbolic link points.
 *
 * Prints the target exactly as it was written, relative or not. Resolving it
 * against the link's own directory is the reader's job, because a target is
 * text and only means a path once somebody decides where to read it from.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char** argv)
{
    if (argc < 2) {
        printf("usage: readlink NAME...\n");
        return 1;
    }

    int failed = 0;
    for (int i = 1; i < argc; ++i) {
        char target[512];
        const long n = readlink(argv[i], target, sizeof(target) - 1);
        if (n < 0) {
            fprintf(stderr, "readlink: %s: %s\n", argv[i], strerror(errno));
            failed = 1;
            continue;
        }
        target[n] = '\0';
        printf("%s\n", target);
    }
    return failed;
}
