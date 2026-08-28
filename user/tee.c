/* Copy standard input to standard output, and to files as well.
 *
 * The one command whose whole purpose is that a pipeline is a line: it lets
 * something in the middle of one be looked at without being taken out of it.
 */

#include <cli.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MAX_FILES 8

int main(int argc, char** argv)
{
    cli_begin(argc, argv, "[-a] [file...]", "a");
    const int append = cli_flag("-a");

    int fds[MAX_FILES], count = 0;
    for (int i = 0; i < cli_argc() && count < MAX_FILES; ++i) {
        const int fd = open(cli_arg(i), O_WRONLY | O_CREAT |
                            (append ? O_APPEND : O_TRUNC));
        if (fd < 0) {
            cli_fail("%s: %s", cli_arg(i), strerror(errno));
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
