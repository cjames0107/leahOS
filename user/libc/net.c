#include <net.h>
#include <sys/syscall.h>

int netinfo(struct netinfo* out)
{
    return (int)__syscall(SYS_netinfo, (long)out, 0, 0, 0, 0);
}

int ping(uint32_t ip, uint16_t seq, uint8_t* ttl)
{
    return (int)__syscall(SYS_ping, (long)ip, (long)seq, (long)ttl, 0, 0);
}

int arp(uint32_t ip, uint8_t* mac)
{
    return (int)__syscall(SYS_arp, (long)ip, (long)mac, 0, 0, 0);
}

int resolve(const char* host, uint32_t* ip)
{
    return (int)__syscall(SYS_resolve, (long)host, (long)ip, 0, 0, 0);
}

int tcp_connect(uint32_t ip, uint16_t port)
{
    return (int)__syscall(SYS_connect, (long)ip, (long)port, 0, 0, 0);
}

int parse_ip(const char* text, uint32_t* out)
{
    uint32_t address = 0;
    for (int octet = 0; octet < 4; ++octet) {
        if (*text < '0' || *text > '9')
            return -1;                      /* need at least one digit */
        int value = 0;
        while (*text >= '0' && *text <= '9') {
            value = value * 10 + (*text - '0');
            if (value > 255)
                return -1;
            ++text;
        }
        address = (address << 8) | (uint32_t)value;
        if (octet < 3) {
            if (*text != '.')
                return -1;
            ++text;
        }
    }
    if (*text != '\0')
        return -1;
    *out = address;
    return 0;
}
