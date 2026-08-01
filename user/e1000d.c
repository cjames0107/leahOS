/* e1000d - the network card's driver, in ring 3.
 *
 * This is the first driver to leave the kernel, and it is a fair test of
 * whether anything was really gained: it does PCI configuration, memory-mapped
 * registers, DMA rings the card walks by itself, and none of it in supervisor
 * mode. Everything it is allowed to do was asked for by name - eight I/O ports
 * for the configuration space, one BAR's worth of registers, and some memory
 * with a physical address. A bug here cannot reach the kernel, and when it
 * dies the machine does not.
 *
 * It is not a ring-1 driver, because on x86-64 that would be a ring-1 driver
 * with supervisor page access, which is a driver that can scribble on the
 * kernel while looking as though it cannot. See <driver.h>.
 */

#include <driver.h>
#include <ipc.h>
#include <nic.h>
#include <shm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* --- PCI, through the two configuration ports ---------------------------- */

#define PCI_ADDRESS 0xCF8
#define PCI_DATA    0xCFC

static unsigned cfg_read(unsigned bus, unsigned slot, unsigned fn, unsigned off)
{
    outl(PCI_ADDRESS, 0x80000000u | (bus << 16) | (slot << 11) | (fn << 8) |
                      (off & 0xFC));
    return inl(PCI_DATA);
}

static void cfg_write(unsigned bus, unsigned slot, unsigned fn, unsigned off,
                      unsigned value)
{
    outl(PCI_ADDRESS, 0x80000000u | (bus << 16) | (slot << 11) | (fn << 8) |
                      (off & 0xFC));
    outl(PCI_DATA, value);
}

/* --- e1000 registers, as offsets from BAR0 ------------------------------- */

#define CTRL   0x0000
#define STATUS 0x0008
#define RCTL   0x0100
#define TCTL   0x0400
#define TIPG   0x0410
#define RDBAL  0x2800
#define RDBAH  0x2804
#define RDLEN  0x2808
#define RDH    0x2810
#define RDT    0x2818
#define TDBAL  0x3800
#define TDBAH  0x3804
#define TDLEN  0x3808
#define TDH    0x3810
#define TDT    0x3818
#define RAL0   0x5400
#define RAH0   0x5404

#define CTRL_SLU   (1u << 6)
#define RCTL_EN    (1u << 1)
#define RCTL_UPE   (1u << 3)
#define RCTL_MPE   (1u << 4)    /* accept multicast */
#define RCTL_BAM   (1u << 15)
#define RCTL_SECRC (1u << 26)
#define TCTL_EN    (1u << 1)
#define TCTL_PSP   (1u << 3)

#define RX_DD      (1u << 0)
#define TX_EOP     (1u << 0)
#define TX_IFCS    (1u << 1)
#define TX_RS      (1u << 3)
#define TX_DD      (1u << 0)

#define RX_COUNT 32
#define TX_COUNT 32
#define BUF_SIZE 2048

struct rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed));

struct tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed));

static volatile uint8_t* g_regs;
static struct rx_desc*   g_rx;
static struct tx_desc*   g_tx;
static uint64_t g_rx_phys, g_tx_phys, g_rxbuf_phys, g_txbuf_phys;
static uint8_t* g_rxbuf;
static uint8_t* g_txbuf;
static unsigned g_tx_tail;
static uint8_t  g_mac[6];
static struct nic_shared* g_shared;
static unsigned long g_sent, g_received;

static void reg_write(unsigned off, unsigned value)
{
    *(volatile unsigned*)(g_regs + off) = value;
}

static unsigned reg_read(unsigned off)
{
    return *(volatile unsigned*)(g_regs + off);
}

/* Find the card. `want` selects which one when there is more than one, which is
 * how this can be tried out alongside the kernel's own driver rather than
 * having to replace it before it is known to work. */
static int find_card(int want, unsigned* bus_out, unsigned* slot_out)
{
    int seen = 0;
    for (unsigned bus = 0; bus < 4; ++bus) {
        for (unsigned slot = 0; slot < 32; ++slot) {
            const unsigned id = cfg_read(bus, slot, 0, 0);
            if ((id & 0xFFFF) != 0x8086)
                continue;
            const unsigned dev = id >> 16;
            /* 100E is the card QEMU gives us; the others are the same silicon
             * as far as anything here is concerned. */
            if (dev != 0x100E && dev != 0x153A && dev != 0x10D3)
                continue;
            if (seen++ != want)
                continue;
            *bus_out = bus;
            *slot_out = slot;
            return 0;
        }
    }
    return -1;
}

