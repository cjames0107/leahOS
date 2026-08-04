/* gunzip - decompress a .gz file.
 *
 * The work is libc's: inflate_gzip checks the header, runs the deflate stream
 * and verifies the CRC. What is here is deciding where the output goes, which
 * is the part that is a program rather than a library.
 */

#include <fcntl.h>
#include <inflate.h>
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
    int to_stdout = 0, list_only = 0, i = 1;
    for (; i < argc; ++i) {
        if (argv[i][0] != '-' || argv[i][1] == '\0')
            break;
        for (int k = 1; argv[i][k] != '\0'; ++k) {
            if (argv[i][k] == 'c')      to_stdout = 1;
            else if (argv[i][k] == 'l') list_only = 1;
            else { printf("gunzip: unknown option -%c\n", argv[i][k]); return 2; }
        }
    }
    if (i >= argc) {
        printf("usage: gunzip [-c] [-l] <file.gz>...\n");
        printf("  -c  write to standard output    -l  list, do not extract\n");
        return 2;
    }

    g_in = (unsigned char*)malloc(MAX_INPUT);
    if (g_in == 0) {
        printf("gunzip: out of memory\n");
        return 1;
    }

    int status = 0;
    for (; i < argc; ++i) {
        const int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            printf("gunzip: %s: cannot open\n", argv[i]);
            status = 1;
            continue;
        }
        long len = 0, n;
        while ((n = read(fd, g_in + len, MAX_INPUT - (unsigned long)len)) > 0)
            len += n;
        close(fd);

        const long size = inflate_gzip_size(g_in, (unsigned long)len);
        if (size < 0) {
            printf("gunzip: %s: not a gzip file\n", argv[i]);
            status = 1;
            continue;
        }
        if (list_only) {
            printf("%10ld %10ld  %s\n", len, size, argv[i]);
            continue;
        }

        unsigned char* out = (unsigned char*)malloc((unsigned long)size + 1);
        if (out == 0) {
            printf("gunzip: %s: needs %ld bytes and there are not that many\n",
                   argv[i], size);
            status = 1;
            continue;
        }
        const long got = inflate_gzip(g_in, (unsigned long)len, out,
                                      (unsigned long)size + 1);
        if (got < 0) {
            /* The CRC is checked, so this covers a truncated file and a
             * corrupted one as well as a malformed stream. */
            printf("gunzip: %s: damaged, or not really gzip\n", argv[i]);
            status = 1;
            continue;
        }

        if (to_stdout) {
            write(1, out, (unsigned long)got);
            continue;
        }
        char name[256];
        output_name(argv[i], name, sizeof(name));
        const int wfd = open(name, O_WRONLY | O_CREAT | O_TRUNC);
        if (wfd < 0) {
            printf("gunzip: %s: cannot create\n", name);
            status = 1;
            continue;
        }
        const long wrote = write(wfd, out, (unsigned long)got);
        close(wfd);
        if (wrote != got) {
            printf("gunzip: %s: wrote %ld of %ld bytes\n", name, wrote, got);
            status = 1;
            continue;
        }
        printf("%s -> %s (%ld bytes)\n", argv[i], name, got);
    }
    return status;
}
