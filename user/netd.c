/* netd - the network stack, outside the kernel.
 *
 * It owns no hardware. Frames reach it through e1000d, which owns the card,
 * and it reaches its own callers through a port. Three processes end to end,
 * none of them able to touch another's memory: a program asks netd, netd asks
 * the driver, the driver asks the card.
 *
 * Single-threaded on purpose. A request that cannot be answered yet - a ping,
 * an address that has to be resolved first - is put aside with its reply
 * handle and answered when the frame it was waiting for arrives. A server that
 * blocked instead would stop draining the card, which is the one thing it must
 * always be doing.
 */

#include <ipc.h>
#include <netd.h>
#include <nic.h>
#include <shm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* What to fall back to when nothing answers. QEMU's user-mode network always
 * hands out exactly this, so a machine with no DHCP server still works - but
 * asking first is the difference between a stack and a set of constants. */
#define FALLBACK_IP   0x0A00020F      /* 10.0.2.15 */
#define FALLBACK_MASK 0xFFFFFF00
#define FALLBACK_GW   0x0A000202      /* 10.0.2.2  */
#define FALLBACK_DNS  0x0A000203      /* 10.0.2.3  */

#define ETH_ARP 0x0806
#define ETH_IP  0x0800
#define IP_ICMP 1
#define IP_UDP  17

/* Learned from DHCP, or fallen back to. Zero until one or the other has
 * happened, which is why nothing is sent before the lease is in. */
static unsigned g_ip, g_mask, g_gw, g_dns;
static int g_configured;

static int g_nic_port;
static struct nic_shared* g_net;
static uint8_t g_mac[6];
static unsigned long g_in, g_out;

/* --- reading and writing the awkward sizes -------------------------------- */

