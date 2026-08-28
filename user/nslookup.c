#include <net.h>
#include <cli.h>
#include <stdio.h>

int main(int argc, char** argv)
{
    cli_begin(argc, argv, "HOSTNAME", "");
    if (cli_argc() != 1)
        cli_usage();
    const char* host = cli_arg(0);

    uint32_t ip;
    if (resolve(host, &ip) < 0) {
        cli_fail("cannot resolve '%s'", host);
        return 1;
    }

    printf("%s has address %u.%u.%u.%u\n", host,
           (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
    return 0;
}
