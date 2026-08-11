#ifndef _BLK_H
#define _BLK_H

#include <stdint.h>

/* A disk, as seen from outside the process that drives it.
 *
 * Sectors do not travel in messages, for the same reason frames do not: 512
 * bytes will not fit in one and copying every block twice through the kernel
 * is the cost a microkernel is always accused of. The driver publishes one
 * shared segment and a request says which sectors and where in it.
 */

#define BLK_SHM_KEY   0x424Cu       /* "BL" */
/* The second driver publishes its own segment: two drivers sharing one buffer
 * would be two drivers writing over each other's transfers. */
#define BLK_SHM_KEY2  0x424Du       /* "BM" */

/* Disks are numbered across both drivers. The first BLK_MAX_DISKS belong to
 * the programmed-I/O driver on the legacy controller; anything at or above
 * that is on the AHCI controller, and a client subtracts to get the index the
 * second driver knows it by. */
#define BLK_AHCI_BASE BLK_MAX_DISKS
#define BLK_SECTOR    512
#define BLK_MAX_COUNT 64            /* 32 KiB in one request */

/* Several transfers' worth of room, not one.
 *
 * One buffer per driver means one transfer at a time, however many requests a
 * client can have outstanding - two threads reading different files would
 * hand the driver two requests and get each other's bytes back. The slot in
 * the request says which region to use, so concurrent transfers land in
 * different memory and nothing has to be serialised to keep them apart.
 *
 * A caller with nothing to co-ordinate uses slot zero and never thinks about
 * it, which is what every existing caller does by leaving the field at its
 * zeroed default. */
#define BLK_SLOTS 4

struct blk_shared {
    uint8_t data[BLK_SLOTS][BLK_SECTOR * BLK_MAX_COUNT];
};

/* Message tags on IPC_PORT_BLOCK. */
/* w2 names the disk, and zero is the one the system booted from - so a caller
 * that predates there being more than one asks for that one and is right. */
#define BLK_INFO  1     /* w2 = disk -> w0 = sectors, w1 = bytes, data = model */
#define BLK_READ  2     /* w0 = sector, w1 = count, w2 = disk, w3 = slot       */
#define BLK_WRITE 3     /* w0 = sector, w1 = count, w2 = disk, w3 = slot       */

#define BLK_MAX_DISKS 4

#endif
