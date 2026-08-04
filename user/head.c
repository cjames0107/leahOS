/* head - the first few lines of something. */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void first(int fd, long want, const char* name, int show_name)
{
    if (show_name)
        printf("==> %s <==\n", name);

    char buffer[1024];
    long n, seen = 0;
    while (seen < want && (n = read(fd, buffer, sizeof(buffer))) > 0) {
        for (long i = 0; i < n; ++i) {
            write(1, &buffer[i], 1);
            if (buffer[i] == '\n' && ++seen == want)
                return;
        }
    }
}

int main(int argc, char** argv)
{
    long want = 10;
    int i = 1;
    if (i < argc && argv[i][0] == '-' && argv[i][1] == 'n') {
        /* -n10 and -n 10 both, because both are what people type. */
        if (argv[i][2] != '\0') { want = atoi_simple(&argv[i][2]); ++i; }
        else if (i + 1 < argc) { want = atoi_simple(argv[i + 1]); i += 2; }
    }
    if (want <= 0)
        return 0;

    if (i >= argc) {
        first(0, want, 0, 0);
        return 0;
    }
    const int many = (argc - i) > 1;
    int status = 0;
    for (int k = 0; i < argc; ++i, ++k) {
        const int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            printf("head: %s: cannot open\n", argv[i]);
            status = 1;
            continue;
        }
        if (many && k > 0)
            printf("\n");
        first(fd, want, argv[i], many);
        close(fd);
    }
    return status;
}