static unsigned rd16(const uint8_t* p) { return (unsigned)p[0] << 8 | p[1]; }
static unsigned rd32(const uint8_t* p)
{
    return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 |
           (unsigned)p[2] << 8  | p[3];
}
static void wr16(uint8_t* p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void wr32(uint8_t* p, unsigned v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* The one's-complement sum every header in this family is checked with. */
static unsigned checksum(const uint8_t* data, unsigned length)
{
    unsigned sum = 0;
    for (unsigned i = 0; i + 1 < length; i += 2)
        sum += rd16(data + i);
    if (length & 1)
        sum += (unsigned)data[length - 1] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (~sum) & 0xFFFF;
}

static int send_frame(const uint8_t* frame, unsigned length)
{
    memcpy((void*)g_net->tx.data, frame, length);
    struct ipc_message q, a;
    memset(&q, 0, sizeof(q));
    memset(&a, 0, sizeof(a));
    q.tag = NIC_SEND;
    q.word[0] = (long)length;
    if (ipc_call(g_nic_port, &q, &a) != 0 || a.word[0] != 0)
        return -1;
    ++g_out;
    return 0;
}

/* --- what we know about who is where -------------------------------------- */

#define ARP_MAX 16
static struct { unsigned ip; uint8_t mac[6]; int used; } g_arp[ARP_MAX];

static void arp_learn(unsigned ip, const uint8_t* mac)
{
    for (int i = 0; i < ARP_MAX; ++i)
        if (g_arp[i].used && g_arp[i].ip == ip) {
            memcpy(g_arp[i].mac, mac, 6);
            return;
        }
    for (int i = 0; i < ARP_MAX; ++i)
        if (!g_arp[i].used) {
            g_arp[i].used = 1;
            g_arp[i].ip = ip;
            memcpy(g_arp[i].mac, mac, 6);
            return;
        }
}

static const uint8_t* arp_lookup(unsigned ip)
{
    for (int i = 0; i < ARP_MAX; ++i)
        if (g_arp[i].used && g_arp[i].ip == ip)
            return g_arp[i].mac;
    return 0;
}

static int arp_count(void)
{
    int n = 0;
    for (int i = 0; i < ARP_MAX; ++i) n += g_arp[i].used;
    return n;
}

static void arp_ask(unsigned ip)
{
    uint8_t f[42];
    memset(f, 0, sizeof(f));
    memset(f, 0xFF, 6);
    memcpy(f + 6, g_mac, 6);
    wr16(f + 12, ETH_ARP);
    wr16(f + 14, 1); wr16(f + 16, ETH_IP);
    f[18] = 6; f[19] = 4;
    wr16(f + 20, 1);
    memcpy(f + 22, g_mac, 6);
    wr32(f + 28, g_ip);
    wr32(f + 38, ip);
    send_frame(f, sizeof(f));
}

/* Someone asked who we are. Answering is not optional: a host that never
 * replies to ARP is a host nothing can send to. */
static void arp_answer(const uint8_t* req)
{
    uint8_t f[42];
    memset(f, 0, sizeof(f));
    memcpy(f, req + 22, 6);             /* back to whoever asked */
    memcpy(f + 6, g_mac, 6);
    wr16(f + 12, ETH_ARP);
    wr16(f + 14, 1); wr16(f + 16, ETH_IP);
    f[18] = 6; f[19] = 4;
    wr16(f + 20, 2);                    /* a reply */
    memcpy(f + 22, g_mac, 6);
    wr32(f + 28, g_ip);
    memcpy(f + 32, req + 22, 6);
    wr32(f + 38, rd32(req + 28));
    send_frame(f, sizeof(f));
}

/* --- requests that cannot be answered yet --------------------------------- */

#define PENDING_MAX 8
#define WAIT_ARP  1
#define WAIT_PING 2
#define WAIT_DNS  3     /* a name being looked up */
#define WAIT_UDP  4     /* a datagram being waited for on a port */
#define WAIT_CONNECT 5  /* a handshake that has not finished */
#define WAIT_READ    6  /* a reader with nothing yet to read */
#define WAIT_TCP_ARP 7  /* a connection that needs an address first */
#define WAIT_ND    8    /* a neighbour being solicited over IPv6 */
#define WAIT_PING6 9    /* an ICMPv6 echo that has not come back */

static struct pending {
    int used;
    int kind;
    int handle;
    unsigned ip;
    unsigned seq;           /* also the DNS id, and the UDP port */
    unsigned deadline;      /* in loop ticks */
    int      want6;         /* a lookup asking for AAAA rather than A */
    int      over6;         /* and one whose question travels over IPv6 */
    char     name[128];     /* the host being looked up, or 16 address bytes */
} g_pending[PENDING_MAX];

static unsigned g_ticks;

struct conn;
static struct conn* conn_find(unsigned local_port);
static void conn_close(struct conn* c);

static struct pending* pending_add(int kind, int handle, unsigned ip, unsigned seq)
{
    for (int i = 0; i < PENDING_MAX; ++i)
        if (!g_pending[i].used) {
            g_pending[i].used = 1;
            g_pending[i].kind = kind;
            g_pending[i].handle = handle;
            g_pending[i].ip = ip;
            g_pending[i].seq = seq;
            g_pending[i].deadline = g_ticks + 2000;   /* two seconds of ticks */
            g_pending[i].name[0] = '\0';
            g_pending[i].want6 = 0;
            g_pending[i].over6 = 0;
            return &g_pending[i];
        }
    return 0;
}

static void answer(struct pending* p, long w0, const uint8_t* data, unsigned bytes)
{
    struct ipc_message r;
    memset(&r, 0, sizeof(r));
    r.word[0] = w0;
    if (data != 0 && bytes > 0) {
        memcpy(r.data, data, bytes);
        r.bytes = bytes;
    }
    ipc_reply(p->handle, &r);
    p->used = 0;
}

/* Ask again over IPv6 when the IPv4 resolver did not answer. Defined further
 * down, where the v6 addresses and routing exist; declared here because this is
 * where a question runs out of time. Returns true when it has taken the query
 * on and it should not be failed yet. */
static int dns_retry_over_v6(struct pending* p);

static void expire_pending(void)
{
    for (int i = 0; i < PENDING_MAX; ++i) {
        if (!g_pending[i].used || g_ticks < g_pending[i].deadline)
            continue;
        if (g_pending[i].kind == WAIT_CONNECT ||
            g_pending[i].kind == WAIT_TCP_ARP) {
            /* Nothing answered; do not leave a half-open connection behind. */
            conn_close(conn_find(g_pending[i].ip));
        }
        /* A name that got no answer over v4 is worth one more try over v6 -
         * which is what "v4 first, v6 as the fallback" was always supposed to
         * mean. It fell back only when there was no v4 resolver *configured*,
         * never when the configured one failed to reply, and on a v6-only
         * network netd invents a v4 resolver anyway. So every lookup went to
         * an address nothing was listening at, and DNS simply did not work. */
        if (g_pending[i].kind == WAIT_DNS && dns_retry_over_v6(&g_pending[i]))
            continue;
        answer(&g_pending[i], -1, 0, 0);
    }
}

/* --- ICMP ----------------------------------------------------------------- */

static void send_ping(unsigned ip, unsigned seq, const uint8_t* mac)
{
    uint8_t f[14 + 20 + 16];
    memset(f, 0, sizeof(f));
    memcpy(f, mac, 6);
    memcpy(f + 6, g_mac, 6);
    wr16(f + 12, ETH_IP);

    uint8_t* ip_hdr = f + 14;
    ip_hdr[0] = 0x45;
    wr16(ip_hdr + 2, 20 + 16);
    wr16(ip_hdr + 4, seq);              /* identification */
    ip_hdr[8] = 64;                     /* time to live */
    ip_hdr[9] = IP_ICMP;
    wr32(ip_hdr + 12, g_ip);
    wr32(ip_hdr + 16, ip);
    wr16(ip_hdr + 10, checksum(ip_hdr, 20));

    uint8_t* icmp = ip_hdr + 20;
    icmp[0] = 8;                        /* echo request */
    wr16(icmp + 4, 0x4C45);             /* an identifier: "LE" */
    wr16(icmp + 6, seq);
    for (int i = 0; i < 8; ++i)
        icmp[8 + i] = (uint8_t)('a' + i);
    wr16(icmp + 2, checksum(icmp, 16));

    send_frame(f, sizeof(f));
}

/* An IPv6 address. Defined here rather than with the rest of IPv6 because DNS
 * sits above both families and has to name one before either exists. */
struct in6 { uint8_t b[16]; };

/* Link-local is fe80::/10 - the top ten bits, not the top eight.
 *
 * Testing only b[0] == 0xFE calls fec0::/10 link-local too, and this network's
 * router and resolver both live there. The consequences were quiet: a packet
 * to fec0::3 was sent from the link-local address rather than the global one,
 * and was never handed to the router because it looked like it was already on
 * the link. ICMP survived that; a DNS query did not. */
static int is_link_local(const struct in6* a)
{
    return a->b[0] == 0xFE && (a->b[1] & 0xC0) == 0x80;
}
static int send_udp6(const struct in6* dst, const uint8_t* dst_mac,
                     unsigned src_port, unsigned dst_port,
                     const uint8_t* payload, unsigned length);
static const uint8_t* route6(const struct in6* to, struct in6* via_out);

/* --- UDP ------------------------------------------------------------------
 *
 * A datagram is an IP packet with four extra fields, which is the whole reason
 * DHCP and DNS are built on it: neither wants a connection, both want one
 * question and one answer, and anything more would be machinery in the way.
 */

static int send_udp(unsigned dst_ip, const uint8_t* dst_mac,
                    unsigned src_port, unsigned dst_port,
                    const uint8_t* payload, unsigned length)
{
    /* Static, not automatic. This is reached from inside the receive path,
     * which already has a frame of its own on the stack, and a chain of
     * kilobyte buffers is how a stack quietly runs out. Safe because this
     * server is single-threaded and does one thing at a time - which is also
     * why it can hold a request aside instead of blocking. */
    static uint8_t f[1536];
    if (length > sizeof(f) - 42)        /* not length + 42: that can wrap */
        return -1;
    memset(f, 0, 42);
    memcpy(f, dst_mac, 6);
    memcpy(f + 6, g_mac, 6);
    wr16(f + 12, ETH_IP);

    uint8_t* ip_hdr = f + 14;
    ip_hdr[0] = 0x45;
    wr16(ip_hdr + 2, 28 + length);
    ip_hdr[8] = 64;
    ip_hdr[9] = IP_UDP;
    wr32(ip_hdr + 12, g_ip);
    wr32(ip_hdr + 16, dst_ip);
    wr16(ip_hdr + 10, checksum(ip_hdr, 20));

    uint8_t* udp = ip_hdr + 20;
    wr16(udp + 0, src_port);
    wr16(udp + 2, dst_port);
    wr16(udp + 4, 8 + length);
    /* The checksum is optional over IPv4 and zero means "not computed". It is
     * left out here because every datagram this stack sends is also protected
     * by the Ethernet CRC, and the one thing worse than no checksum is a wrong
     * one. */
    wr16(udp + 6, 0);
    memcpy(udp + 8, payload, length);
    return send_frame(f, 42 + length);
}

/* --- DHCP -----------------------------------------------------------------
 *
 * Four messages: we ask if anyone is there, a server offers an address, we ask
 * for that one specifically, and it confirms. The middle two exist so that a
 * network with two servers settles on one answer rather than two.
 */

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5

static unsigned g_xid = 0x1EA40501;
static unsigned g_dhcp_server;
static unsigned g_offered;
static int g_dhcp_state;            /* which message we are waiting for */

static void dhcp_send(int kind)
{
    static uint8_t p[300];
    memset(p, 0, sizeof(p));
    p[0] = 1;                       /* a request, from a client */
    p[1] = 1;                       /* over Ethernet */
    p[2] = 6;                       /* six bytes of it */
    wr32(p + 4, g_xid);
    wr16(p + 10, 0x8000);           /* answer by broadcast: we have no address */
    memcpy(p + 28, g_mac, 6);
    wr32(p + 236, 0x63825363);      /* the magic that says options follow */

    unsigned at = 240;
    p[at++] = 53; p[at++] = 1; p[at++] = (uint8_t)kind;
    if (kind == DHCP_REQUEST) {
        p[at++] = 50; p[at++] = 4; wr32(p + at, g_offered); at += 4;
        p[at++] = 54; p[at++] = 4; wr32(p + at, g_dhcp_server); at += 4;
    }
    /* What we would like to be told: netmask, router, name servers. */
    p[at++] = 55; p[at++] = 3; p[at++] = 1; p[at++] = 3; p[at++] = 6;
    p[at++] = 255;

    static const uint8_t broadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    const unsigned saved = g_ip;
    g_ip = 0;                       /* we do not have one yet, and must say so */
    send_udp(0xFFFFFFFFu, broadcast, 68, 67, p, at);
    g_ip = saved;
}

static void configure(unsigned ip, unsigned mask, unsigned gw, unsigned dns)
{
    g_ip = ip;
    g_mask = mask ? mask : FALLBACK_MASK;
    g_gw = gw;
    g_dns = dns ? dns : FALLBACK_DNS;
    g_configured = 1;
    printf("netd: address %u.%u.%u.%u\n",
           (g_ip >> 24) & 0xFF, (g_ip >> 16) & 0xFF,
           (g_ip >> 8) & 0xFF, g_ip & 0xFF);
    printf("netd: gateway %u.%u.%u.%u\n",
           (g_gw >> 24) & 0xFF, (g_gw >> 16) & 0xFF,
           (g_gw >> 8) & 0xFF, g_gw & 0xFF);
    printf("netd: resolver %u.%u.%u.%u\n",
           (g_dns >> 24) & 0xFF, (g_dns >> 16) & 0xFF,
           (g_dns >> 8) & 0xFF, g_dns & 0xFF);
}

static void dhcp_receive(const uint8_t* p, unsigned length)
{
    if (length < 240 || rd32(p + 4) != g_xid || rd32(p + 236) != 0x63825363)
        return;

    int kind = 0;
    unsigned mask = 0, router = 0, dns = 0, server = 0;
    unsigned at = 240;
    while (at + 1 < length) {
        const unsigned code = p[at];
        if (code == 255)
            break;
        if (code == 0) { ++at; continue; }
        const unsigned len = p[at + 1];
        const uint8_t* v = p + at + 2;
        if (at + 2 + len > length)
            break;
        if (code == 53 && len >= 1)      kind = v[0];
        else if (code == 1 && len >= 4)  mask = rd32(v);
        else if (code == 3 && len >= 4)  router = rd32(v);
        else if (code == 6 && len >= 4)  dns = rd32(v);
        else if (code == 54 && len >= 4) server = rd32(v);
        at += 2 + len;
    }

    if (kind == DHCP_OFFER && g_dhcp_state == DHCP_OFFER) {
        g_offered = rd32(p + 16);       /* yiaddr: the address being offered */
        g_dhcp_server = server;
        g_dhcp_state = DHCP_ACK;
        dhcp_send(DHCP_REQUEST);
    } else if (kind == DHCP_ACK && g_dhcp_state == DHCP_ACK) {
        g_dhcp_state = 0;
        configure(rd32(p + 16), mask, router, dns);
    }
}

/* --- DNS ------------------------------------------------------------------ */

#define DNS_PORT 53
static unsigned g_dns_id = 0x4C45;

/* "www.example.com" becomes 3www7example3com0, which is the only encoding a
 * name has on the wire. */
static unsigned encode_name(const char* host, uint8_t* out)
{
    unsigned at = 0, label = 0;
    out[at++] = 0;                          /* the first length, filled below */
    for (unsigned i = 0; ; ++i) {
        if (host[i] == '.' || host[i] == '\0') {
            out[at - label - 1] = (uint8_t)label;
            if (host[i] == '\0')
                break;
            label = 0;
            out[at++] = 0;
        } else {
            out[at++] = (uint8_t)host[i];
            ++label;
        }
    }
    out[at++] = 0;
    return at;
}

/* `want6` asks for AAAA rather than A. The question is otherwise identical -
 * a record type is one number, which is most of why the same resolver serves
 * both families without knowing anything about either. */
static void dns_ask(const char* host, unsigned id, const uint8_t* mac, int want6)
{
    static uint8_t q[300];
    memset(q, 0, sizeof(q));
    wr16(q + 0, id);
    wr16(q + 2, 0x0100);                    /* a query, please recurse */
    wr16(q + 4, 1);                         /* one question */
    unsigned at = 12 + encode_name(host, q + 12);
    wr16(q + at, want6 ? 28 : 1); at += 2;  /* AAAA or A */
    wr16(q + at, 1); at += 2;               /* on the internet */
    send_udp(g_dns, mac, 40000 + (id & 0xFF), DNS_PORT, q, at);
}

/* The same question carried over IPv6 rather than IPv4, to a resolver that has
 * a v6 address. Which transport a lookup travels over and which family it asks
 * about are independent, and conflating them is how a stack ends up unable to
 * find an A record when only v6 works. */
static void dns_ask6(const char* host, unsigned id, const struct in6* server,
                     const uint8_t* mac, int want6)
{
    static uint8_t q[300];
    memset(q, 0, sizeof(q));
    wr16(q + 0, id);
    wr16(q + 2, 0x0100);
    wr16(q + 4, 1);
    unsigned at = 12 + encode_name(host, q + 12);
    wr16(q + at, want6 ? 28 : 1); at += 2;
    wr16(q + at, 1); at += 2;
    send_udp6(server, mac, 40000 + (id & 0xFF), DNS_PORT, q, at);
}

/* Skip a name, which may end in a pointer to one earlier in the packet. */
static unsigned skip_name(const uint8_t* p, unsigned at, unsigned length)
{
    while (at < length) {
        const unsigned len = p[at];
        if (len == 0)
            return at + 1;
        if ((len & 0xC0) == 0xC0)
            return at + 2;                  /* a pointer, and always the end */
        at += 1 + len;
    }
    return length;
}

/* Pull the first address of the wanted type out of an answer. A four-byte
 * record is written to `v4`, a sixteen-byte one to `v6`; the caller knows
 * which it asked for. */
static int dns_answer6(const uint8_t* p, unsigned length, unsigned* id_out,
                       struct in6* v6)
{
    if (length < 12)
        return 0;
    *id_out = rd16(p);
    const unsigned questions = rd16(p + 4), answers = rd16(p + 6);
    if ((rd16(p + 2) & 0x000F) != 0)
        return 0;
    unsigned at = 12;
    for (unsigned i = 0; i < questions; ++i)
        at = skip_name(p, at, length) + 4;
    for (unsigned i = 0; i < answers && at + 10 <= length; ++i) {
        at = skip_name(p, at, length);
        const unsigned type = rd16(p + at);
        const unsigned len = rd16(p + at + 8);
        at += 10;
        if (type == 28 && len == 16 && at + 16 <= length) {
            memcpy(v6->b, p + at, 16);
            return 1;
        }
        at += len;
    }
    return 0;
}

static unsigned dns_answer(const uint8_t* p, unsigned length, unsigned* id_out)
{
    if (length < 12)
        return 0;
    *id_out = rd16(p);
    const unsigned questions = rd16(p + 4), answers = rd16(p + 6);
    if ((rd16(p + 2) & 0x000F) != 0)
        return 0;                           /* the server said no */

    unsigned at = 12;
    for (unsigned i = 0; i < questions; ++i)
        at = skip_name(p, at, length) + 4;
    for (unsigned i = 0; i < answers && at + 10 <= length; ++i) {
        at = skip_name(p, at, length);
        const unsigned type = rd16(p + at);
        const unsigned len = rd16(p + at + 8);
        at += 10;
        if (type == 1 && len == 4 && at + 4 <= length)
            return rd32(p + at);            /* the first address will do */
        at += len;
    }
    return 0;
}

/* --- TCP ------------------------------------------------------------------
 *
 * Enough of it to be a client: connect, send, receive, close, and retransmit
 * what has not been acknowledged. Listening is not here because nothing in this
 * system listens yet, and a half-written server side would be a thing to
 * maintain rather than a thing that works.
 *
 * Only in-order data is accepted. A segment that arrives out of order is
 * dropped and the sender will send it again, which is correct but slow -
 * reassembly is the obvious next thing and is not needed to be right.
 */

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

#define IP_TCP 6

#define TCP_CLOSED      0
#define TCP_SYN_SENT    1
#define TCP_ESTABLISHED 2
#define TCP_FIN_WAIT    3
#define TCP_DEAD        4

#define CONN_MAX 8
#define TCP_RX   8192
#define TCP_SEG  1400       /* under the 1500-byte link with room for headers */

static struct conn {
    int used;
    int state;
    int peer_closed;
    unsigned peer_ip, peer_port, local_port;
    int      v6;                    /* the peer is an IPv6 address */
    struct in6 peer6;
    unsigned snd_next;              /* the next sequence number we will use  */
    unsigned rcv_next;              /* the next one we expect from them      */
    uint8_t  rx[TCP_RX];
    unsigned rx_len;

    /* One segment in flight at a time. A window of one is slow and is not
     * wrong, and the alternative - a real send queue - is a lot of machinery
     * to add before anything has complained about the speed. */
    uint8_t  out[TCP_SEG];
    unsigned out_len, out_seq;
    unsigned out_deadline;
    int      out_tries;
} g_conn[CONN_MAX];

static unsigned g_next_port = 40000;

/* TCP's checksum covers a header that is not in the packet: the addresses, the
 * protocol and the length, so that a segment delivered to the wrong host or
 * the wrong protocol fails the check rather than being accepted. */
static unsigned tcp_checksum(unsigned src, unsigned dst,
                             const uint8_t* segment, unsigned length)
{
    unsigned sum = 0;
    sum += (src >> 16) & 0xFFFF; sum += src & 0xFFFF;
    sum += (dst >> 16) & 0xFFFF; sum += dst & 0xFFFF;
    sum += IP_TCP;
    sum += length;
    for (unsigned i = 0; i + 1 < length; i += 2)
        sum += rd16(segment + i);
    if (length & 1)
        sum += (unsigned)segment[length - 1] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (~sum) & 0xFFFF;
}

static int tcp_send6(struct conn* c, unsigned flags, unsigned seq,
                     const uint8_t* payload, unsigned length);

static int tcp_send(struct conn* c, unsigned flags, unsigned seq,
                    const uint8_t* payload, unsigned length)
{
    if (c->v6)
        return tcp_send6(c, flags, seq, payload, length);
    const uint8_t* mac = arp_lookup(
        ((c->peer_ip ^ g_ip) & g_mask) ? g_gw : c->peer_ip);
    if (mac == 0)
        return -1;

    static uint8_t f[1536];
    memset(f, 0, 54);
    memcpy(f, mac, 6);
    memcpy(f + 6, g_mac, 6);
    wr16(f + 12, ETH_IP);

    uint8_t* ip_hdr = f + 14;
    ip_hdr[0] = 0x45;
    wr16(ip_hdr + 2, 40 + length);
    ip_hdr[8] = 64;
    ip_hdr[9] = IP_TCP;
    wr32(ip_hdr + 12, g_ip);
    wr32(ip_hdr + 16, c->peer_ip);
    wr16(ip_hdr + 10, checksum(ip_hdr, 20));

    uint8_t* t = ip_hdr + 20;
    wr16(t + 0, c->local_port);
    wr16(t + 2, c->peer_port);
    wr32(t + 4, seq);
    wr32(t + 8, c->rcv_next);
    wr16(t + 12, (5u << 12) | flags);           /* five words of header */
    wr16(t + 14, TCP_RX - c->rx_len);           /* what we can still take */
    if (length > 0)
        memcpy(t + 20, payload, length);
    wr16(t + 16, tcp_checksum(g_ip, c->peer_ip, t, 20 + length));

    return send_frame(f, 54 + length);
}

static struct conn* conn_find(unsigned local_port)
{
    for (int i = 0; i < CONN_MAX; ++i)
        if (g_conn[i].used && g_conn[i].local_port == local_port)
            return &g_conn[i];
    return 0;
}

static void conn_close(struct conn* c)
{
    if (c != 0)
        c->used = 0;
}

static void conn_retransmit(void)
{
    for (int i = 0; i < CONN_MAX; ++i) {
        struct conn* c = &g_conn[i];
        if (!c->used || c->out_len == 0 || g_ticks < c->out_deadline)
            continue;
        if (c->state == TCP_SYN_SENT) {
            /* Still handshaking: what needs repeating is the SYN, and its
             * sequence number is the one before the next. */
            if (++c->out_tries > 8) {
                c->state = TCP_DEAD;
                c->out_len = 0;
                continue;
            }
            tcp_send(c, TCP_SYN, c->snd_next - 1, 0, 0);
            c->out_deadline = g_ticks + 200 * (unsigned)c->out_tries;
            continue;
        }
        if (++c->out_tries > 5) {
            /* Five goes and no acknowledgement. The other end is gone. */
            c->state = TCP_DEAD;
            c->out_len = 0;
            continue;
        }
        tcp_send(c, TCP_ACK | TCP_PSH, c->out_seq, c->out, c->out_len);
        c->out_deadline = g_ticks + 400 * (unsigned)c->out_tries;
    }
}

/* --- IPv6 -----------------------------------------------------------------
 *
 * Not a second address family bolted beside the first. IPv6 replaces the
 * pieces underneath rather than adding to them: there is no ARP, because
 * finding a neighbour is done with ICMP like everything else; there is no
 * broadcast, because multicast covers it properly; and an address is not
 * something a server hands out one at a time - the router says what the prefix
 * is and every machine works out its own.
 *
 * Which is why this is more code than it looks like it should be. Half of it
 * is the things IPv4 got by asking someone.
 */

#define ETH_IP6   0x86DD
#define IP6_ICMP  58
#define IP6_UDP   17
#define IP6_TCP   6

#define ND_ROUTER_SOLICIT   133
#define ND_ROUTER_ADVERT    134
#define ND_NEIGHBOUR_SOLICIT 135
#define ND_NEIGHBOUR_ADVERT  136
#define ICMP6_ECHO_REQUEST  128
#define ICMP6_ECHO_REPLY    129

static struct in6 g_ll;             /* fe80::, always ours                   */
static struct in6 g_global;         /* from the router's prefix, if there is one */
static struct in6 g_router;         /* the router's own link-local address    */
static struct in6 g_dns6;           /* a resolver reachable over IPv6         */
static int g_have_global, g_have_router, g_have_dns6;

static int in6_equal(const struct in6* a, const struct in6* b)
{
    for (int i = 0; i < 16; ++i)
        if (a->b[i] != b->b[i])
            return 0;
    return 1;
}

static int in6_zero(const struct in6* a)
{
    for (int i = 0; i < 16; ++i)
        if (a->b[i])
            return 0;
    return 1;
}

/* An address a machine can work out for itself, from the one number it is
 * born with. The MAC is split down the middle, ff:fe is dropped in between,
 * and the bit that means "this was assigned by a manufacturer" is flipped to
 * mean "and I am using it as an address". */
static void eui64(struct in6* out, const uint8_t* mac)
{
    memset(out, 0, sizeof(*out));
    out->b[0] = 0xFE; out->b[1] = 0x80;
    out->b[8]  = mac[0] ^ 0x02;
    out->b[9]  = mac[1];
    out->b[10] = mac[2];
    out->b[11] = 0xFF;
    out->b[12] = 0xFE;
    out->b[13] = mac[3];
    out->b[14] = mac[4];
    out->b[15] = mac[5];
}

/* Multicast has no broadcast to fall back on and does not need one: the last
 * four bytes of the address are the last four of the Ethernet address, so a
 * card can filter without understanding IPv6 at all. */
static void multicast_mac(const struct in6* addr, uint8_t* mac)
{
    mac[0] = 0x33; mac[1] = 0x33;
    mac[2] = addr->b[12]; mac[3] = addr->b[13];
    mac[4] = addr->b[14]; mac[5] = addr->b[15];
}

/* ff02::1:ffXX:XXXX - the group every machine joins for its own address, so a
 * neighbour solicitation reaches one machine rather than all of them. */
static void solicited_node(const struct in6* target, struct in6* out)
{
    memset(out, 0, sizeof(*out));
    out->b[0] = 0xFF; out->b[1] = 0x02;
    out->b[11] = 0x01;
    out->b[12] = 0xFF;
    out->b[13] = target->b[13];
    out->b[14] = target->b[14];
    out->b[15] = target->b[15];
}

static void all_routers(struct in6* out)
{
    memset(out, 0, sizeof(*out));
    out->b[0] = 0xFF; out->b[1] = 0x02; out->b[15] = 0x02;
}

/* The checksum is not optional here, and it covers a header that is not in the
 * packet: the two addresses, the length and the protocol. */
static unsigned icmp6_checksum(const struct in6* src, const struct in6* dst,
                               unsigned next, const uint8_t* body, unsigned length)
{
    unsigned sum = 0;
    for (int i = 0; i < 16; i += 2) sum += rd16(src->b + i);
    for (int i = 0; i < 16; i += 2) sum += rd16(dst->b + i);
    sum += (length >> 16) & 0xFFFF;
    sum += length & 0xFFFF;
    sum += next;
    for (unsigned i = 0; i + 1 < length; i += 2)
        sum += rd16(body + i);
    if (length & 1)
        sum += (unsigned)body[length - 1] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (~sum) & 0xFFFF;
}

/* --- who is where, over IPv6 ---------------------------------------------- */

#define NEIGH_MAX 16
static struct { struct in6 addr; uint8_t mac[6]; int used; } g_neigh[NEIGH_MAX];

static void neigh_learn(const struct in6* addr, const uint8_t* mac)
{
    for (int i = 0; i < NEIGH_MAX; ++i)
        if (g_neigh[i].used && in6_equal(&g_neigh[i].addr, addr)) {
            memcpy(g_neigh[i].mac, mac, 6);
            return;
        }
    for (int i = 0; i < NEIGH_MAX; ++i)
        if (!g_neigh[i].used) {
            g_neigh[i].used = 1;
            g_neigh[i].addr = *addr;
            memcpy(g_neigh[i].mac, mac, 6);
            return;
        }
}

static const uint8_t* neigh_lookup(const struct in6* addr)
{
    for (int i = 0; i < NEIGH_MAX; ++i)
        if (g_neigh[i].used && in6_equal(&g_neigh[i].addr, addr))
            return g_neigh[i].mac;
    return 0;
}

/* Build and send one IPv6 packet. `hop` is 255 for neighbour discovery, which
 * is how the receiver knows the message cannot have come through a router and
 * therefore really is from the local link. */
static int send_ip6(const struct in6* src, const struct in6* dst,
                    const uint8_t* dst_mac, unsigned next, unsigned hop,
                    const uint8_t* body, unsigned length)
{
    static uint8_t f[1536];
    if (length > sizeof(f) - 54)
        return -1;
    memcpy(f, dst_mac, 6);
    memcpy(f + 6, g_mac, 6);
    wr16(f + 12, ETH_IP6);

    uint8_t* h = f + 14;
    memset(h, 0, 40);
    h[0] = 0x60;                    /* version six, nothing else set */
    wr16(h + 4, length);
    h[6] = (uint8_t)next;
    h[7] = (uint8_t)hop;
    memcpy(h + 8, src->b, 16);
    memcpy(h + 24, dst->b, 16);
    memcpy(h + 40, body, length);
    return send_frame(f, 54 + length);
}

/* --- neighbour discovery, which is ARP's whole job done properly ---------- */

static void nd_solicit(const struct in6* target)
{
    struct in6 dst;
    solicited_node(target, &dst);
    uint8_t mac[6];
    multicast_mac(&dst, mac);

    uint8_t body[32];
    memset(body, 0, sizeof(body));
    body[0] = ND_NEIGHBOUR_SOLICIT;
    memcpy(body + 8, target->b, 16);
    body[24] = 1;                   /* option: this is my link-layer address */
    body[25] = 1;                   /* one eight-byte unit long */
    memcpy(body + 26, g_mac, 6);
    wr16(body + 2, icmp6_checksum(&g_ll, &dst, IP6_ICMP, body, sizeof(body)));
    send_ip6(&g_ll, &dst, mac, IP6_ICMP, 255, body, sizeof(body));
}

static void nd_advertise(const struct in6* to, const uint8_t* to_mac,
                         const struct in6* target)
{
    uint8_t body[32];
    memset(body, 0, sizeof(body));
    body[0] = ND_NEIGHBOUR_ADVERT;
    body[4] = 0x60;                 /* solicited, and override what they had */
    memcpy(body + 8, target->b, 16);
    body[24] = 2;                   /* option: the address they asked about */
    body[25] = 1;
    memcpy(body + 26, g_mac, 6);
    wr16(body + 2, icmp6_checksum(target, to, IP6_ICMP, body, sizeof(body)));
    send_ip6(target, to, to_mac, IP6_ICMP, 255, body, sizeof(body));
}

static void rs_send(void)
{
    struct in6 dst;
    all_routers(&dst);
    uint8_t mac[6];
    multicast_mac(&dst, mac);

    uint8_t body[16];
    memset(body, 0, sizeof(body));
    body[0] = ND_ROUTER_SOLICIT;
    body[8] = 1;                    /* option: my link-layer address */
    body[9] = 1;
    memcpy(body + 10, g_mac, 6);
    wr16(body + 2, icmp6_checksum(&g_ll, &dst, IP6_ICMP, body, sizeof(body)));
    send_ip6(&g_ll, &dst, mac, IP6_ICMP, 255, body, sizeof(body));
}

/* A datagram over IPv6.
 *
 * The checksum is not optional here. Over IPv4 a zero means "not computed" and
 * this stack leaves it out; IPv6 removed that escape, because there is no
 * header checksum underneath it any more - the datagram's own is the only
 * thing standing between a corrupted address and the wrong process.
 */
static int send_udp6(const struct in6* dst, const uint8_t* dst_mac,
                     unsigned src_port, unsigned dst_port,
                     const uint8_t* payload, unsigned length)
{
    static uint8_t body[1400];
    if (length > sizeof(body) - 8)
        return -1;
    const struct in6* me = is_link_local(dst) ? &g_ll
                         : (g_have_global ? &g_global : &g_ll);
    wr16(body + 0, src_port);
    wr16(body + 2, dst_port);
    wr16(body + 4, 8 + length);
    wr16(body + 6, 0);
    memcpy(body + 8, payload, length);
    unsigned sum = icmp6_checksum(me, dst, IP6_UDP, body, 8 + length);
    /* Zero is reserved for "no checksum" over IPv4 and is not legal here, so
     * the all-ones form of the same value is sent instead. */
    if (sum == 0)
        sum = 0xFFFF;
    wr16(body + 6, sum);
    return send_ip6(me, dst, dst_mac, IP6_UDP, 64, body, 8 + length);
}

/* Whoever can carry a packet to this address: the neighbour itself when it is
 * on the link, otherwise the router. */
static const uint8_t* route6(const struct in6* to, struct in6* via_out)
{
    struct in6 via = *to;
    if (!is_link_local(to) && g_have_router)
        via = g_router;
    if (via_out != 0)
        *via_out = via;
    return neigh_lookup(&via);
}

static void send_ping6(const struct in6* to, unsigned seq, const uint8_t* mac)
{
    uint8_t body[16];
    memset(body, 0, sizeof(body));
    body[0] = ICMP6_ECHO_REQUEST;
    wr16(body + 4, 0x4C45);             /* an identifier: "LE" */
    wr16(body + 6, seq);
    for (int i = 0; i < 8; ++i)
        body[8 + i] = (uint8_t)('a' + i);
    const struct in6* me = g_have_global ? &g_global : &g_ll;
    wr16(body + 2, icmp6_checksum(me, to, IP6_ICMP, body, sizeof(body)));
    send_ip6(me, to, mac, IP6_ICMP, 64, body, sizeof(body));
}

/* --- everything that arrives ---------------------------------------------- */

static unsigned src_ip_of(const uint8_t* ip_hdr) { return rd32(ip_hdr + 12); }

static void handle_udp6(const uint8_t* h, const uint8_t* udp, unsigned length);

static int dns_retry_over_v6(struct pending* p)
{
    struct in6 via6;
    const uint8_t* mac6;

    /* Once only: a second failure is a real one, and a query that keeps
     * renewing its own deadline would never be answered either way. */
    if (p->over6 || !g_have_dns6 || p->name[0] == '\0')
        return 0;

    p->over6 = 1;
    p->deadline = g_ticks + 2000;
    mac6 = route6(&g_dns6, &via6);
    if (mac6 != 0)
        dns_ask6(p->name, p->seq, &g_dns6, mac6, p->want6);
    else
        nd_solicit(&via6);      /* the neighbour first; the reply re-sends */
    return 1;
}
static void handle_tcp6(const uint8_t* f, const uint8_t* h, const uint8_t* t,
                        unsigned length);

/* Walk the options that follow a discovery message. They are all
 * type-length-value with the length in eight-byte units, which is the one
 * thing about IPv6 options that never varies. */
static const uint8_t* nd_option(const uint8_t* body, unsigned length,
                                unsigned at, unsigned want, unsigned* len_out)
{
    while (at + 2 <= length) {
        const unsigned type = body[at];
        const unsigned units = body[at + 1];
        if (units == 0 || at + units * 8 > length)
            return 0;
        if (type == want) {
            if (len_out != 0) *len_out = units * 8;
            return body + at;
        }
        at += units * 8;
    }
    return 0;
}

static void handle_icmp6(const uint8_t* f, unsigned len, const uint8_t* h,
                         const uint8_t* body, unsigned length)
{
    struct in6 src, dst;
    memcpy(src.b, h + 8, 16);
    memcpy(dst.b, h + 24, 16);
    const unsigned type = body[0];

    if (type == ND_NEIGHBOUR_SOLICIT && length >= 24) {
        struct in6 target;
        memcpy(target.b, body + 8, 16);
        /* Only answer for addresses that are actually ours. */
        if (!in6_equal(&target, &g_ll) &&
            !(g_have_global && in6_equal(&target, &g_global)))
            return;
        unsigned olen = 0;
        const uint8_t* opt = nd_option(body, length, 24, 1, &olen);
        if (opt != 0 && olen >= 8)
            neigh_learn(&src, opt + 2);
        /* A solicitation from the unspecified address is duplicate address
         * detection, and is answered to the all-nodes group rather than to a
         * sender that does not have an address yet. */
        const uint8_t* to_mac = in6_zero(&src) ? 0 : neigh_lookup(&src);
        if (to_mac == 0)
            to_mac = f + 6;         /* whoever sent it, whatever they claimed */
        nd_advertise(&src, to_mac, &target);
        return;
    }

    if (type == ND_NEIGHBOUR_ADVERT && length >= 24) {
        struct in6 target;
        memcpy(target.b, body + 8, 16);
        unsigned olen = 0;
        const uint8_t* opt = nd_option(body, length, 24, 2, &olen);
        neigh_learn(&target, (opt != 0 && olen >= 8) ? opt + 2 : f + 6);
        for (int i = 0; i < PENDING_MAX; ++i) {
            struct pending* p = &g_pending[i];
            if (!p->used)
                continue;
            if (p->kind == WAIT_ND && in6_equal((struct in6*)p->name, &target)) {
                answer(p, 0, neigh_lookup(&target), 6);
            } else if (p->kind == WAIT_DNS && p->over6 && p->name[0] != '\0') {
                dns_ask6(p->name, p->seq, &g_dns6, neigh_lookup(&target),
                         p->want6);
            } else if (p->kind == WAIT_PING6) {
                /* It was waiting for whoever could carry it. */
                struct in6 to;
                memcpy(to.b, p->name, 16);
                const uint8_t* mac = neigh_lookup(&target);
                if (mac != 0)
                    send_ping6(&to, p->seq, mac);
            }
        }
        return;
    }

    if (type == ND_ROUTER_ADVERT && length >= 16) {
        g_router = src;
        g_have_router = 1;
        neigh_learn(&src, f + 6);
        unsigned olen = 0;
        const uint8_t* opt = nd_option(body, length, 16, 1, &olen);
        if (opt != 0 && olen >= 8)
            neigh_learn(&src, opt + 2);

        /* The prefix, and then the address worked out from it. This is what
         * replaces asking a server for one: the router says what the network
         * is and every machine fills in its own half. */
        unsigned plen = 0;
        const uint8_t* prefix = nd_option(body, length, 16, 3, &plen);
        if (prefix != 0 && plen >= 32 && prefix[2] == 64 && !g_have_global) {
            eui64(&g_global, g_mac);
            memcpy(g_global.b, prefix + 16, 8);      /* the router's half */
            g_have_global = 1;
            /* The resolver on this prefix. There is no option in a router
             * advertisement that carries one here - the RDNSS option exists
             * and QEMU does not send it - so this is the convention the same
             * network uses for IPv4: the third address. It is a guess, and it
             * is only ever used when it answers. */
            memset(&g_dns6, 0, sizeof(g_dns6));
            memcpy(g_dns6.b, prefix + 16, 8);
            g_dns6.b[15] = 3;
            g_have_dns6 = 1;
            printf("netd: address %x:%x::%x:%x:%x:%x from the router\n",
                   rd16(g_global.b), rd16(g_global.b + 2),
                   rd16(g_global.b + 8), rd16(g_global.b + 10),
                   rd16(g_global.b + 12), rd16(g_global.b + 14));
        }
        return;
    }

    if (type == ICMP6_ECHO_REQUEST && length >= 8) {
        static uint8_t reply[1280];
        const unsigned n = length > sizeof(reply) ? (unsigned)sizeof(reply) : length;
        memcpy(reply, body, n);
        reply[0] = ICMP6_ECHO_REPLY;
        wr16(reply + 2, 0);
        const struct in6* me = in6_equal(&dst, &g_ll) ? &g_ll : &g_global;
        wr16(reply + 2, icmp6_checksum(me, &src, IP6_ICMP, reply, n));
        const uint8_t* mac = neigh_lookup(&src);
        send_ip6(me, &src, mac != 0 ? mac : f + 6, IP6_ICMP, 64, reply, n);
        return;
    }

    if (type == ICMP6_ECHO_REPLY && length >= 8) {
        const unsigned seq = rd16(body + 6);
        for (int i = 0; i < PENDING_MAX; ++i) {
            struct pending* p = &g_pending[i];
            if (p->used && p->kind == WAIT_PING6 && p->seq == seq)
                answer(p, (long)h[7], 0, 0);        /* the hop limit it had */
        }
    }
    (void)len;
}

static void handle_frame(const uint8_t* f, unsigned len)
{
    ++g_in;
    if (len < 14)
        return;
    const unsigned type = rd16(f + 12);

    if (type == ETH_ARP && len >= 42) {
        const unsigned op = rd16(f + 20);
        const unsigned sender_ip = rd32(f + 28);
        if (op == 1 && g_configured && rd32(f + 38) == g_ip) {
            arp_learn(sender_ip, f + 22);
            arp_answer(f);
        } else if (op == 2) {
            arp_learn(sender_ip, f + 22);
            /* Anything that was waiting on this address can go now. */
            for (int i = 0; i < PENDING_MAX; ++i) {
                struct pending* p = &g_pending[i];
                if (!p->used || p->ip != sender_ip)
                    continue;
                if (p->kind == WAIT_ARP) {
                    answer(p, 0, f + 22, 6);
                } else if (p->kind == WAIT_PING) {
                    send_ping(p->ip, p->seq, f + 22);
                } else if (p->kind == WAIT_DNS && p->name[0] != '\0') {
                    dns_ask(p->name, p->seq, f + 22, p->want6);
                } else if (p->kind == WAIT_TCP_ARP) {
                    struct conn* c = conn_find(p->ip);
                    if (c != 0) {
                        tcp_send(c, TCP_SYN, c->snd_next, 0, 0);
                        c->snd_next += 1;
                    }
                    p->kind = WAIT_CONNECT;
                }
            }
        }
        return;
    }

    if (type == ETH_IP6 && len >= 14 + 40) {
        const uint8_t* h = f + 14;
        const unsigned payload = rd16(h + 4);
        unsigned n = payload;
        if (n > len - 14 - 40)
            n = len - 14 - 40;
        if (h[6] == IP6_ICMP && len >= 14 + 40 + 8) {
            handle_icmp6(f, len, h, h + 40, n);
        } else if (h[6] == IP6_UDP && n >= 8) {
            handle_udp6(h, h + 40, n);
        } else if (h[6] == IP6_TCP && n >= 20) {
            handle_tcp6(f, h, h + 40, n);
        }
        return;
    }

    if (type != ETH_IP || len < 14 + 20)
        return;
    const uint8_t* ip_hdr = f + 14;
    if ((ip_hdr[0] >> 4) != 4)
        return;
    const unsigned ihl = (ip_hdr[0] & 0x0F) * 4;
    const unsigned src = rd32(ip_hdr + 12);
    if (len < 14 + ihl + 8)
        return;

    const uint8_t* icmp = ip_hdr + ihl;
    if (ip_hdr[9] == IP_ICMP && icmp[0] == 8 && rd32(ip_hdr + 16) == g_ip) {
        /* Somebody pinged us. Turn it round: same payload, type 0. */
        static uint8_t r[1536];
        const unsigned n = len > sizeof(r) ? (unsigned)sizeof(r) : len;
        memcpy(r, f, n);
        memcpy(r, f + 6, 6);
        memcpy(r + 6, g_mac, 6);
        wr32(r + 14 + 12, g_ip);
        wr32(r + 14 + 16, src);
        wr16(r + 14 + 10, 0);
        wr16(r + 14 + 10, checksum(r + 14, ihl));
        uint8_t* ri = r + 14 + ihl;
        ri[0] = 0;
        wr16(ri + 2, 0);
        wr16(ri + 2, checksum(ri, n - 14 - ihl));
        send_frame(r, n);
        return;
    }

    if (ip_hdr[9] == IP_UDP) {
        const uint8_t* udp = ip_hdr + ihl;
        if (len < 14 + ihl + 8)
            return;
        const unsigned src_port = rd16(udp);
        const unsigned dst_port = rd16(udp + 2);
        const unsigned udp_len = rd16(udp + 4);
        /* A datagram claiming to be shorter than its own header is either a
         * broken sender or a hostile one. Subtracting first and checking after
         * would underflow to four billion, and the check would wrap with it. */
        if (udp_len < 8)
            return;
        const unsigned dlen = udp_len - 8;
        const uint8_t* body = udp + 8;
        if (dlen > len - 14 - ihl - 8)
            return;

        if (dst_port == 68) {
            dhcp_receive(body, dlen);
            return;
        }
        if (src_port == DNS_PORT) {
            /* Which family the answer holds is the question that was asked,
             * not the one that carried it: an AAAA record comes back over
             * IPv4 perfectly happily, and reading it with the A parser finds
             * nothing and reports the name as unknown. */
            unsigned id = 0;
            struct in6 got;
            memset(&got, 0, sizeof(got));
            const int have6 = dns_answer6(body, dlen, &id, &got);
            const unsigned address = dns_answer(body, dlen, &id);
            for (int i = 0; i < PENDING_MAX; ++i) {
                struct pending* p = &g_pending[i];
                if (!p->used || p->kind != WAIT_DNS || p->seq != id)
                    continue;
                if (p->want6)
                    answer(p, have6 ? 0 : -1, have6 ? got.b : 0, have6 ? 16 : 0);
                else
                    answer(p, address != 0 ? (long)address : -1, 0, 0);
            }
            return;
        }
        /* Anything else is for whoever asked to listen on that port. */
        for (int i = 0; i < PENDING_MAX; ++i) {
            struct pending* p = &g_pending[i];
            if (p->used && p->kind == WAIT_UDP && p->seq == dst_port) {
                const unsigned n = dlen > 200 ? 200 : dlen;
                answer(p, (long)src_ip_of(ip_hdr), body, n);
            }
        }
        return;
    }

    if (ip_hdr[9] == IP_TCP) {
        const uint8_t* t = ip_hdr + ihl;
        if (len < 14 + ihl + 20)
            return;
        const unsigned dst_port = rd16(t + 2);
        struct conn* c = conn_find(dst_port);
        if (c == 0 || !c->used)
            return;

        const unsigned seq   = rd32(t + 4);
        const unsigned ack   = rd32(t + 8);
        const unsigned flags = rd16(t + 12) & 0x3F;
        const unsigned off   = ((rd16(t + 12) >> 12) & 0xF) * 4;
        const unsigned total = rd16(ip_hdr + 2);
        const unsigned dlen  = (total > ihl + off) ? total - ihl - off : 0;
        const uint8_t* body  = t + off;

        if (flags & TCP_RST) {
            /* Refused, or the other end has given up. Either way this
             * connection is over and anyone waiting on it should be told so
             * rather than waiting for a timeout. */
            c->state = TCP_DEAD;
            for (int i = 0; i < PENDING_MAX; ++i) {
                struct pending* p = &g_pending[i];
                if (p->used && p->ip == c->local_port &&
                    (p->kind == WAIT_CONNECT || p->kind == WAIT_READ))
                    answer(p, -1, 0, 0);
            }
            return;
        }

        if (c->state == TCP_SYN_SENT && (flags & TCP_SYN) && (flags & TCP_ACK)) {
            c->rcv_next = seq + 1;
            c->snd_next = ack;
            c->state = TCP_ESTABLISHED;
            tcp_send(c, TCP_ACK, c->snd_next, 0, 0);
            for (int i = 0; i < PENDING_MAX; ++i) {
                struct pending* p = &g_pending[i];
                if (p->used && p->kind == WAIT_CONNECT && p->ip == c->local_port)
                    answer(p, (long)c->local_port, 0, 0);
            }
            return;
        }

        /* Anything we sent that has now been acknowledged can stop being
         * kept. Only the whole segment counts, because only whole segments
         * are ever sent. */
        if ((flags & TCP_ACK) && c->out_len > 0 &&
            ack >= c->out_seq + c->out_len) {
            c->out_len = 0;
            c->out_tries = 0;
        }

        if (dlen > 0) {
            if (seq == c->rcv_next) {
                unsigned n = dlen;
                if (n > TCP_RX - c->rx_len)
                    n = TCP_RX - c->rx_len;
                memcpy(c->rx + c->rx_len, body, n);
                c->rx_len += n;
                c->rcv_next += dlen;
            }
            /* Acknowledge either way: in order it moves things along, and out
             * of order it tells the sender what we are actually waiting for. */
            tcp_send(c, TCP_ACK, c->snd_next, 0, 0);
        }

        if (flags & TCP_FIN) {
            c->rcv_next = seq + dlen + 1;
            c->peer_closed = 1;
            tcp_send(c, TCP_ACK, c->snd_next, 0, 0);
        }

        /* Whoever was waiting to read can have what arrived, or be told the
         * stream has ended. */
        for (int i = 0; i < PENDING_MAX; ++i) {
            struct pending* p = &g_pending[i];
            if (!p->used || p->kind != WAIT_READ || p->ip != c->local_port)
                continue;
            if (c->rx_len > 0) {
                unsigned n = c->rx_len > 200 ? 200 : c->rx_len;
                answer(p, (long)n, c->rx, n);
                memmove(c->rx, c->rx + n, c->rx_len - n);
                c->rx_len -= n;
            } else if (c->peer_closed) {
                answer(p, 0, 0, 0);     /* the end of the stream */
            }
        }
        return;
    }

    if (ip_hdr[9] != IP_ICMP)
        return;

    if (icmp[0] == 0) {                 /* an echo reply */
        const unsigned seq = rd16(icmp + 6);
        for (int i = 0; i < PENDING_MAX; ++i) {
            struct pending* p = &g_pending[i];
            if (p->used && p->kind == WAIT_PING && p->seq == seq)
                answer(p, (long)ip_hdr[8], 0, 0);    /* the TTL that came back */
        }
    }
}

/* The same segment over IPv6. Only the wrapping differs - the header, and a
 * checksum taken over sixteen-byte addresses instead of four-byte ones. The
 * sequence numbers, the flags and the window are the same protocol, which is
 * the whole point of TCP not caring what carries it. */
static int tcp_send6(struct conn* c, unsigned flags, unsigned seq,
                     const uint8_t* payload, unsigned length)
{
    struct in6 via;
    const uint8_t* mac = route6(&c->peer6, &via);
    if (mac == 0) {
        nd_solicit(&via);
        return -1;
    }
    static uint8_t body[1500];
    if (length > sizeof(body) - 20)
        return -1;

    const struct in6* me = is_link_local(&c->peer6) ? &g_ll
                         : (g_have_global ? &g_global : &g_ll);
    wr16(body + 0, c->local_port);
    wr16(body + 2, c->peer_port);
    wr32(body + 4, seq);
    wr32(body + 8, c->rcv_next);
    wr16(body + 12, (5u << 12) | flags);
    wr16(body + 14, TCP_RX - c->rx_len);
    wr16(body + 16, 0);
    wr16(body + 18, 0);
    if (length > 0)
        memcpy(body + 20, payload, length);
    wr16(body + 16, icmp6_checksum(me, &c->peer6, IP6_TCP, body, 20 + length));
    return send_ip6(me, &c->peer6, mac, IP6_TCP, 64, body, 20 + length);
}

/* A datagram that arrived over IPv6. The only thing the family changes above
 * this point is how it got here. */
static void handle_udp6(const uint8_t* h, const uint8_t* udp, unsigned length)
{
    if (length < 8)
        return;
    const unsigned src_port = rd16(udp);
    const unsigned dst_port = rd16(udp + 2);
    const unsigned udp_len = rd16(udp + 4);
    if (udp_len < 8 || udp_len - 8 > length - 8)
        return;
    const unsigned dlen = udp_len - 8;
    const uint8_t* body = udp + 8;

    if (src_port == DNS_PORT) {
        unsigned id = 0;
        struct in6 got;
        memset(&got, 0, sizeof(got));
        const int have6 = dns_answer6(body, dlen, &id, &got);
        const unsigned v4 = have6 ? 0 : dns_answer(body, dlen, &id);
        for (int i = 0; i < PENDING_MAX; ++i) {
            struct pending* p = &g_pending[i];
            if (!p->used || p->kind != WAIT_DNS || p->seq != id)
                continue;
            if (have6)
                answer(p, 0, got.b, 16);
            else
                answer(p, v4 != 0 ? (long)v4 : -1, 0, 0);
        }
        return;
    }
    for (int i = 0; i < PENDING_MAX; ++i) {
        struct pending* p = &g_pending[i];
        if (p->used && p->kind == WAIT_UDP && p->seq == dst_port) {
            const unsigned n = dlen > 200 ? 200 : dlen;
            answer(p, 0, body, n);
        }
    }
    (void)h;
}

/* A segment that arrived over IPv6. The state machine is the one the v4 path
 * uses; only finding the connection and acknowledging differ, because the
 * address is sixteen bytes rather than four. */
static void handle_tcp6(const uint8_t* f, const uint8_t* h, const uint8_t* t,
                        unsigned length)
{
    if (length < 20)
        return;
    struct conn* c = conn_find(rd16(t + 2));
    if (c == 0 || !c->used || !c->v6)
        return;

    const unsigned seq   = rd32(t + 4);
    const unsigned ack   = rd32(t + 8);
    const unsigned flags = rd16(t + 12) & 0x3F;
    const unsigned off   = ((rd16(t + 12) >> 12) & 0xF) * 4;
    const unsigned dlen  = length > off ? length - off : 0;
    const uint8_t* body  = t + off;

    if (flags & TCP_RST) {
        /* Refused, which is an answer: something on the other side received
         * the segment, understood it, and said no. Reported differently from
         * silence so a caller can tell a closed port from a link that never
         * carried the packet at all. */
        c->state = TCP_DEAD;
        for (int i = 0; i < PENDING_MAX; ++i) {
            struct pending* p = &g_pending[i];
            if (p->used && p->ip == c->local_port &&
                (p->kind == WAIT_CONNECT || p->kind == WAIT_READ))
                answer(p, -2, 0, 0);
        }
        return;
    }

    if (c->state == TCP_SYN_SENT && (flags & TCP_SYN) && (flags & TCP_ACK)) {
        c->rcv_next = seq + 1;
        c->snd_next = ack;
        c->state = TCP_ESTABLISHED;
        c->out_len = 0;
        tcp_send(c, TCP_ACK, c->snd_next, 0, 0);
        for (int i = 0; i < PENDING_MAX; ++i) {
            struct pending* p = &g_pending[i];
            if (p->used && p->kind == WAIT_CONNECT && p->ip == c->local_port)
                answer(p, (long)c->local_port, 0, 0);
        }
        return;
    }

    if ((flags & TCP_ACK) && c->out_len > 0 && ack >= c->out_seq + c->out_len) {
        c->out_len = 0;
        c->out_tries = 0;
    }

    if (dlen > 0) {
        if (seq == c->rcv_next) {
            unsigned n = dlen;
            if (n > TCP_RX - c->rx_len)
                n = TCP_RX - c->rx_len;
            memcpy(c->rx + c->rx_len, body, n);
            c->rx_len += n;
            c->rcv_next += dlen;
        }
        tcp_send(c, TCP_ACK, c->snd_next, 0, 0);
    }

    if (flags & TCP_FIN) {
        c->rcv_next = seq + dlen + 1;
        c->peer_closed = 1;
        tcp_send(c, TCP_ACK, c->snd_next, 0, 0);
    }

    for (int i = 0; i < PENDING_MAX; ++i) {
        struct pending* p = &g_pending[i];
        if (!p->used || p->kind != WAIT_READ || p->ip != c->local_port)
            continue;
        if (c->rx_len > 0) {
            unsigned n = c->rx_len > 200 ? 200 : c->rx_len;
            answer(p, (long)n, c->rx, n);
            memmove(c->rx, c->rx + n, c->rx_len - n);
            c->rx_len -= n;
        } else if (c->peer_closed) {
            answer(p, 0, 0, 0);
        }
    }
    (void)f;
    (void)h;
}

static void drain(void)
{
    while (g_net->rx_tail != g_net->rx_head) {
        const unsigned t = g_net->rx_tail;
        handle_frame((const uint8_t*)g_net->rx[t].data, g_net->rx[t].length);
        g_net->rx_tail = (t + 1) % NIC_RX_SLOTS;
    }
}

int main(void)
{
    for (int i = 0; i < 400 && g_nic_port <= 0; ++i) {
        g_nic_port = port_open(IPC_PORT_NIC);
        if (g_nic_port < 0) msleep(10);
    }
    if (g_nic_port < 0) {
        printf("netd: no driver is serving a card\n");
        return 1;
    }

    struct ipc_message q, a;
    memset(&q, 0, sizeof(q));
    memset(&a, 0, sizeof(a));
    q.tag = NIC_ATTACH;
    if (ipc_call(g_nic_port, &q, &a) != 0) {
        printf("netd: the driver would not attach\n");
        return 1;
    }
    memcpy(g_mac, a.data, 6);
    const int shm = shm_open((unsigned)a.word[0], sizeof(struct nic_shared), 0);
    g_net = shm < 0 ? 0 : (struct nic_shared*)shm_map(shm);
    if (g_net == 0) {
        printf("netd: cannot map the driver's frames\n");
        return 1;
    }

    const int port = port_create(IPC_PORT_NET);
    if (port < 0) {
        printf("netd: a stack is already running\n");
        return 1;
    }
    printf("netd[%d]: up on %02x:%02x:%02x:%02x:%02x:%02x, asking for a lease\n",
           getpid(), g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5]);

    /* IPv6 needs no server to hand out an address, only a router willing to
     * say what the network is - so this asks, and works out the rest itself. */
    eui64(&g_ll, g_mac);
    printf("netd: link-local fe80::%x:%x:%x:%x\n",
           rd16(g_ll.b + 8), rd16(g_ll.b + 10),
           rd16(g_ll.b + 12), rd16(g_ll.b + 14));
    rs_send();

    g_dhcp_state = DHCP_OFFER;
    dhcp_send(DHCP_DISCOVER);
    unsigned give_up = 3000;            /* three seconds of ticks */

    for (;;) {
        /* A network with no DHCP server is a network, not an error. Waiting
         * forever for a lease would make this stack useless on exactly the
         * link it was easiest to test on. */
        if (!g_configured && g_ticks > give_up) {
            printf("netd: nobody offered a lease; using the fallback\n");
            configure(FALLBACK_IP, FALLBACK_MASK, FALLBACK_GW, FALLBACK_DNS);
        }

        drain();
        conn_retransmit();
        expire_pending();

        struct ipc_message m, r;
        unsigned from = 0;
        const int handle = ipc_try_recv(port, &m, &from);
        if (handle < 0) {
            ++g_ticks;
            msleep(1);
            continue;
        }

        memset(&r, 0, sizeof(r));
        r.tag = m.tag;
        if (m.tag == NET_INFO) {
            r.word[0] = g_ip;
            r.word[1] = g_gw;
            memcpy(r.data, g_mac, 6);
            r.bytes = 6;
            ipc_reply(handle, &r);
        } else if (m.tag == NET_STATS) {
            r.word[0] = (long)g_in;
            r.word[1] = (long)g_out;
            r.word[2] = arp_count();
            ipc_reply(handle, &r);
        } else if (m.tag == NET_RESOLVE || m.tag == NET_PING) {
            const unsigned ip = (unsigned)m.word[0];
            /* Off the local network, ask the gateway - it is the only thing
             * that can be reached directly. */
            const unsigned target = ((ip ^ g_ip) & g_mask) ? g_gw : ip;
            const uint8_t* mac = arp_lookup(target);
            if (m.tag == NET_RESOLVE && mac != 0) {
                r.word[0] = 0;
                memcpy(r.data, mac, 6);
                r.bytes = 6;
                ipc_reply(handle, &r);
            } else {
                struct pending* p = pending_add(
                    m.tag == NET_PING ? WAIT_PING : WAIT_ARP, handle,
                    target, (unsigned)m.word[1]);
                if (p == 0) {
                    r.word[0] = -1;
                    ipc_reply(handle, &r);
                } else if (mac != 0) {
                    send_ping(ip, (unsigned)m.word[1], mac);
                } else {
                    arp_ask(target);
                }
            }
        } else if (m.tag == NET_LOOKUP || m.tag == NET6_LOOKUP) {
            const int want6 = m.tag == NET6_LOOKUP;
            /* A name needs the resolver's address resolved first, which is a
             * second round trip - so this waits twice and neither wait blocks
             * anything else. */
            const uint8_t* dns_mac = arp_lookup(
                ((g_dns ^ g_ip) & g_mask) ? g_gw : g_dns);
            const unsigned id = ++g_dns_id & 0xFFFF;
            struct pending* p = pending_add(WAIT_DNS, handle, g_dns, id);
            if (p == 0) {
                r.word[0] = -1;
                ipc_reply(handle, &r);
            } else {
                unsigned k = 0;
                while (k < m.bytes && k < sizeof(p->name) - 1) {
                    p->name[k] = m.data[k];
                    ++k;
                }
                p->name[k] = '\0';
                p->want6 = want6;
                /* Which transport carries the question is independent of
                 * which family it asks about: an AAAA record comes back over
                 * IPv4 perfectly well, and this network's only resolver that
                 * actually answers is the IPv4 one. So v4 is preferred and v6
                 * is the fallback rather than the other way round - a stack
                 * that insists on asking over v6 stops resolving anything the
                 * moment nothing is listening there, which is what happened
                 * the first time this was wired the other way. */
                struct in6 via6;
                const uint8_t* mac6 = g_have_dns6 ? route6(&g_dns6, &via6) : 0;
                if (dns_mac != 0) {
                    dns_ask(p->name, id, dns_mac, want6);
                } else if (g_dns != 0) {
                    arp_ask(((g_dns ^ g_ip) & g_mask) ? g_gw : g_dns);
                } else if (g_have_dns6 && mac6 != 0) {
                    p->over6 = 1;
                    dns_ask6(p->name, id, &g_dns6, mac6, want6);
                } else if (g_have_dns6) {
                    p->over6 = 1;
                    nd_solicit(&via6);
                }
            }
        } else if (m.tag == NET_UDP_SEND) {
            const unsigned dst = (unsigned)m.word[0];
            const uint8_t* mac = arp_lookup(((dst ^ g_ip) & g_mask) ? g_gw : dst);
            if (mac == 0) {
                arp_ask(((dst ^ g_ip) & g_mask) ? g_gw : dst);
                r.word[0] = -1;
            } else {
                r.word[0] = send_udp(dst, mac, (unsigned)m.word[2],
                                     (unsigned)m.word[1], (const uint8_t*)m.data,
                                     m.bytes);
            }
            ipc_reply(handle, &r);
        } else if (m.tag == NET6_UDP_SEND) {
            /* The address is the first sixteen bytes, the datagram the rest.
             * netd has been able to send these since IPv6 went in - it is how
             * a DNS question travels - but nothing outside netd could ask for
             * one, which is why the path went unexercised for so long. */
            struct in6 dst;
            struct in6 via;
            const uint8_t* mac;
            memcpy(dst.b, m.data, 16);
            mac = route6(&dst, &via);
            if (mac == 0) {
                nd_solicit(&via);
                r.word[0] = -1;
            } else {
                r.word[0] = send_udp6(&dst, mac, (unsigned)m.word[2],
                                      (unsigned)m.word[1],
                                      (const uint8_t*)m.data + 16,
                                      m.bytes > 16 ? m.bytes - 16 : 0);
            }
            ipc_reply(handle, &r);
        } else if (m.tag == NET_UDP_RECV) {
            struct pending* p = pending_add(WAIT_UDP, handle, 0,
                                            (unsigned)m.word[0]);
            if (p == 0) {
                r.word[0] = -1;
                ipc_reply(handle, &r);
            }
        } else if (m.tag == NET6_TCP_CONNECT) {
            struct conn* c = 0;
            for (int i = 0; i < CONN_MAX && c == 0; ++i)
                if (!g_conn[i].used) c = &g_conn[i];
            if (c == 0) {
                r.word[0] = -1;
                ipc_reply(handle, &r);
            } else {
                memset(c, 0, sizeof(*c));
                c->used = 1;
                c->v6 = 1;
                memcpy(c->peer6.b, m.data, 16);
                c->peer_port = (unsigned)m.word[1];
                c->local_port = g_next_port++;
                if (g_next_port > 60000) g_next_port = 40000;
                c->snd_next = 0x5EED0000u + g_ticks * 7919u;
                c->state = TCP_SYN_SENT;

                struct pending* p = pending_add(WAIT_CONNECT, handle,
                                                c->local_port, 0);
                if (p == 0) {
                    c->used = 0;
                    r.word[0] = -1;
                    ipc_reply(handle, &r);
                } else {
                    /* tcp_send6 solicits the neighbour itself when it does not
                     * know one, and the retransmit timer sends the SYN again
                     * once the advertisement has arrived - so a first attempt
                     * that fails for want of an address is not an error. */
                    c->out_len = 0;
                    if (tcp_send6(c, TCP_SYN, c->snd_next, 0, 0) != 0) {
                        c->out_deadline = g_ticks + 200;
                        c->out_tries = 0;
                        c->out_len = 1;         /* something to retry */
                        c->out[0] = 0;
                        c->out_seq = c->snd_next;
                    }
                    c->snd_next += 1;
                }
            }
        } else if (m.tag == NET_TCP_CONNECT) {
            struct conn* c = 0;
            for (int i = 0; i < CONN_MAX && c == 0; ++i)
                if (!g_conn[i].used) c = &g_conn[i];
            if (c == 0) {
                r.word[0] = -1;
                ipc_reply(handle, &r);
            } else {
                memset(c, 0, sizeof(*c));
                c->used = 1;
                c->peer_ip = (unsigned)m.word[0];
                c->peer_port = (unsigned)m.word[1];
                c->local_port = g_next_port++;
                if (g_next_port > 60000) g_next_port = 40000;
                /* An initial sequence number that is not always the same, so
                 * a new connection cannot be confused with an old one on the
                 * same pair of ports. */
                c->snd_next = 0x5EED0000u + g_ticks * 7919u;
                c->state = TCP_SYN_SENT;

                const unsigned via = ((c->peer_ip ^ g_ip) & g_mask)
                                         ? g_gw : c->peer_ip;
                struct pending* p = pending_add(WAIT_CONNECT, handle,
                                                c->local_port, 0);
                if (p == 0) {
                    c->used = 0;
                    r.word[0] = -1;
                    ipc_reply(handle, &r);
                } else if (arp_lookup(via) != 0) {
                    tcp_send(c, TCP_SYN, c->snd_next, 0, 0);
                    c->snd_next += 1;
                } else {
                    /* The address has to be found before the handshake can
                     * start. Remember which connection is waiting on it. */
                    p->kind = WAIT_TCP_ARP;
                    p->seq = via;
                    arp_ask(via);
                }
            }
        } else if (m.tag == NET_TCP_SEND) {
            struct conn* c = conn_find((unsigned)m.word[0]);
            if (c == 0 || c->state != TCP_ESTABLISHED || c->out_len > 0) {
                /* Busy or gone. One segment in flight at a time, so a caller
                 * offering more before the last was acknowledged is told to
                 * wait rather than having it silently dropped. */
                r.word[0] = (c != 0 && c->out_len > 0) ? 0 : -1;
            } else {
                /* Bounded by the message, not by the segment: m.bytes comes
                 * from the caller and the data it describes is only ever
                 * sizeof(m.data) long. */
                unsigned n = m.bytes;
                if (n > sizeof(m.data)) n = sizeof(m.data);
                memcpy(c->out, m.data, n);
                c->out_len = n;
                c->out_seq = c->snd_next;
                c->out_tries = 0;
                c->out_deadline = g_ticks + 400;
                tcp_send(c, TCP_ACK | TCP_PSH, c->snd_next, c->out, n);
                c->snd_next += n;
                r.word[0] = (long)n;
            }
            ipc_reply(handle, &r);
        } else if (m.tag == NET_TCP_RECV) {
            struct conn* c = conn_find((unsigned)m.word[0]);
            if (c == 0 || c->state == TCP_DEAD) {
                r.word[0] = -1;
                ipc_reply(handle, &r);
            } else if (c->rx_len > 0) {
                unsigned n = c->rx_len > 200 ? 200 : c->rx_len;
                memcpy(r.data, c->rx, n);
                r.bytes = n;
                r.word[0] = (long)n;
                memmove(c->rx, c->rx + n, c->rx_len - n);
                c->rx_len -= n;
                ipc_reply(handle, &r);
            } else if (c->peer_closed) {
                r.word[0] = 0;
                ipc_reply(handle, &r);
            } else {
                /* Nothing yet. Put the reader aside rather than answering
                 * with nothing, which would turn every read into a poll. */
                if (pending_add(WAIT_READ, handle, c->local_port, 0) == 0) {
                    r.word[0] = -1;
                    ipc_reply(handle, &r);
                }
            }
        } else if (m.tag == NET_TCP_CLOSE) {
            struct conn* c = conn_find((unsigned)m.word[0]);
            if (c != 0) {
                if (c->state == TCP_ESTABLISHED) {
                    tcp_send(c, TCP_FIN | TCP_ACK, c->snd_next, 0, 0);
                    c->snd_next += 1;
                }
                c->used = 0;
            }
            r.word[0] = 0;
            ipc_reply(handle, &r);
        } else if (m.tag == NET6_INFO) {
            memcpy(r.data, g_ll.b, 16);
            memcpy(r.data + 16, g_global.b, 16);
            r.bytes = 32;
            r.word[0] = g_have_global;
            r.word[1] = g_have_router;
            ipc_reply(handle, &r);
        } else if (m.tag == NET6_NEIGH || m.tag == NET6_PING) {
            struct in6 target;
            memcpy(target.b, m.data, 16);
            /* Off the link, ask the router - which is found the same way any
             * other neighbour is, because it is one. */
            struct in6 via = target;
            if (!is_link_local(&target) && g_have_router)
                via = g_router;
            const uint8_t* mac = neigh_lookup(&via);

            if (m.tag == NET6_NEIGH && mac != 0) {
                memcpy(r.data, mac, 6);
                r.bytes = 6;
                r.word[0] = 0;
                ipc_reply(handle, &r);
            } else {
                struct pending* p = pending_add(
                    m.tag == NET6_PING ? WAIT_PING6 : WAIT_ND, handle, 0,
                    (unsigned)m.word[1]);
                if (p == 0) {
                    r.word[0] = -1;
                    ipc_reply(handle, &r);
                } else {
                    /* The address being waited on rides in the same field a
                     * host name would; both are just bytes to remember. */
                    memcpy(p->name, m.tag == NET6_PING ? target.b : via.b, 16);
                    if (mac != 0)
                        send_ping6(&target, (unsigned)m.word[1], mac);
                    else
                        nd_solicit(&via);
                }
            }
        } else {
            r.word[0] = -1;
            ipc_reply(handle, &r);
        }
    }
}
