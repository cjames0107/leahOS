#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

static void copy_fd(int fd)
{
    char buffer[512];
    long n;
    while ((n = read(fd, buffer, sizeof(buffer))) > 0)
        write(1, buffer, (unsigned long)n);
}

int main(int argc, char** argv)
{
    /* No files: copy stdin to stdout, which is what makes `... | cat` work. */
    if (argc < 2) {
        copy_fd(0);
        return 0;
    }

    int status = 0;
    for (int i = 1; i < argc; ++i) {
        const int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            printf("cat: %s: cannot open\n", argv[i]);
            status = 1;
            continue;
        }

        copy_fd(fd);
        close(fd);
    }
    return status;
}
