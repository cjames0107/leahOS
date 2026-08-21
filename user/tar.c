/* tar - list and extract archives.
 *
 * The format itself is in user/libc/archive.c, because there are two programs
 * that read them now - this and the Archiver - and a file format described in
 * two places is a file format that will eventually be described differently in
 * the two places.
 *
 * A gzipped archive is handled here as well, by its magic rather than by its
 * name: `gunzip it first` was a step the library can take on its own, and a
 * .tgz somebody renamed is the same file either way.
 */

#include <archive.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct job {
    int extract;
    int verbose;
    int status;
};

static int member(void* user, const struct ar_entry* e, const unsigned char* body)
{
    struct job* j = (struct job*)user;

    if (!j->extract) {
        if (j->verbose)
            printf("%c %8lu %s\n",
                   e->kind == AR_DIR ? 'd' : e->kind == AR_FILE ? '-' : '?',
                   e->size, e->path);
        else
            printf("%s\n", e->path);
        return 0;
    }

    if (e->kind == AR_OTHER) {
        /* Named and passed over rather than guessed at. */
        printf("tar: %s: not a file or directory, skipped\n", e->path);
        return 0;
    }
    if (ar_extract(e, body, 0) != 0) {
        printf("tar: %s: %s\n", e->path,
               errno == EINVAL ? "refusing a path that escapes the directory"
                               : "cannot create");
        j->status = 1;
        return 0;
    }
    if (j->verbose)
        printf("%s\n", e->path);
    return 0;
}

int main(int argc, char** argv)
{
    struct job j = { 0, 0, 0 };
    int list = 0, i = 1;
    for (; i < argc; ++i) {
        if (argv[i][0] != '-' || argv[i][1] == '\0')
            break;
        for (int k = 1; argv[i][k] != '\0'; ++k) {
            switch (argv[i][k]) {
            case 'x': j.extract = 1; break;
            case 't': list = 1; break;
            case 'v': j.verbose = 1; break;
            default: printf("tar: unknown option -%c\n", argv[i][k]); return 2;
            }
        }
    }
    if (i >= argc || (!j.extract && !list)) {
        printf("usage: tar -t|-x [-v] <archive.tar|archive.tar.gz>\n");
        printf("  -t list    -x extract    -v say what is happening\n");
        return 2;
    }

    unsigned long len = 0;
    unsigned char* data = ar_read(argv[i], &len);
    if (data == 0) {
        printf("tar: %s: cannot read\n", argv[i]);
        return 1;
    }

    const long members = ar_walk(data, len, member, &j);
    free(data);
    if (members < 0) {
        printf("tar: %s: header checksum is wrong - not a tar?\n", argv[i]);
        return 1;
    }
    if (members == 0) {
        printf("tar: %s: no members\n", argv[i]);
        return 1;
    }
    return j.status;
}
