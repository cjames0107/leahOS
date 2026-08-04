/* wc - count lines, words and characters.
 *
 * A word is a run of anything that is not a space, which is the definition
 * every wc has used since the first one, and it is worth stating because it
 * means punctuation counts as part of a word and a tab does not.
 */

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

struct counts { long lines, words, bytes; };

static void tally(int fd, struct counts* c)
{
    char buffer[1024];
    long n;
    int in_word = 0;
    while ((n = read(fd, buffer, sizeof(buffer))) > 0) {
        for (long i = 0; i < n; ++i) {
            const char ch = buffer[i];
            ++c->bytes;
            if (ch == '\n')
                ++c->lines;
            const int space = (ch == ' ' || ch == '\t' || ch == '\n' ||
                               ch == '\r' || ch == '\f' || ch == '\v');
            if (space) {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                ++c->words;
            }
        }
    }
}

static void show(const struct counts* c, const char* name, int lines_only,
                 int words_only, int bytes_only)
{
    const int all = !lines_only && !words_only && !bytes_only;
    if (all || lines_only) printf("%8ld", c->lines);
    if (all || words_only) printf("%8ld", c->words);
    if (all || bytes_only) printf("%8ld", c->bytes);
    if (name != 0) printf(" %s", name);
    printf("\n");
}

int main(int argc, char** argv)
{
    int lines_only = 0, words_only = 0, bytes_only = 0, first_file = argc;
    int i = 1;
    for (; i < argc; ++i) {
        if (argv[i][0] != '-' || argv[i][1] == '\0')
            break;
        for (int k = 1; argv[i][k] != '\0'; ++k) {
            if (argv[i][k] == 'l') lines_only = 1;
            else if (argv[i][k] == 'w') words_only = 1;
            else if (argv[i][k] == 'c') bytes_only = 1;
            else { printf("wc: unknown option -%c\n", argv[i][k]); return 2; }
        }
    }
    first_file = i;

    if (first_file >= argc) {
        struct counts c = { 0, 0, 0 };
        tally(0, &c);
        show(&c, 0, lines_only, words_only, bytes_only);
        return 0;
    }

    struct counts total = { 0, 0, 0 };
    int status = 0, files = 0;
    for (i = first_file; i < argc; ++i) {
        const int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            printf("wc: %s: cannot open\n", argv[i]);
            status = 1;
            continue;
        }
        struct counts c = { 0, 0, 0 };
        tally(fd, &c);
        close(fd);
        show(&c, argv[i], lines_only, words_only, bytes_only);
        total.lines += c.lines; total.words += c.words; total.bytes += c.bytes;
        ++files;
    }
    if (files > 1)
        show(&total, "total", lines_only, words_only, bytes_only);
    return status;
}
