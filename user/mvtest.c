/* mvtest - move a file back and forth until something breaks.
 *
 * Dragging a file between two folders repeatedly in the Files window takes the
 * app down and eventually the machine with it. Every drag is one rename(), so
 * this is that, on a loop, with the contents checked after every move - which
 * separates "the move lost the data" from "the move corrupted the filesystem".
 *
 * Both directions are exercised: within one directory, where the entry is
 * removed and re-added to the same block, and across two, where one directory
 * loses an entry and another gains one and both have to be written back.
 */

#include <fcntl.h>
#include <cli.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TEXT "the quick brown fox jumps over the lazy dog\n"

static int write_file(const char* path, const char* text)
{
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    long n;
    if (fd < 0)
        return -1;
    n = write(fd, text, strlen(text));
    close(fd);
    return n == (long)strlen(text) ? 0 : -1;
}

/* Read it back and say whether it is still what was written. */
static int intact(const char* path)
{
    char buf[256];
    const int fd = open(path, O_RDONLY);
    long n;
    if (fd < 0)
        return 0;
    memset(buf, 0, sizeof(buf));
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n != (long)strlen(TEXT))
        return 0;
    return strcmp(buf, TEXT) == 0;
}

int main(int argc, char** argv)
{
    cli_begin(argc, argv, "[rounds]", "");
    int r;
    int bad_content = 0, bad_move = 0;

    int rounds = cli_argc() > 0 ? atoi_simple(cli_arg(0)) : 200;
    if (rounds <= 0)
        rounds = 200;

    if (write_file("/root/mv-a.txt", TEXT) != 0) {
        cli_fail("cannot create the file");
        return 1;
    }

    printf("mvtest: %d round trips\n", rounds);
    for (r = 0; r < rounds; ++r) {
        /* Across directories, and back. */
        if (rename("/root/mv-a.txt", "/usr/share/doc/mv-a.txt") != 0) {
            printf("mvtest: round %d: move out failed\n", r);
            ++bad_move;
            break;
        }
        if (!intact("/usr/share/doc/mv-a.txt")) {
            printf("mvtest: round %d: contents wrong after moving out\n", r);
            ++bad_content;
            break;
        }
        if (rename("/usr/share/doc/mv-a.txt", "/root/mv-a.txt") != 0) {
            printf("mvtest: round %d: move back failed\n", r);
            ++bad_move;
            break;
        }
        if (!intact("/root/mv-a.txt")) {
            printf("mvtest: round %d: contents wrong after moving back\n", r);
            ++bad_content;
            break;
        }

        /* And within one directory, which is the other code path. */
        if (rename("/root/mv-a.txt", "/root/mv-b.txt") != 0 ||
            rename("/root/mv-b.txt", "/root/mv-a.txt") != 0) {
            printf("mvtest: round %d: same-directory rename failed\n", r);
            ++bad_move;
            break;
        }
        if (!intact("/root/mv-a.txt")) {
            printf("mvtest: round %d: contents wrong after same-dir rename\n", r);
            ++bad_content;
            break;
        }

        if ((r % 20) == 0)
            printf("mvtest: round %d ok\n", r);
    }

    printf("mvtest: done - %d content failures, %d move failures\n",
           bad_content, bad_move);
    unlink("/root/mv-a.txt");
    return bad_content + bad_move != 0;
}
