/* tar - list and extract tape archives.
 *
 * The format is deliberately dull: a 512-byte header, the file's bytes padded
 * up to the next 512, and so on, ending with two zeroed blocks. Numbers are
 * octal in ASCII, which is the part everyone gets wrong the first time.
 *
 * Reading only. Writing an archive is a different job - it needs to walk a
 * tree, decide what to include and get the ownership right - and reading is
 * what makes software arrive on a machine, which is the point of having this
 * at all. With gunzip alongside it, a .tar.gz becomes two commands.
 */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BLOCK 512

/* The fields of the header this actually uses. ustar puts a prefix field at
 * 345 which can carry a longer path; it is joined here because otherwise a
 * deep archive silently loses its directories. */
struct header {
    char name[100];         /*   0 */
    char mode[8];           /* 100 */
    char uid[8];            /* 108 */
    char gid[8];            /* 116 */
    char size[12];          /* 124 */
    char mtime[12];         /* 136 */
    char checksum[8];       /* 148 */
    char type;              /* 156 */
    char link[100];         /* 157 */
    char magic[6];          /* 257 */
    char version[2];        /* 263 */
    char uname[32];         /* 265 */
    char gname[32];         /* 297 */
    char devmajor[8];       /* 329 */
    char devminor[8];       /* 337 */
    char prefix[155];       /* 345 */
    char padding[12];       /* 500 */
};

static unsigned long from_octal(const char* field, int len)
{
    unsigned long value = 0;
    for (int i = 0; i < len; ++i) {
        const char c = field[i];
        if (c == '\0' || c == ' ')
            break;                      /* the field is space or NUL padded */
        if (c < '0' || c > '7')
            continue;
        value = value * 8 + (unsigned long)(c - '0');
    }
    return value;
}

/* The header's own checksum: the sum of every byte, with the checksum field
 * itself counted as spaces. A tar whose sums do not add up is not a tar. */
static int checksum_ok(const unsigned char* raw)
{
    unsigned long sum = 0;
    for (int i = 0; i < BLOCK; ++i)
        sum += (i >= 148 && i < 156) ? (unsigned)' ' : raw[i];
    const struct header* h = (const struct header*)raw;
    return sum == from_octal(h->checksum, 8);
}

static int is_zero_block(const unsigned char* raw)
{
    for (int i = 0; i < BLOCK; ++i)
        if (raw[i] != 0)
            return 0;
    return 1;
}

/* Refuse a path that would land outside where we are extracting. An archive is
 * an untrusted file, and "../.." in a member name is the oldest trick there
 * is. */
static int path_is_safe(const char* path)
{
    if (path[0] == '/')
        return 0;
    for (const char* p = path; *p != '\0'; ++p)
        if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\0') &&
            (p == path || p[-1] == '/'))
            return 0;
    return 1;
}

/* Every directory along a path, so a member can be extracted before the
 * archive gets round to mentioning the directory it is in - which happens. */
static void make_parents(const char* path)
{
    char partial[256];
    unsigned long n = 0;
    for (const char* p = path; *p != '\0' && n < sizeof(partial) - 1; ++p) {
        partial[n++] = *p;
        if (*p == '/') {
            partial[n - 1] = '\0';
            if (partial[0] != '\0')
                mkdir(partial);
            partial[n - 1] = '/';
        }
    }
}

int main(int argc, char** argv)
{
    int extract = 0, list = 0, verbose = 0, i = 1;
    for (; i < argc; ++i) {
        if (argv[i][0] != '-' || argv[i][1] == '\0')
            break;
        for (int k = 1; argv[i][k] != '\0'; ++k) {
            switch (argv[i][k]) {
            case 'x': extract = 1; break;
            case 't': list = 1; break;
            case 'v': verbose = 1; break;
            default: printf("tar: unknown option -%c\n", argv[i][k]); return 2;
            }
        }
    }
    if (i >= argc || (!extract && !list)) {
        printf("usage: tar -t|-x [-v] <archive.tar>\n");
        printf("  -t list    -x extract    -v say what is happening\n");
        printf("  for a .tar.gz: gunzip it first\n");
        return 2;
    }

    const int fd = open(argv[i], O_RDONLY);
    if (fd < 0) {
        printf("tar: %s: cannot open\n", argv[i]);
        return 1;
    }

    static unsigned char raw[BLOCK];
    static unsigned char body[BLOCK];
    int zeros = 0, members = 0, status = 0;

    for (;;) {
        if (read(fd, raw, BLOCK) != BLOCK)
            break;
        if (is_zero_block(raw)) {
            /* Two in a row ends the archive; one on its own is padding some
             * writers emit and is not worth complaining about. */
            if (++zeros >= 2)
                break;
            continue;
        }
        zeros = 0;
        if (!checksum_ok(raw)) {
            printf("tar: %s: header checksum is wrong - not a tar?\n", argv[i]);
            status = 1;
            break;
        }

        const struct header* h = (const struct header*)raw;
        char path[256];
        if (h->prefix[0] != '\0' && memcmp(h->magic, "ustar", 5) == 0)
            snprintf(path, sizeof(path), "%.155s/%.100s", h->prefix, h->name);
        else
            snprintf(path, sizeof(path), "%.100s", h->name);

        const unsigned long size = from_octal(h->size, 12);
        const unsigned long blocks = (size + BLOCK - 1) / BLOCK;
        /* '0' and a NUL both mean a regular file; older writers use the NUL. */
        const int is_file = (h->type == '0' || h->type == '\0');
        const int is_dir  = (h->type == '5');
        ++members;

        if (list) {
            if (verbose)
                printf("%c %8lu %s\n", is_dir ? 'd' : is_file ? '-' : '?',
                       size, path);
            else
                printf("%s\n", path);
        }

        if (!extract || (!is_file && !is_dir)) {
            /* Skip the body. Everything that is not a file or a directory - a
             * symlink, a device - is named and passed over rather than
             * guessed at. */
            if (extract && !is_file && !is_dir)
                printf("tar: %s: not a file or directory, skipped\n", path);
            for (unsigned long b = 0; b < blocks; ++b)
                if (read(fd, body, BLOCK) != BLOCK)
                    goto done;
            continue;
        }

        if (!path_is_safe(path)) {
            printf("tar: %s: refusing a path that escapes the directory\n", path);
            status = 1;
            for (unsigned long b = 0; b < blocks; ++b)
                if (read(fd, body, BLOCK) != BLOCK)
                    goto done;
            continue;
        }

        if (is_dir) {
            make_parents(path);
            mkdir(path);
            if (verbose && !list)
                printf("%s\n", path);
            continue;
        }

        make_parents(path);
        const int out = open(path, O_WRONLY | O_CREAT | O_TRUNC);
        if (out < 0) {
            printf("tar: %s: cannot create\n", path);
            status = 1;
        }
        unsigned long left = size;
        for (unsigned long b = 0; b < blocks; ++b) {
            if (read(fd, body, BLOCK) != BLOCK)
                goto done;
            /* The last block is padded; only the real bytes are written. */
            const unsigned long want = left < BLOCK ? left : BLOCK;
            if (out >= 0 && want > 0)
                write(out, body, want);
            left -= want;
        }
        if (out >= 0) {
            close(out);
            chmod(path, (unsigned)from_octal(h->mode, 8) & 0777u);
            if (verbose && !list)
                printf("%s\n", path);
        }
    }
done:
    close(fd);
    if (members == 0) {
        printf("tar: %s: no members - is it gzipped?\n", argv[i]);
        return 1;
    }
    return status;
}
