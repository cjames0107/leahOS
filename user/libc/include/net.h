#ifndef _NET_H
#define _NET_H

#include <stdint.h>

/* Interface configuration returned by netinfo(). The layout is shared with
 * the kernel's net::Info (kernel/include/leah/net.hpp); change both together. */
struct netinfo {
    uint32_t ip;        /* host order */
    uint32_t gateway;
    uint32_t netmask;
    uint8_t  mac[6];
};

/* Fill *out with the interface configuration. Returns 0, or -1 if there is no
 * network. */
int netinfo(struct netinfo* out);

/* Send one ICMP echo request to `ip` (host order) with the given sequence
 * number. Returns 1 on a reply (writing its IP TTL to *ttl when non-NULL),
 * 0 on timeout, or -1 if there is no network. */
int ping(uint32_t ip, uint16_t seq, uint8_t* ttl);

/* Resolve `ip` (host order) to a MAC in mac[6]. Returns 0, or -1 on failure. */
int arp(uint32_t ip, uint8_t* mac);

/* Parse a dotted-quad "a.b.c.d" into a host-order address. Returns 0, or -1 if
 * the text is not a well-formed IPv4 address. */
int parse_ip(const char* text, uint32_t* out);

/* Resolve a hostname to a host-order IPv4 address over DNS. Returns 0, or -1 on
 * failure. */
int resolve(const char* host, uint32_t* ip);

/* Open a TCP connection to `ip`:`port` and return a file descriptor. read,
 * write and close work on it exactly as on a file or a pipe. -1 on failure. */
int tcp_connect(uint32_t ip, uint16_t port);

#endif /* _NET_H */
