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
    /* No files: copy stdin to stdout, which is what makes `... | cat` work. */
    if (argc < 2) {
        if (copy_fd(0) != 0) {
            fprintf(stderr, "cat: %s\n", strerror(errno));
            return 1;
        }
        return 0;
    }

    int status = 0;
    for (int i = 1; i < argc; ++i) {
        const int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "cat: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }

        if (copy_fd(fd) != 0) {
            fprintf(stderr, "cat: %s: %s\n", argv[i], strerror(errno));
            status = 1;
        }
        close(fd);
    }
    return status;
}
