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
            printf("rm: %s: cannot remove\n", argv[i]);
            status = 1;
        }
    }
    return status;
}
