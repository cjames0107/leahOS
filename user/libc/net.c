/* The network, as a client of the stack rather than of the kernel.
 *
 * Every one of these used to be a system call. They are now messages to netd,
 * which is an ordinary process; the shape of the interface has barely changed,
 * because the shape was never the reason the stack was in the kernel.
 *
 * The port is found once and kept. If netd is not running these all fail,
 * which is the honest answer - there is no network without it.
 */

#include <ipc.h>
#include <net.h>
#include <netd.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static int g_port = -2;         /* -2 not tried, -1 no stack */

static int stack(void)
{
    if (g_port == -2)
        g_port = port_open(IPC_PORT_NET);
    return g_port;
}

static int ask(struct ipc_message* q, struct ipc_message* a)
{
    const int port = stack();
    if (port < 0)
        return -1;
    memset(a, 0, sizeof(*a));
    return ipc_call(port, q, a);
}

int netinfo(struct netinfo* out)
{
    struct ipc_message q, a;
    memset(&q, 0, sizeof(q));
    q.tag = NET_INFO;
    if (out == 0 || ask(&q, &a) != 0)
        return -1;
    out->ip = (uint32_t)a.word[0];
    out->gateway = (uint32_t)a.word[1];
    /* The mask is not carried separately: everything this system talks to is
     * on a /24 or through the gateway, and a field nobody reads is a field
     * that goes wrong quietly. */
    out->netmask = 0xFFFFFF00u;
    memcpy(out->mac, a.data, 6);
    return 0;
}

int ping(uint32_t ip, uint16_t seq, uint8_t* ttl)
{
    struct ipc_message q, a;
    memset(&q, 0, sizeof(q));
    q.tag = NET_PING;
    q.word[0] = (long)ip;
    q.word[1] = (long)seq;
    if (ask(&q, &a) != 0)
        return -1;
    if (a.word[0] < 0)
        return 0;               /* no answer is not the same as no network */
    if (ttl != 0)
        *ttl = (uint8_t)a.word[0];
    return 1;
}

int arp(uint32_t ip, uint8_t* mac)
{
    struct ipc_message q, a;
    memset(&q, 0, sizeof(q));
    q.tag = NET_RESOLVE;
    q.word[0] = (long)ip;
    if (ask(&q, &a) != 0 || a.word[0] != 0)
        return -1;
    if (mac != 0)
        memcpy(mac, a.data, 6);
    return 0;
}

int resolve(const char* host, uint32_t* ip)
{
    struct ipc_message q, a;
    memset(&q, 0, sizeof(q));
    q.tag = NET_LOOKUP;
    unsigned n = 0;
    while (host != 0 && host[n] != '\0' && n < sizeof(q.data) - 1) {
        q.data[n] = host[n];
        ++n;
    }
    q.bytes = n;
    if (n == 0 || ask(&q, &a) != 0 || a.word[0] <= 0)
        return -1;
    if (ip != 0)
        *ip = (uint32_t)a.word[0];
    return 0;
}

int parse_ip(const char* text, uint32_t* out)
{
    uint32_t value = 0;
    int part = 0, digits = 0, octet = 0;
    for (int i = 0; ; ++i) {
        const char c = text[i];
        if (c >= '0' && c <= '9') {
            octet = octet * 10 + (c - '0');
            if (++digits > 3 || octet > 255)
                return -1;
        } else if (c == '.' || c == '\0') {
            if (digits == 0)
                return -1;
            value = (value << 8) | (uint32_t)octet;
            octet = 0;
            digits = 0;
            if (++part == 4 && c == '\0') {
                if (out != 0) *out = value;
                return 0;
            }
            if (part >= 4 || c == '\0')
                return -1;
        } else {
            return -1;
        }
    }
}

int tcp_connect(uint32_t ip, uint16_t port)
{
    struct ipc_message q, a;
    memset(&q, 0, sizeof(q));
    q.tag = NET_TCP_CONNECT;
    q.word[0] = (long)ip;
    q.word[1] = (long)port;
    if (ask(&q, &a) != 0 || a.word[0] < 0)
        return -1;
    return (int)a.word[0];
}

long tcp_read(int connection, void* buffer, unsigned long bytes)
{
    struct ipc_message q, a;
    memset(&q, 0, sizeof(q));
    q.tag = NET_TCP_RECV;
    q.word[0] = connection;
    if (ask(&q, &a) != 0 || a.word[0] < 0)
        return -1;
    unsigned long n = (unsigned long)a.word[0];
    if (n > bytes) n = bytes;
    if (n > 0)
        memcpy(buffer, a.data, n);
    return (long)n;
}

long tcp_write(int connection, const void* buffer, unsigned long bytes)
{
    const unsigned char* p = (const unsigned char*)buffer;
    unsigned long done = 0;
    while (done < bytes) {
        struct ipc_message q, a;
        memset(&q, 0, sizeof(q));
        q.tag = NET_TCP_SEND;
        q.word[0] = connection;
        unsigned n = (unsigned)(bytes - done);
        if (n > sizeof(q.data)) n = sizeof(q.data);
        memcpy(q.data, p + done, n);
        q.bytes = n;
        if (ask(&q, &a) != 0 || a.word[0] < 0)
            return done > 0 ? (long)done : -1;
        if (a.word[0] == 0) {
            /* The last segment has not been acknowledged yet. */
            msleep(5);
            continue;
        }
        done += (unsigned long)a.word[0];
    }
    return (long)done;
}

void tcp_close(int connection)
{
    struct ipc_message q, a;
    memset(&q, 0, sizeof(q));
    q.tag = NET_TCP_CLOSE;
    q.word[0] = connection;
    ask(&q, &a);
}
