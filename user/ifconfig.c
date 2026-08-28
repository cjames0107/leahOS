#include <net.h>
#include <cli.h>
#include <stdio.h>

static void print_ip(uint32_t ip)
{
    printf("%u.%u.%u.%u",
           (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
}

int main(int argc, char** argv)
{
    cli_begin(argc, argv, "", "");

    struct netinfo ni;
    if (netinfo(&ni) < 0) {
        cli_fail("no network interface");
        return 1;
    }

    printf("eth0  ether %02x:%02x:%02x:%02x:%02x:%02x\n",
           ni.mac[0], ni.mac[1], ni.mac[2], ni.mac[3], ni.mac[4], ni.mac[5]);

    printf("      inet ");
    print_ip(ni.ip);
    printf("  netmask ");
    print_ip(ni.netmask);
    printf("  gateway ");
    print_ip(ni.gateway);
    printf("\n");
    return 0;
}
