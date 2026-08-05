#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>

int main(int argc, char** argv)
{
    if (argc < 2) {
        printf("usage: mkdir DIR...\n");
        return 1;
    }
    int status = 0;
    for (int i = 1; i < argc; ++i) {
        if (mkdir(argv[i]) < 0) {
            fprintf(stderr, "mkdir: %s: %s\n", argv[i], strerror(errno));
            status = 1;
        }
    }
    return status;
}
