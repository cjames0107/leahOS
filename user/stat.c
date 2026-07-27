/* stat - what the filesystem knows about a file. */

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

/* The familiar rwxrwxrwx, which is nine bits read three at a time. */
static void mode_string(unsigned mode, int is_dir, char* out)
{
    static const char* kBits = "rwx";
    out[0] = is_dir ? 'd' : '-';
    for (int i = 0; i < 9; ++i)
        out[1 + i] = (mode & (0400 >> i)) ? kBits[i % 3] : '-';
    out[10] = '\0';
}

static void print_owner(const char* label, unsigned id)
{
    char name[32];
    if (username(id, name) == 0)
        printf("%s%u (%s)", label, id, name);
    else
        printf("%s%u", label, id);
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        printf("usage: stat <file>\n");
        return 1;
    }

    struct stat st;
    if (stat(argv[1], &st) < 0) {
        printf("stat: %s: no such file\n", argv[1]);
        return 1;
    }

    const int is_dir = st.st_type == S_IFDIR;
    char mode[11];
    mode_string(st.st_mode, is_dir, mode);

    printf("  file: %s\n", argv[1]);
    printf("  type: %s\n", is_dir ? "directory" : "regular file");
    printf("  size: %lu bytes\n", (unsigned long)st.st_size);
    printf("  mode: %s (%04o)\n", mode, st.st_mode & 0777);
    printf("  ");
    print_owner("uid: ", st.st_uid);
    printf("   ");
    print_owner("gid: ", st.st_gid);
    printf("\n");
    return 0;
}