static int bring_up(int which)
{
    /* The configuration ports first: without them nothing else can even be
     * found. Eight ports, which is the address register and the data register
     * and nothing else. */
    if (io_permit(PCI_ADDRESS, 8) != 0) {
        printf("e1000d: refused the PCI configuration ports\n");
        return -1;
    }

    unsigned bus, slot;
    if (find_card(which, &bus, &slot) != 0) {
        printf("e1000d: no e1000 at index %d\n", which);
        return -1;
    }

    /* Memory space and bus mastering: the card reads its descriptors and the
     * frames out of memory by itself, and cannot do that until it is told it
     * may. */
    const unsigned command = cfg_read(bus, slot, 0, 0x04);
    cfg_write(bus, slot, 0, 0x04, command | (1u << 1) | (1u << 2));

    const unsigned bar0 = cfg_read(bus, slot, 0, 0x10) & ~0xFu;
    g_regs = (volatile uint8_t*)map_physical(bar0, 0x20000);
    if (g_regs == 0) {
        printf("e1000d: cannot map the registers at %x\n", bar0);
        return -1;
    }

    /* The MAC is in the receive-address registers after reset, and the high
     * bit of RAH says it is real. */
    const unsigned low = reg_read(RAL0), high = reg_read(RAH0);
    if ((high & (1u << 31)) == 0) {
        printf("e1000d: the card has no address\n");
        return -1;
    }
    g_mac[0] = low & 0xFF;         g_mac[1] = (low >> 8) & 0xFF;
    g_mac[2] = (low >> 16) & 0xFF; g_mac[3] = (low >> 24) & 0xFF;
    g_mac[4] = high & 0xFF;        g_mac[5] = (high >> 8) & 0xFF;

    reg_write(CTRL, reg_read(CTRL) | CTRL_SLU);

    g_rx    = (struct rx_desc*)dma_alloc(RX_COUNT * sizeof(struct rx_desc), &g_rx_phys);
    g_tx    = (struct tx_desc*)dma_alloc(TX_COUNT * sizeof(struct tx_desc), &g_tx_phys);
    g_rxbuf = (uint8_t*)dma_alloc(RX_COUNT * BUF_SIZE, &g_rxbuf_phys);
    g_txbuf = (uint8_t*)dma_alloc(TX_COUNT * BUF_SIZE, &g_txbuf_phys);
    if (g_rx == 0 || g_tx == 0 || g_rxbuf == 0 || g_txbuf == 0) {
        printf("e1000d: cannot get memory the card can reach\n");
        return -1;
    }

    for (int i = 0; i < RX_COUNT; ++i) {
        g_rx[i].addr = g_rxbuf_phys + (uint64_t)i * BUF_SIZE;
        g_rx[i].status = 0;
    }
    reg_write(RDBAL, (unsigned)g_rx_phys);
    reg_write(RDBAH, (unsigned)(g_rx_phys >> 32));
    reg_write(RDLEN, RX_COUNT * sizeof(struct rx_desc));
    reg_write(RDH, 0);
    reg_write(RDT, RX_COUNT - 1);
    /* Promiscuous for unicast and multicast both. Unicast because QEMU's
     * receive-address filter drops our own replies even with the address
     * programmed, and on a point-to-point link there is nothing to sift out
     * anyway.
     *
     * Multicast because IPv6 has no broadcast. Every router advertisement,
     * every neighbour solicitation - the whole of discovery - arrives on a
     * multicast address, so a card set up the way IPv4 was happy with receives
     * none of it and the machine simply never learns that a network is there.
     * That is what happened: the link-local address formed, the solicitation
     * went out, and nothing ever came back. */
    reg_write(RCTL, RCTL_EN | RCTL_UPE | RCTL_MPE | RCTL_BAM | RCTL_SECRC);

    for (int i = 0; i < TX_COUNT; ++i)
        g_tx[i].status = TX_DD;
    reg_write(TDBAL, (unsigned)g_tx_phys);
    reg_write(TDBAH, (unsigned)(g_tx_phys >> 32));
    reg_write(TDLEN, TX_COUNT * sizeof(struct tx_desc));
    reg_write(TDH, 0);
    reg_write(TDT, 0);
    reg_write(TCTL, TCTL_EN | TCTL_PSP | (0x0F << 4) | (0x40 << 12));
    reg_write(TIPG, 10 | (8 << 10) | (6 << 20));
    g_tx_tail = 0;
    return 0;
}

