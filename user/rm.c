#include <errno.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char** argv)
{
    if (argc < 2) {
        printf("usage: rm FILE...\n");
        return 1;
    }
    int status = 0;
    for (int i = 1; i < argc; ++i) {
        if (unlink(argv[i]) < 0) {
            fprintf(stderr, "rm: %s: %s\n", argv[i], strerror(errno));
            status = 1;
        }
    }
    return status;
}
