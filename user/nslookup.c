#include <net.h>
#include <stdio.h>

int main(int argc, char** argv)
{
    if (argc != 2) {
        printf("usage: nslookup <hostname>\n");
        return 1;
    }

    uint32_t ip;
    if (resolve(argv[1], &ip) < 0) {
        printf("nslookup: cannot resolve '%s'\n", argv[1]);
        return 1;
    }

    printf("%s has address %u.%u.%u.%u\n", argv[1],
           (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
    return 0;
}
