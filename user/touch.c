#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

/* Make the file exist, and mark it as of now.
 *
 * There are timestamps to bump now, and no utimes syscall to bump them with -
 * so an existing file is touched by writing to it: reading the last byte back
 * and writing the same byte returns, which vfsd stamps like any other write.
 * The contents are unchanged and mtime moves, which is what touch is for.
 *
 * An empty file has no last byte, so it is opened for writing and closed,
 * which creates it if it was not there and leaves it alone if it was. That
 * case does not move mtime, and -c is the flag for people who care. */
int main(int argc, char** argv)
{
    if (argc < 2) {
        printf("usage: touch FILE...\n");
        return 1;
    }
    int status = 0;
    for (int i = 1; i < argc; ++i) {
        struct stat info;
        const int existed = (stat(argv[i], &info) == 0);

        if (existed && info.st_type == S_IFREG && info.st_size > 0) {
            /* Rewrite the last byte with itself. */
            const int rd = open(argv[i], O_RDONLY);
            char byte = 0;
            int ok = 0;
            if (rd >= 0) {
                if (lseek(rd, (long)info.st_size - 1, SEEK_SET) >= 0 &&
                    read(rd, &byte, 1) == 1)
                    ok = 1;
                close(rd);
            }
            const int wr = ok ? open(argv[i], O_WRONLY) : -1;
            if (wr >= 0) {
                if (lseek(wr, (long)info.st_size - 1, SEEK_SET) < 0 ||
                    write(wr, &byte, 1) != 1)
                    ok = 0;
                close(wr);
            } else {
                ok = 0;
            }
            if (!ok) {
                printf("touch: %s: cannot update\n", argv[i]);
                status = 1;
            }
            continue;
        }

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