static int transmit(const uint8_t* frame, unsigned length)
{
    if (length == 0 || length > BUF_SIZE)
        return -1;
    struct tx_desc* d = &g_tx[g_tx_tail];
    if ((d->status & TX_DD) == 0)
        return -1;                  /* the ring is full; the card is behind */

    memcpy(g_txbuf + (unsigned long)g_tx_tail * BUF_SIZE, frame, length);
    d->addr   = g_txbuf_phys + (uint64_t)g_tx_tail * BUF_SIZE;
    d->length = (uint16_t)length;
    d->cmd    = TX_EOP | TX_IFCS | TX_RS;
    d->status = 0;

    g_tx_tail = (g_tx_tail + 1) % TX_COUNT;
    reg_write(TDT, g_tx_tail);
    ++g_sent;
    return 0;
}

/* Move what the card has finished receiving into the shared ring.
 *
 * Bounded to one pass round the ring rather than "until there is nothing
 * left". A link with enough traffic on it can refill the ring as fast as this
 * empties it, and an unbounded loop then never returns - the driver stops
 * answering its port and the machine appears to have hung, which is exactly
 * what happened the moment multicast was let in. Whatever is left is still
 * there next time round the loop, a millisecond later.
 */
static void pump_receive(void)
{
    unsigned tail = reg_read(RDT);
    for (unsigned pass = 0; pass < RX_COUNT; ++pass) {
        const unsigned index = (tail + 1) % RX_COUNT;
        struct rx_desc* d = &g_rx[index];
        if ((d->status & RX_DD) == 0)
            break;

        const unsigned head = g_shared->rx_head;
        const unsigned next = (head + 1) % NIC_RX_SLOTS;
        if (next == g_shared->rx_tail) {
            /* Nobody is reading. Drop it and say so rather than stalling the
             * card - a driver that blocks because its client is slow takes the
             * link down for everyone. */
            g_shared->dropped = g_shared->dropped + 1;
        } else if (d->length > 0 && d->length <= NIC_FRAME_MAX) {
            memcpy((void*)g_shared->rx[head].data,
                   g_rxbuf + (unsigned long)index * BUF_SIZE, d->length);
            g_shared->rx[head].length = d->length;
            /* The length is written before the head moves, so a reader that
             * sees the slot sees a complete frame in it. */
            __atomic_store_n(&g_shared->rx_head, next, __ATOMIC_RELEASE);
            ++g_received;
        }

        d->status = 0;
        tail = index;
        reg_write(RDT, tail);
    }
}

int main(int argc, char** argv)
{
    const int which = argc > 1 ? atoi_simple(argv[1]) : 0;

    if (bring_up(which) != 0)
        return 1;

    const int shm = shm_open(NIC_SHM_KEY, sizeof(struct nic_shared), SHM_PUBLIC);
    if (shm < 0) {
        printf("e1000d: cannot publish the frame buffer\n");
        return 1;
    }
    g_shared = (struct nic_shared*)shm_map(shm);
    if (g_shared == 0) {
        printf("e1000d: cannot map the frame buffer\n");
        return 1;
    }
    memset(g_shared, 0, sizeof(*g_shared));

    const int port = port_create(IPC_PORT_NIC);
    if (port < 0) {
        printf("e1000d: the card already has a driver\n");
        return 1;
    }

    printf("e1000d[%d]: %02x:%02x:%02x:%02x:%02x:%02x up in ring 3\n",
           getpid(), g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5]);

    for (;;) {
        pump_receive();

        struct ipc_message m, r;
        unsigned from = 0;
        const int handle = ipc_try_recv(port, &m, &from);
        if (handle < 0) {
            /* Nothing to answer and nothing arrived. A millisecond is short
             * enough that a packet is never waiting long and long enough that
             * an idle link costs nothing. */
            msleep(1);
            continue;
        }

        memset(&r, 0, sizeof(r));
        r.tag = m.tag;
        if (m.tag == NIC_ATTACH) {
            r.word[0] = NIC_SHM_KEY;
            memcpy(r.data, g_mac, 6);
            r.bytes = 6;
        } else if (m.tag == NIC_SEND) {
            r.word[0] = transmit((const uint8_t*)g_shared->tx.data,
                                 (unsigned)m.word[0]);
        } else if (m.tag == NIC_STATS) {
            r.word[0] = (long)g_sent;
            r.word[1] = (long)g_received;
            r.word[2] = (long)g_shared->dropped;
        } else {
            r.word[0] = -1;
        }
        ipc_reply(handle, &r);
    }
}
