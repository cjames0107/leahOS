#include <net.h>
#include <stdio.h>

int main(int argc, char** argv)
{
    if (argc != 2) {
        printf("usage: ping <ip>\n");
        return 1;
    }

    uint32_t ip;
    if (parse_ip(argv[1], &ip) < 0) {
        printf("ping: bad address '%s'\n", argv[1]);
        return 1;
    }

    printf("PING %s: 32 data bytes\n", argv[1]);

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
            printf("32 bytes from %s: icmp_seq=%u ttl=%u\n", argv[1], seq, ttl);
            ++received;
        }
    }

    printf("--- %s ping statistics ---\n", argv[1]);
    printf("%d packets transmitted, %d received\n", sent, received);
    return received == 0 ? 1 : 0;
}
