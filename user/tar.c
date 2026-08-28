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
#include <cli.h>
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
        cli_fail("%s: not a file or directory, skipped", e->path);
        return 0;
    }
    if (ar_extract(e, body, 0) != 0) {
        cli_fail("%s: %s", e->path,
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
    cli_begin(argc, argv,
              "-t|-x [-v] <archive.tar|archive.tar.gz>\n"
              "  -t list    -x extract    -v say what is happening",
              "xtv");
    struct job j = { 0, 0, 0 };
    j.extract = cli_flag("-x");
    j.verbose = cli_flag("-v");
    const int list = cli_flag("-t");
    if (cli_argc() < 1 || (!j.extract && !list))
        cli_usage();

    const char* archive = cli_arg(0);
    unsigned long len = 0;
    unsigned char* data = ar_read(archive, &len);
    if (data == 0) {
        cli_fail("%s: cannot read", archive);
        return 1;
    }

    const long members = ar_walk(data, len, member, &j);
    free(data);
    if (members < 0) {
        cli_fail("%s: header checksum is wrong - not a tar?", archive);
        return 1;
    }
    if (members == 0) {
        cli_fail("%s: no members", archive);
        return 1;
    }
    return j.status;
}
