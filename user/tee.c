/* Copy standard input to standard output, and to files as well.
 *
 * The one command whose whole purpose is that a pipeline is a line: it lets
 * something in the middle of one be looked at without being taken out of it.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MAX_FILES 8

int main(int argc, char** argv)
{
    int fds[MAX_FILES], count = 0, append = 0, at = 1;

    for (; at < argc && argv[at][0] == '-' && argv[at][1] != '\0'; ++at) {
        for (int c = 1; argv[at][c] != '\0'; ++c) {
            if (argv[at][c] == 'a') {
                append = 1;
            } else {
                fprintf(stderr, "tee: -%c: not an option here\n", argv[at][c]);
                return 1;
            }
        }
    }

    for (; at < argc && count < MAX_FILES; ++at) {
        const int fd = open(argv[at], O_WRONLY | O_CREAT |
                            (append ? O_APPEND : O_TRUNC));
        if (fd < 0) {
            fprintf(stderr, "tee: %s: %s\n", argv[at], strerror(errno));
            continue;
        }
        fds[count++] = fd;
    }

    char buffer[4096];
    long n;
    while ((n = read(0, buffer, sizeof(buffer))) > 0) {
        write(1, buffer, (unsigned long)n);
        for (int i = 0; i < count; ++i)
            write(fds[i], buffer, (unsigned long)n);
    }
    for (int i = 0; i < count; ++i)
        close(fds[i]);
    return 0;
}
