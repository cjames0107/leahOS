#include <cli.h>
#include <stdio.h>
#include <sys/stat.h>

static int parse_uint(const char* text, unsigned* out)
{
    unsigned value = 0;
    if (*text == '\0')
        return -1;
    for (; *text != '\0'; ++text) {
        if (*text < '0' || *text > '9')
            return -1;
        value = value * 10 + (unsigned)(*text - '0');
    }
    *out = value;
    return 0;
}

int main(int argc, char** argv)
{
    cli_begin(argc, argv, "UID FILE", "");
    if (cli_argc() != 2)
        cli_usage();
    unsigned uid;
    if (parse_uint(cli_arg(0), &uid) < 0) {
        cli_fail("bad uid '%s'", cli_arg(0));
        return 1;
    }
    /* -1 leaves the group alone. */
    if (chown(cli_arg(1), uid, (unsigned)-1) < 0) {
        cli_fail("cannot change '%s' (are you root?)", cli_arg(1));
        return 1;
    }
    return 0;
}
