/* gunzip - decompress a .gz file.
 *
 * The work is libc's: inflate_gzip checks the header, runs the deflate stream
 * and verifies the CRC. What is here is deciding where the output goes, which
 * is the part that is a program rather than a library.
 */

#include <fcntl.h>
#include <inflate.h>
#include <cli.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_INPUT (32u * 1024u * 1024u)

static unsigned char* g_in;

/* "x.gz" becomes "x". Anything else gets ".out", because writing over the
 * input would be the one unrecoverable mistake this program could make. */
static void output_name(const char* in, char* out, unsigned long max)
{
    const unsigned long n = strlen(in);
    if (n > 3 && strcmp(in + n - 3, ".gz") == 0) {
        unsigned long keep = n - 3;
        if (keep >= max) keep = max - 1;
        memcpy(out, in, keep);
        out[keep] = '\0';
        return;
    }
    snprintf(out, max, "%s.out", in);
}

int main(int argc, char** argv)
{
    cli_begin(argc, argv,
              "[-c] [-l] <file.gz>...\n"
              "  -c  write to standard output    -l  list, do not extract",
              "cl");
    const int to_stdout = cli_flag("-c");
    const int list_only = cli_flag("-l");
    if (cli_argc() < 1)
        cli_usage();

    g_in = (unsigned char*)malloc(MAX_INPUT);
    if (g_in == 0) {
        cli_fail("out of memory");
        return 1;
    }

    int status = 0;
    for (int i = 0; i < cli_argc(); ++i) {
        const char* src = cli_arg(i);
        const int fd = open(src, O_RDONLY);
        if (fd < 0) {
            cli_fail("%s: cannot open", src);
            status = 1;
            continue;
        }
        long len = 0, n;
        while ((n = read(fd, g_in + len, MAX_INPUT - (unsigned long)len)) > 0)
            len += n;
        close(fd);

        const long size = inflate_gzip_size(g_in, (unsigned long)len);
        if (size < 0) {
            cli_fail("%s: not a gzip file", src);
            status = 1;
            continue;
        }
        if (list_only) {
            printf("%10ld %10ld  %s\n", len, size, src);
            continue;
        }

        unsigned char* out = (unsigned char*)malloc((unsigned long)size + 1);
        if (out == 0) {
            cli_fail("%s: needs %ld bytes and there are not that many",
                     src, size);
            status = 1;
            continue;
        }
        const long got = inflate_gzip(g_in, (unsigned long)len, out,
                                      (unsigned long)size + 1);
        if (got < 0) {
            /* The CRC is checked, so this covers a truncated file and a
             * corrupted one as well as a malformed stream. */
            cli_fail("%s: damaged, or not really gzip", src);
            status = 1;
            continue;
        }

        if (to_stdout) {
            write(1, out, (unsigned long)got);
            continue;
        }
        char name[256];
        output_name(src, name, sizeof(name));
        const int wfd = open(name, O_WRONLY | O_CREAT | O_TRUNC);
        if (wfd < 0) {
            cli_fail("%s: cannot create", name);
            status = 1;
            continue;
        }
        const long wrote = write(wfd, out, (unsigned long)got);
        close(wfd);
        if (wrote != got) {
            cli_fail("%s: wrote %ld of %ld bytes", name, wrote, got);
            status = 1;
            continue;
        }
        printf("%s -> %s (%ld bytes)\n", src, name, got);
    }
    return status;
}
