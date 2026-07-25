#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char** argv)
{
    if (argc != 3) {
        printf("usage: cp SRC DST\n");
        return 1;
    }
    const int in = open(argv[1], O_RDONLY);
    if (in < 0) {
        printf("cp: %s: cannot open\n", argv[1]);
        return 1;
    }
    const int out = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) {
        printf("cp: %s: cannot create\n", argv[2]);
        close(in);
        return 1;
    }

    char buffer[512];
    long n;
    while ((n = read(in, buffer, sizeof(buffer))) > 0)
        write(out, buffer, (unsigned long)n);

    close(in);
    close(out);
    return 0;
}
