#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char** argv)
{
    if (argc < 2) {
        printf("usage: cat FILE...\n");
        return 1;
    }

    int status = 0;
    for (int i = 1; i < argc; ++i) {
        const int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            printf("cat: %s: cannot open\n", argv[i]);
            status = 1;
            continue;
        }

        char buffer[512];
        long n;
        while ((n = read(fd, buffer, sizeof(buffer))) > 0)
            write(1, buffer, (unsigned long)n);
        close(fd);
    }
    return status;
}
