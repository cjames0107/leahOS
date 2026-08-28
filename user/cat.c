#include <cli.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Returns 0, or -1 with errno set. A read that fails is not the same as a
 * file that ended, and the loop used to treat them alike - so `cat` on a
 * directory printed nothing at all and looked like an empty file. */
static int copy_fd(int fd)
{
    char buffer[512];
    for (;;) {
        const long n = read(fd, buffer, sizeof(buffer));
        if (n == 0)
            return 0;
        if (n < 0)
            return -1;
        if (write(1, buffer, (unsigned long)n) != n)
            return -1;
    }
}

int main(int argc, char** argv)
{
    cli_begin(argc, argv, "[file...]", "");

    /* No files: copy stdin to stdout, which is what makes `... | cat` work. */
    if (cli_argc() < 1) {
        if (copy_fd(0) == 0)
            return 0;
        cli_fail("%s", strerror(errno));
        return 1;
    }

    int status = 0;
    for (int i = 0; i < cli_argc(); ++i) {
        const char* name = cli_arg(i);

        /* A lone "-" is standard input by name, so it can be put in the
         * middle: `cat header - footer`. */
        const int fd = strcmp(name, "-") == 0 ? 0 : open(name, O_RDONLY);
        if (fd < 0) {
            cli_fail("%s: %s", name, strerror(errno));
            status = 1;
            continue;
        }

        if (copy_fd(fd) != 0) {
            cli_fail("%s: %s", name, strerror(errno));
            status = 1;
        }
        if (fd != 0)
            close(fd);
    }
    return status;
}
