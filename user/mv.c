#include <cli.h>
#include <stdio.h>
#include <unistd.h>

/* A real rename now: one syscall moves the directory entry, no data copy. */
int main(int argc, char** argv)
{
    cli_begin(argc, argv, "SRC DST", "");
    if (cli_argc() != 2)
        cli_usage();
    if (rename(cli_arg(0), cli_arg(1)) < 0) {
        cli_fail("cannot rename %s to %s", cli_arg(0), cli_arg(1));
        return 1;
    }
    return 0;
}
