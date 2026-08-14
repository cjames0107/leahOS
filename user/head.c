/* head - the first few lines of something.
 *
 * Ported to <cli.h>, and worth comparing against what it was. The option
 * parsing was eight lines that knew "-n10" and "-n 10" are the same thing; the
 * error messages named the program in a string literal; the loop over the
 * files had to skip the option's own value by index. All of that was correct
 * and all of it was this program's copy of it.
 */

#include <cli.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

static void first(int fd, long want, const char* name, int show_name)
{
    if (show_name)
        printf("==> %s <==\n", name);

    char buffer[1024];
    long n, seen = 0;
    while (seen < want && (n = read(fd, buffer, sizeof(buffer))) > 0) {
        for (long i = 0; i < n; ++i) {
            write(1, &buffer[i], 1);
            if (buffer[i] == '\n' && ++seen == want)
                return;
        }
    }
}

int main(int argc, char** argv)
{
    cli_begin(argc, argv, "[-n lines] [file...]");

    const long want = cli_number("-n", 10);
    if (want <= 0)
        return 0;

    /* Nothing named: standard input, which is what makes `... | head` work. */
    if (cli_argc() == 0) {
        first(0, want, 0, 0);
        return 0;
    }

    const int many = cli_argc() > 1;
    int status = 0;
    for (int i = 0; i < cli_argc(); ++i) {
        const char* path = cli_arg(i);
        const int fd = open(path, O_RDONLY);
        if (fd < 0) {
            cli_fail("%s: cannot open it", path);
            status = 1;
            continue;
        }
        if (many && i > 0)
            printf("\n");
        first(fd, want, path, many);
        close(fd);
    }
    return status;
}
