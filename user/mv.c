#include <stdio.h>
#include <unistd.h>

/* A real rename now: one syscall moves the directory entry, no data copy. */
int main(int argc, char** argv)
{
    if (argc != 3) {
        printf("usage: mv SRC DST\n");
        return 1;
    }
    if (rename(argv[1], argv[2]) < 0) {
        printf("mv: cannot rename %s to %s\n", argv[1], argv[2]);
        return 1;
    }
    return 0;
}
