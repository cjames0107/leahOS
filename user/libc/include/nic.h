#ifndef _NIC_H
#define _NIC_H

#include <stdint.h>

/* What a network card looks like from outside the process that drives it.
 *
 * Frames do not travel in the message. An Ethernet frame is up to 1514 bytes
 * and an IPC message carries 256, but the real reason is the one in <ipc.h>:
 * copying every packet twice through the kernel is how a microkernel earns its
 * reputation. So the driver publishes one shared segment, and a message says
 * where in it to look.
 *
 * Sending is a call, because the sender wants to know it was queued. Receiving
 * is not a call at all - the driver writes arriving frames into a ring in the
 * same segment and the reader takes them out, so a packet arriving costs no
 * system call on either side.
 */

#define NIC_SHM_KEY   0x4E49u       /* "NI" */
#define NIC_FRAME_MAX 1536
#define NIC_RX_SLOTS  32

struct nic_slot {
    volatile uint32_t length;
    uint8_t  data[NIC_FRAME_MAX];
};

struct nic_shared {
    volatile uint32_t rx_head;      /* the driver writes here  */
    volatile uint32_t rx_tail;      /* the reader consumes here */
    volatile uint32_t dropped;      /* frames the ring had no room for */
    volatile uint32_t reserved;
    struct nic_slot tx;             /* one at a time: a send is a call */
    struct nic_slot rx[NIC_RX_SLOTS];
};

/* Message tags on IPC_PORT_NIC. */
#define NIC_ATTACH 1    /* reply: word[0] = shm key, data = 6 MAC bytes      */
#define NIC_SEND   2    /* word[0] = length, frame already written to tx     */
#define NIC_STATS  3    /* word[0] = sent, word[1] = received, word[2] = dropped */

#endif
