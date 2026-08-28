#include <fcntl.h>
#include <cli.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char** argv)
{
    cli_begin(argc, argv, "SRC DST", "");
    if (cli_argc() != 2)
        cli_usage();
    const int in = open(cli_arg(0), O_RDONLY);
    if (in < 0) {
        cli_fail("%s: cannot open", cli_arg(0));
        return 1;
    }
    const int out = open(cli_arg(1), O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) {
        cli_fail("%s: cannot create", cli_arg(1));
        close(in);
        return 1;
    }

    char buffer[512];
    long n;
    while ((n = read(in, buffer, sizeof(buffer))) > 0)
        write(out, buffer, (unsigned long)n);

    close(in);
    close(out);
    return 0;
}
