#include <net.h>
#include <stdio.h>

int main(int argc, char** argv)
{
    if (argc != 2) {
        printf("usage: ping <host>\n");
        return 1;
    }

    /* Accept a dotted-quad directly, otherwise resolve the name over DNS. */
    uint32_t ip;
    if (parse_ip(argv[1], &ip) < 0) {
        if (resolve(argv[1], &ip) < 0) {
            printf("ping: cannot resolve '%s'\n", argv[1]);
            return 1;
        }
    }

    printf("PING %s (%u.%u.%u.%u): 32 data bytes\n", argv[1],
           (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);

    int sent = 0;
    int received = 0;
    for (uint16_t seq = 1; seq <= 4; ++seq) {
        uint8_t ttl = 0;
        ++sent;
        int result = ping(ip, seq, &ttl);
        if (result < 0) {
            printf("ping: no network\n");
            return 1;
        }
        if (result == 0)
            printf("request timed out: icmp_seq=%u\n", seq);
        else {
            printf("32 bytes from %u.%u.%u.%u: icmp_seq=%u ttl=%u\n",
                   (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF,
                   ip & 0xFF, seq, ttl);
            ++received;
        }
    }

    printf("--- %s ping statistics ---\n", argv[1]);
    printf("%d packets transmitted, %d received\n", sent, received);
    return received == 0 ? 1 : 0;
}
