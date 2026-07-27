#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void mode_string(unsigned mode, int is_dir, char* out)
{
    static const char* kBits = "rwx";
    out[0] = is_dir ? 'd' : '-';
    for (int i = 0; i < 9; ++i)
        out[1 + i] = (mode & (0400 >> i)) ? kBits[i % 3] : '-';
    out[10] = '\0';
}

/* Join a directory and an entry into a path stat() can use. */
static void join(const char* dir, const char* name, char* out, int max)
{
    if (strcmp(dir, "/") == 0)
        snprintf(out, max, "/%s", name);
    else
        snprintf(out, max, "%s/%s", dir, name);
}

int main(int argc, char** argv)
{
    int long_format = 0;
    const char* path = ".";
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-l") == 0)
            long_format = 1;
        else
            path = argv[i];
    }

    struct dirent entries[64];
    const int n = getdents(path, entries, 64);
    if (n < 0) {
        printf("ls: %s: not a directory\n", path);
        return 1;
    }

    for (int i = 0; i < n; ++i) {
        const int is_dir = entries[i].d_type == S_IFDIR;
        if (!long_format) {
            if (is_dir)
                printf("%s/\n", entries[i].d_name);
            else
                printf("%s\t%lu\n", entries[i].d_name,
                       (unsigned long)entries[i].d_size);
            continue;
        }

        /* The directory entry carries a name, a type and a size but no owner
         * or mode, so the long format has to stat each one. */
        char full[256];
        join(path, entries[i].d_name, full, sizeof(full));

        struct stat st;
        if (stat(full, &st) < 0) {
            printf("?????????? %s\n", entries[i].d_name);
            continue;
        }

        char mode[11];
        mode_string(st.st_mode, is_dir, mode);

        char owner[32];
        if (username(st.st_uid, owner) != 0)
            snprintf(owner, sizeof(owner), "%u", st.st_uid);

        printf("%s %-8s %8lu %s%s\n", mode, owner,
               (unsigned long)st.st_size, entries[i].d_name, is_dir ? "/" : "");
    }
    return 0;
}
