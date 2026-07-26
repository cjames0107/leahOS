#include <net.h>
#include <stdio.h>

int main(int argc, char** argv)
{
    if (argc != 2) {
        printf("usage: arp <ip>\n");
        return 1;
    }

    uint32_t ip;
    if (parse_ip(argv[1], &ip) < 0) {
        printf("arp: bad address '%s'\n", argv[1]);
        return 1;
    }

    uint8_t mac[6];
    if (arp(ip, mac) < 0) {
        printf("arp: %s did not answer\n", argv[1]);
        return 1;
    }

    printf("%s is at %02x:%02x:%02x:%02x:%02x:%02x\n",
           argv[1], mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return 0;
}
