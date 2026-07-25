#include <stdio.h>
#include <sys/stat.h>

int main(int argc, char** argv)
{
    const char* path = argc > 1 ? argv[1] : ".";

    struct dirent entries[64];
    const int n = getdents(path, entries, 64);
    if (n < 0) {
        printf("ls: %s: not a directory\n", path);
        return 1;
    }

    for (int i = 0; i < n; ++i) {
        if (entries[i].d_type == S_IFDIR)
            printf("%s/\n", entries[i].d_name);
        else
            printf("%s\t%lu\n", entries[i].d_name, (unsigned long)entries[i].d_size);
    }
    return 0;
}
