#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

/* Create the file if it does not exist. We have no timestamps to bump, so on an
 * existing file this is a no-op, which is fine for the common "make sure it
 * exists" use. */
int main(int argc, char** argv)
{
    if (argc < 2) {
        printf("usage: touch FILE...\n");
        return 1;
    }
    int status = 0;
    for (int i = 1; i < argc; ++i) {
        const int fd = open(argv[i], O_WRONLY | O_CREAT);
        if (fd < 0) {
            printf("touch: %s: cannot create\n", argv[i]);
            status = 1;
        } else {
            close(fd);
        }
    }
    return status;
}
