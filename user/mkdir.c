#include <cli.h>
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>

int main(int argc, char** argv)
{
    cli_begin(argc, argv, "DIR...", "");
    if (cli_argc() < 1)
        cli_usage();
    int status = 0;
    for (int i = 0; i < cli_argc(); ++i)
        if (mkdir(cli_arg(i)) < 0) {
            cli_fail("%s: %s", cli_arg(i), strerror(errno));
            status = 1;
        }
    return status;
}
