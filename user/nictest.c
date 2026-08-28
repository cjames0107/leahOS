/* nictest - ask the ring 3 driver to put a real frame on the wire.
 *
 * An ARP request for the gateway, and the gateway's answer. Nothing about it
 * can be faked from inside this process: the frame has to reach the card, the
 * card has to put it on the link, something on the other end has to reply, and
 * the reply has to come back through the driver's ring. If any of the four
 * grants the driver was given were not real, none of that happens.
 */

#include <ipc.h>
#include <nic.h>
#include <shm.h>
#include <cli.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void put16(uint8_t* p, unsigned v) { p[0] = v >> 8; p[1] = v & 0xFF; }

int main(int argc, char** argv)
{
    cli_begin(argc, argv, "", "");
    int port = -1;
    for (int i = 0; i < 300 && port < 0; ++i) {
        port = port_open(IPC_PORT_NIC);
        if (port < 0) msleep(10);
    }
    if (port < 0) {
        printf("nictest: no driver is serving the card\n");
        return 1;
    }

    struct ipc_message q, a;
    memset(&q, 0, sizeof(q));
    memset(&a, 0, sizeof(a));
    q.tag = NIC_ATTACH;
    if (ipc_call(port, &q, &a) != 0) {
        printf("nictest: the driver would not attach\n");
        return 1;
    }
    uint8_t mac[6];
    memcpy(mac, a.data, 6);
    printf("nictest: driver reports %02x:%02x:%02x:%02x:%02x:%02x\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    const int shm = shm_open((unsigned)a.word[0], sizeof(struct nic_shared), 0);
    struct nic_shared* net = shm < 0 ? 0 : (struct nic_shared*)shm_map(shm);
    if (net == 0) {
        printf("nictest: cannot map the driver's frame buffer\n");
        return 1;
    }

    /* An ARP request for 10.0.2.2, which is what QEMU puts at the other end. */
    uint8_t* f = (uint8_t*)net->tx.data;
    memset(f, 0, 42);
    memset(f, 0xFF, 6);                 /* to everyone */
    memcpy(f + 6, mac, 6);
    put16(f + 12, 0x0806);              /* ARP */
    put16(f + 14, 0x0001);              /* over Ethernet */
    put16(f + 16, 0x0800);              /* for IPv4 */
    f[18] = 6; f[19] = 4;
    put16(f + 20, 0x0001);              /* a request */
    memcpy(f + 22, mac, 6);
    f[28] = 10; f[29] = 0; f[30] = 2; f[31] = 15;   /* from 10.0.2.15 */
    f[38] = 10; f[39] = 0; f[40] = 2; f[41] = 2;    /* about 10.0.2.2  */

    memset(&q, 0, sizeof(q));
    q.tag = NIC_SEND;
    q.word[0] = 42;
    if (ipc_call(port, &q, &a) != 0 || a.word[0] != 0) {
        printf("nictest: the driver would not send\n");
        return 1;
    }

    /* And wait for the answer to appear in the ring the driver fills. */
    for (int i = 0; i < 400; ++i) {
        while (net->rx_tail != net->rx_head) {
            const unsigned t = net->rx_tail;
            const uint8_t* r = (const uint8_t*)net->rx[t].data;
            const unsigned len = net->rx[t].length;
            const int is_arp_reply = len >= 42 &&
                r[12] == 0x08 && r[13] == 0x06 &&
                r[20] == 0x00 && r[21] == 0x02 &&
                r[28] == 10 && r[29] == 0 && r[30] == 2 && r[31] == 2;
            if (is_arp_reply) {
                printf("nictest: 10.0.2.2 is at "
                       "%02x:%02x:%02x:%02x:%02x:%02x\n",
                       r[22], r[23], r[24], r[25], r[26], r[27]);
                printf("  ok  a frame went out and an answer came back\n");
                return 0;
            }
            net->rx_tail = (t + 1) % NIC_RX_SLOTS;
        }
        msleep(10);
    }
    printf("nictest: nothing answered\n");
    return 1;
}
