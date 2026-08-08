/* Make a name for a pipe.
 *
 * Two programs started separately cannot share an ordinary pipe: a pipe is
 * found by inheritance, and they have no common ancestor to inherit from. This
 * gives one a name on the filesystem, so both can find it.
 *
 *   mkfifo /tmp/f
 *   cat /tmp/f &
 *   echo hello > /tmp/f
 *
 * Opening one waits for the other end, which is what makes it a rendezvous
 * rather than a file: without it the echo would finish before the cat arrived
 * and what it wrote would go nowhere.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

int main(int argc, char** argv)
{
    if (argc < 2) {
        printf("usage: mkfifo NAME...\n");
        return 1;
    }
    int failed = 0;
    for (int i = 1; i < argc; ++i)
        if (mkfifo(argv[i], 0644) != 0) {
            fprintf(stderr, "mkfifo: %s: %s\n", argv[i], strerror(errno));
            failed = 1;
        }
    return failed;
}
