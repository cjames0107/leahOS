#include <cli.h>
#include <stdio.h>
#include <sys/stat.h>

/* Octal only: the symbolic form (u+rw) is a parser for little gain here. */
static int parse_octal(const char* text, unsigned* out)
{
    unsigned value = 0;
    if (*text == '\0')
        return -1;
    for (; *text != '\0'; ++text) {
        if (*text < '0' || *text > '7')
            return -1;
        value = value * 8 + (unsigned)(*text - '0');
    }
    if (value > 0777)
        return -1;
    *out = value;
    return 0;
}

int main(int argc, char** argv)
{
    cli_begin(argc, argv, "OCTAL-MODE FILE", "");
    if (cli_argc() != 2)
        cli_usage();
    unsigned mode;
    if (parse_octal(cli_arg(0), &mode) < 0) {
        cli_fail("bad mode '%s'", cli_arg(0));
        return 1;
    }
    if (chmod(cli_arg(1), mode) < 0) {
        cli_fail("cannot change '%s'", cli_arg(1));
        return 1;
    }
    return 0;
}
