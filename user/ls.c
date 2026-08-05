#include <stdio.h>
#include <time.h>
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

    const time_t now = time(0);
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

        /* The directory entry carries a name, a type, a size, a mode and a
         * modification time but no owner, so the long format still has to stat
         * each one. */
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

        /* The date, in the form every ls uses: a recent file shows the time
         * of day, an old one shows the year, because the column is the same
         * width either way and which of the two you want depends on how long
         * ago it was. Six months is the traditional dividing line. */
        char when[24] = "            ";
        if (st.st_mtime > 0) {
            struct tm t;
            if (localtime_r(&st.st_mtime, &t) != 0) {
                const time_t age = now - st.st_mtime;
                strftime(when, sizeof(when),
                         (age > 15552000 || age < -3600) ? "%e %b  %Y"
                                                         : "%e %b %H:%M", &t);
            }
        }
        printf("%s %-8s %8lu %s %s%s\n", mode, owner,
               (unsigned long)st.st_size, when, entries[i].d_name,
               is_dir ? "/" : "");
    }
    return 0;
}
