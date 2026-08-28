/* stat - what the filesystem knows about a file. */

#include <cli.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

/* The familiar rwxrwxrwx, which is nine bits read three at a time. */
static void mode_string(unsigned mode, unsigned type, char* out)
{
    static const char* kBits = "rwx";
    out[0] = type == S_IFDIR  ? 'd'
           : type == S_IFLNK  ? 'l'
           : type == S_IFIFO  ? 'p'
           : type == S_IFCHR  ? 'c'
           : type == S_IFBLK  ? 'b'
                              : '-';
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
    cli_begin(argc, argv, "FILE", "");
    if (cli_argc() != 1)
        cli_usage();
    const char* name = cli_arg(0);

    struct stat st;
    if (stat(name, &st) < 0) {
        cli_fail("%s: no such file", name);
        return 1;
    }

    char mode[11];
    mode_string(st.st_mode, st.st_type, mode);

    const char* kind = st.st_type == S_IFDIR  ? "directory"
                     : st.st_type == S_IFLNK  ? "symbolic link"
                     : st.st_type == S_IFIFO  ? "fifo"
                     : st.st_type == S_IFCHR  ? "character device"
                     : st.st_type == S_IFBLK  ? "block device"
                                              : "regular file";

    printf("  file: %s\n", name);
    printf("  type: %s\n", kind);
    /* A device has no size worth printing; it has a driver. */
    if (st.st_type == S_IFCHR || st.st_type == S_IFBLK)
        printf("device: %u, %u\n", major(st.st_rdev), minor(st.st_rdev));
    else
        printf("  size: %lu bytes\n", (unsigned long)st.st_size);
    printf("  mode: %s (%04o)\n", mode, st.st_mode & 0777);
    printf("  ");
    print_owner("uid: ", st.st_uid);
    printf("   ");
    print_owner("gid: ", st.st_gid);
    printf("\n");
    return 0;
}
