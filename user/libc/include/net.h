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

/* Open a TCP connection to `ip`:`port`. Returns a connection number, or -1.
 *
 * Not a file descriptor any more. The stack runs in another process now, and a
 * descriptor would mean the kernel knowing what a connection is - which is the
 * thing moving the stack out of it was for. So a connection has its own three
 * calls instead of borrowing read, write and close.
 *
 * tcp_read returns the number of bytes written to `buffer`, 0 once the other
 * end has finished, or -1 if the connection failed. */
int  tcp_connect(uint32_t ip, uint16_t port);
long tcp_read(int connection, void* buffer, unsigned long bytes);
long tcp_write(int connection, const void* buffer, unsigned long bytes);
void tcp_close(int connection);

/* The same over IPv6. A connection made this way is read, written and closed
 * with the three calls above: what carried the handshake stops mattering the
 * moment it has finished, which is the whole reason TCP is not two protocols.
 *
 * resolve6 asks for AAAA rather than A and writes sixteen bytes. */
int tcp_connect6(const unsigned char address[16], uint16_t port);
int resolve6(const char* host, unsigned char out[16]);

#endif /* _NET_H */
