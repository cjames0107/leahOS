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

static struct pending {
    int used;
    int kind;
    int handle;
    unsigned ip;
    unsigned seq;           /* also the DNS id, and the UDP port */
    unsigned deadline;      /* in loop ticks */
    char     name[128];     /* the host being looked up */
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

static void dns_ask(const char* host, unsigned id, const uint8_t* mac)
{
    static uint8_t q[300];
    memset(q, 0, sizeof(q));
    wr16(q + 0, id);
    wr16(q + 2, 0x0100);                    /* a query, please recurse */
    wr16(q + 4, 1);                         /* one question */
    unsigned at = 12 + encode_name(host, q + 12);
    wr16(q + at, 1); at += 2;               /* an address record */
    wr16(q + at, 1); at += 2;               /* on the internet */
    send_udp(g_dns, mac, 40000 + (id & 0xFF), DNS_PORT, q, at);
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

static int tcp_send(struct conn* c, unsigned flags, unsigned seq,
                    const uint8_t* payload, unsigned length)
{
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

/* --- everything that arrives ---------------------------------------------- */

static unsigned src_ip_of(const uint8_t* ip_hdr) { return rd32(ip_hdr + 12); }

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
                    dns_ask(p->name, p->seq, f + 22);
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
            unsigned id = 0;
            const unsigned address = dns_answer(body, dlen, &id);
            for (int i = 0; i < PENDING_MAX; ++i) {
                struct pending* p = &g_pending[i];
                if (p->used && p->kind == WAIT_DNS && p->seq == id)
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
        } else if (m.tag == NET_LOOKUP) {
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
                if (dns_mac != 0)
                    dns_ask(p->name, id, dns_mac);
                else
                    arp_ask(((g_dns ^ g_ip) & g_mask) ? g_gw : g_dns);
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
        } else if (m.tag == NET_UDP_RECV) {
            struct pending* p = pending_add(WAIT_UDP, handle, 0,
                                            (unsigned)m.word[0]);
            if (p == 0) {
                r.word[0] = -1;
                ipc_reply(handle, &r);
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
        } else {
            r.word[0] = -1;
            ipc_reply(handle, &r);
        }
    }
}
