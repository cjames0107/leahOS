#include <net.h>
#include <cli.h>
#include <stdio.h>

int main(int argc, char** argv)
{
    cli_begin(argc, argv, "IP", "");
    if (cli_argc() != 1)
        cli_usage();
    const char* who = cli_arg(0);

    uint32_t ip;
    if (parse_ip(who, &ip) < 0) {
        cli_fail("bad address '%s'", who);
        return 1;
    }

    uint8_t mac[6];
    if (arp(ip, mac) < 0) {
        cli_fail("%s did not answer", who);
        return 1;
    }

    printf("%s is at %02x:%02x:%02x:%02x:%02x:%02x\n",
           who, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return 0;
}
