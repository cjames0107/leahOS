/* tar, read and written. See archive.h for the shape of it. */

#include <archive.h>
#include <errno.h>
#include <fcntl.h>
#include <inflate.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BLOCK 512

/* The fields this actually uses. ustar puts a prefix at 345 which can carry a
 * longer path; it is joined on the way in, because otherwise a deep archive
 * silently loses its directories. */
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
            break;                      /* space or NUL padded */
        if (c < '0' || c > '7')
            continue;
        value = value * 8 + (unsigned long)(c - '0');
    }
    return value;
}

static void to_octal(char* field, int len, unsigned long value)
{
    /* Right-aligned, zero-padded, with a NUL in the last position - which is
     * what every reader expects even though the format allows a space. */
    for (int i = len - 2; i >= 0; --i) {
        field[i] = (char)('0' + (value & 7));
        value >>= 3;
    }
    field[len - 1] = '\0';
}

/* The header's own checksum: every byte summed, with the checksum field itself
 * counted as spaces. A tar whose sums do not add up is not a tar. */
static unsigned long checksum_of(const unsigned char* raw)
{
    unsigned long sum = 0;
    for (int i = 0; i < BLOCK; ++i)
        sum += (i >= 148 && i < 156) ? ' ' : raw[i];
    return sum;
}

static int checksum_ok(const unsigned char* raw)
{
    const struct header* h = (const struct header*)raw;
    return from_octal(h->checksum, 8) == checksum_of(raw);
}

static int is_zero_block(const unsigned char* raw)
{
    for (int i = 0; i < BLOCK; ++i)
        if (raw[i] != 0)
            return 0;
    return 1;
}

/* --- reading ---------------------------------------------------------------- */

unsigned char* ar_read(const char* path, unsigned long* len)
{
    const int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    unsigned long cap = 65536, n = 0;
    unsigned char* buf = (unsigned char*)malloc(cap);
    if (buf == 0) {
        close(fd);
        errno = ENOMEM;
        return 0;
    }
    for (;;) {
        if (n == cap) {
            unsigned char* grown = (unsigned char*)malloc(cap * 2);
            if (grown == 0) {
                free(buf);
                close(fd);
                errno = ENOMEM;
                return 0;
            }
            memcpy(grown, buf, n);
            free(buf);
            buf = grown;
            cap *= 2;
        }
        const long got = read(fd, &buf[n], cap - n);
        if (got <= 0)
            break;
        n += (unsigned long)got;
    }
    close(fd);

    /* Gzipped, by its magic rather than by its name: a .tgz, a .tar.gz and a
     * file somebody renamed are all the same thing. */
    if (n > 2 && buf[0] == 0x1F && buf[1] == 0x8B) {
        const long want = inflate_gzip_size(buf, n);
        if (want <= 0) {
            free(buf);
            errno = EINVAL;
            return 0;
        }
        unsigned char* out = (unsigned char*)malloc((unsigned long)want);
        if (out == 0) {
            free(buf);
            errno = ENOMEM;
            return 0;
        }
        const long got = inflate_gzip(buf, n, out, (unsigned long)want);
        free(buf);
        if (got < 0) {
            free(out);
            errno = EINVAL;
            return 0;
        }
        buf = out;
        n = (unsigned long)got;
    }

    if (len != 0)
        *len = n;
    return buf;
}

long ar_walk(const unsigned char* data, unsigned long len,
             ar_visit visit, void* user)
{
    if (data == 0 || len < BLOCK)
        return -1;
    unsigned long at = 0;
    int zeros = 0;
    long members = 0;

    while (at + BLOCK <= len) {
        const unsigned char* raw = &data[at];
        if (is_zero_block(raw)) {
            /* Two in a row ends the archive; one on its own is padding some
             * writers emit and is not worth complaining about. */
            at += BLOCK;
            if (++zeros >= 2)
                break;
            continue;
        }
        zeros = 0;
        if (!checksum_ok(raw))
            return members > 0 ? members : -1;

        const struct header* h = (const struct header*)raw;
        struct ar_entry e;
        if (h->prefix[0] != '\0' && memcmp(h->magic, "ustar", 5) == 0)
            snprintf(e.path, sizeof(e.path), "%.155s/%.100s", h->prefix, h->name);
        else
            snprintf(e.path, sizeof(e.path), "%.100s", h->name);
        e.size = from_octal(h->size, 12);
        e.mode = (unsigned)from_octal(h->mode, 8) & 0777u;
        /* '0' and a NUL both mean a regular file; older writers use the NUL. */
        e.kind = (h->type == '0' || h->type == '\0') ? AR_FILE
               : (h->type == '5')                    ? AR_DIR
                                                     : AR_OTHER;
        at += BLOCK;
        e.at = at;

        const unsigned long body = ((e.size + BLOCK - 1) / BLOCK) * BLOCK;
        if (at + (e.kind == AR_DIR ? 0 : e.size) > len)
            return members;             /* truncated: what was read still counts */
        ++members;
        if (visit != 0 && visit(user, &e, &data[at]) != 0)
            return members;
        at += body;
    }
    return members;
}

/* A path an archive is not allowed to choose: absolute, or climbing out with
 * "..". An archive says what its members are called, not where they land. */
static int path_is_safe(const char* path)
{
    if (path[0] == '/' || path[0] == '\0')
        return 0;
    for (const char* p = path; *p != '\0'; ++p)
        if (p[0] == '.' && p[1] == '.' &&
            (p[2] == '/' || p[2] == '\0') &&
            (p == path || p[-1] == '/'))
            return 0;
    return 1;
}

