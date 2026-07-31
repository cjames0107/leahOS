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

#define OUR_IP   0x0A00020F      /* 10.0.2.15 */
#define GATEWAY  0x0A000202      /* 10.0.2.2  */

#define ETH_ARP 0x0806
#define ETH_IP  0x0800
#define IP_ICMP 1

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
    wr32(f + 28, OUR_IP);
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
    wr32(f + 28, OUR_IP);
    memcpy(f + 32, req + 22, 6);
    wr32(f + 38, rd32(req + 28));
    send_frame(f, sizeof(f));
}

/* --- requests that cannot be answered yet --------------------------------- */

#define PENDING_MAX 8
#define WAIT_ARP  1
#define WAIT_PING 2

static struct pending {
    int used;
    int kind;
    int handle;
    unsigned ip;
    unsigned seq;
    unsigned deadline;      /* in loop ticks */
} g_pending[PENDING_MAX];

static unsigned g_ticks;

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
    wr32(ip_hdr + 12, OUR_IP);
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

/* --- everything that arrives ---------------------------------------------- */

static void handle_frame(const uint8_t* f, unsigned len)
{
    ++g_in;
    if (len < 14)
        return;
    const unsigned type = rd16(f + 12);

    if (type == ETH_ARP && len >= 42) {
        const unsigned op = rd16(f + 20);
        const unsigned sender_ip = rd32(f + 28);
        if (op == 1 && rd32(f + 38) == OUR_IP) {
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
    if (ip_hdr[9] != IP_ICMP || len < 14 + ihl + 8)
        return;

    const uint8_t* icmp = ip_hdr + ihl;
    if (icmp[0] == 8 && rd32(ip_hdr + 16) == OUR_IP) {
        /* Somebody pinged us. Turn it round: same payload, type 0. */
        uint8_t r[1536];
        const unsigned n = len > sizeof(r) ? (unsigned)sizeof(r) : len;
        memcpy(r, f, n);
        memcpy(r, f + 6, 6);
        memcpy(r + 6, g_mac, 6);
        wr32(r + 14 + 12, OUR_IP);
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
    printf("netd: 10.0.2.15 up on %02x:%02x:%02x:%02x:%02x:%02x\n",
           g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5]);

    for (;;) {
        drain();
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
            r.word[0] = OUR_IP;
            r.word[1] = GATEWAY;
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
            const unsigned target = ((ip ^ OUR_IP) & 0xFFFFFF00u) ? GATEWAY : ip;
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
        } else {
            r.word[0] = -1;
            ipc_reply(handle, &r);
        }
    }
}