/* Every directory on the way to a file, made in order. */
static void make_parents(const char* path)
{
    char part[256];
    snprintf(part, sizeof(part), "%s", path);
    for (char* p = part + 1; *p != '\0'; ++p) {
        if (*p != '/')
            continue;
        *p = '\0';
        mkdir(part);
        *p = '/';
    }
}

int ar_extract(const struct ar_entry* e, const unsigned char* body,
               const char* into)
{
    if (e == 0 || !path_is_safe(e->path)) {
        errno = EINVAL;
        return -1;
    }
    char full[512];
    if (into != 0 && into[0] != '\0')
        snprintf(full, sizeof(full), "%s/%s", into, e->path);
    else
        snprintf(full, sizeof(full), "%s", e->path);

    if (e->kind == AR_DIR) {
        make_parents(full);
        mkdir(full);
        return 0;
    }
    if (e->kind != AR_FILE) {
        errno = EINVAL;
        return -1;
    }
    make_parents(full);
    const int fd = open(full, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return -1;
    unsigned long left = e->size;
    const unsigned char* at = body;
    while (left > 0) {
        const long wrote = write(fd, at, left);
        if (wrote <= 0) {
            close(fd);
            return -1;
        }
        left -= (unsigned long)wrote;
        at += wrote;
    }
    close(fd);
    chmod(full, e->mode != 0 ? e->mode : 0644u);
    return 0;
}

/* --- writing ---------------------------------------------------------------- */

struct ar_out {
    int fd;
    int failed;
};

struct ar_out* ar_create(const char* path)
{
    struct ar_out* a = (struct ar_out*)malloc(sizeof(struct ar_out));
    if (a == 0)
        return 0;
    a->fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    a->failed = 0;
    if (a->fd < 0) {
        free(a);
        return 0;
    }
    return a;
}

static void put_block(struct ar_out* a, const unsigned char* b)
{
    if (a->failed)
        return;
    if (write(a->fd, b, BLOCK) != BLOCK)
        a->failed = 1;
}

static void put_header(struct ar_out* a, const char* as, unsigned long size,
                       unsigned mode, int is_dir)
{
    unsigned char raw[BLOCK];
    memset(raw, 0, sizeof(raw));
    struct header* h = (struct header*)raw;

    /* A directory's name ends in a slash, which is what tells a reader that
     * had no type field what it was looking at - and what every reader since
     * has gone on expecting. */
    char name[101];
    snprintf(name, sizeof(name), "%s%s", as,
             (is_dir && as[strlen(as) - 1] != '/') ? "/" : "");
    memcpy(h->name, name, strlen(name) < 100 ? strlen(name) : 100);

    to_octal(h->mode, 8, mode & 0777u);
    to_octal(h->uid, 8, 0);
    to_octal(h->gid, 8, 0);
    to_octal(h->size, 12, is_dir ? 0 : size);
    to_octal(h->mtime, 12, 0);
    h->type = is_dir ? '5' : '0';
    memcpy(h->magic, "ustar", 5);
    memcpy(h->version, "00", 2);
    memcpy(h->uname, "root", 4);
    memcpy(h->gname, "root", 4);

    /* Last, because it is the sum of everything above it. */
    to_octal(h->checksum, 8, checksum_of(raw));
    h->checksum[6] = '\0';
    h->checksum[7] = ' ';
    put_block(a, raw);
}

int ar_add(struct ar_out* a, const char* on_disk, const char* as)
{
    if (a == 0)
        return -1;
    struct stat st;
    if (stat(on_disk, &st) != 0)
        return -1;

    if (st.st_type == S_IFDIR) {
        put_header(a, as, 0, st.st_mode, 1);
        return a->failed ? -1 : 0;
    }
    if (st.st_type != S_IFREG)
        return 0;                       /* named and passed over */

    const int fd = open(on_disk, O_RDONLY);
    if (fd < 0)
        return -1;
    put_header(a, as, (unsigned long)st.st_size, st.st_mode, 0);

    unsigned char block[BLOCK];
    unsigned long left = (unsigned long)st.st_size;
    while (left > 0 && !a->failed) {
        memset(block, 0, sizeof(block));
        const unsigned long want = left < BLOCK ? left : BLOCK;
        if (read(fd, block, want) != (long)want) {
            a->failed = 1;
            break;
        }
        put_block(a, block);            /* padded to the block, as tar does */
        left -= want;
    }
    close(fd);
    return a->failed ? -1 : 0;
}

int ar_add_tree(struct ar_out* a, const char* dir, const char* as)
{
    if (a == 0)
        return -1;
    if (ar_add(a, dir, as) != 0)
        return -1;

    /* The listing is taken whole before anything is written, because writing
     * is what makes the archive - and an archive being written into the
     * directory it is archiving would otherwise find itself and grow. */
    struct dirent here[256];
    const int n = getdents(dir, here, 256);
    if (n <= 0)
        return 0;                       /* not a directory, or empty */
    for (int i = 0; i < n && !a->failed; ++i) {
        if (strcmp(here[i].d_name, ".") == 0 ||
            strcmp(here[i].d_name, "..") == 0)
            continue;
        char on_disk[512], under[256];
        snprintf(on_disk, sizeof(on_disk), "%s/%s", dir, here[i].d_name);
        snprintf(under, sizeof(under), "%s/%s", as, here[i].d_name);
        if (here[i].d_type == S_IFDIR)
            ar_add_tree(a, on_disk, under);
        else
            ar_add(a, on_disk, under);
    }
    return a->failed ? -1 : 0;
}

int ar_finish(struct ar_out* a)
{
    if (a == 0)
        return -1;
    unsigned char zero[BLOCK];
    memset(zero, 0, sizeof(zero));
    put_block(a, zero);
    put_block(a, zero);
    const int failed = a->failed;
    if (close(a->fd) != 0)
        return (free(a), -1);
    free(a);
    return failed ? -1 : 0;
}
