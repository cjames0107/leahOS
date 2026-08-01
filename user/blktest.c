/* blktest - read a real disk through the ring 3 driver.
 *
 * The disk it is pointed at holds an ext4 filesystem, so the checks are
 * against structure that is really on the platter rather than against a
 * pattern this program wrote a moment ago: the superblock's magic number sits
 * at a fixed offset and cannot be produced by a driver that is quietly
 * returning zeroes.
 */

#include <blk.h>
#include <ipc.h>
#include <shm.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    int port = -1;
    for (int i = 0; i < 300 && port < 0; ++i) {
        port = port_open(IPC_PORT_BLOCK);
        if (port < 0) msleep(10);
    }
    if (port < 0) {
        printf("blktest: no driver is serving a disk\n");
        return 1;
    }

    struct ipc_message q, a;
    memset(&q, 0, sizeof(q));
    memset(&a, 0, sizeof(a));
    q.tag = BLK_INFO;
    if (ipc_call(port, &q, &a) != 0) {
        printf("blktest: the driver did not answer\n");
        return 1;
    }
    printf("blktest: %s, %ld sectors of %ld bytes\n",
           a.data, (long)a.word[0], (long)a.word[1]);

    const int shm = shm_open(BLK_SHM_KEY, sizeof(struct blk_shared), 0);
    struct blk_shared* buf = shm < 0 ? 0 : (struct blk_shared*)shm_map(shm);
    if (buf == 0) {
        printf("blktest: cannot map the transfer buffer\n");
        return 1;
    }

    int failures = 0;

    /* The superblock lives 1024 bytes in, so it is inside the first four
     * sectors; its magic is two bytes at offset 56 within it. */
    memset(&q, 0, sizeof(q));
    q.tag = BLK_READ;
    q.word[0] = 0;
    q.word[1] = 8;
    if (ipc_call(port, &q, &a) != 0 || a.word[0] != 0) {
        printf("blktest: the read failed\n");
        return 1;
    }
    const unsigned magic = (unsigned)buf->data[1024 + 56] |
                           ((unsigned)buf->data[1024 + 57] << 8);
    if (magic == 0xEF53) {
        printf("  ok  read an ext4 superblock off the platter\n");
    } else {
        printf("  FAIL magic was %x, wanted ef53\n", magic);
        ++failures;
    }

    /* And a write, somewhere no filesystem structure lives. The image is
     * opened with -snapshot, so nothing here outlives the machine. */
    const unsigned long spare = 40000;
    for (int i = 0; i < BLK_SECTOR; ++i)
        buf->data[i] = (unsigned char)(i * 7 + 3);
    memset(&q, 0, sizeof(q));
    q.tag = BLK_WRITE;
    q.word[0] = (long)spare;
    q.word[1] = 1;
    const int wrote = ipc_call(port, &q, &a) == 0 && a.word[0] == 0;

    memset(buf->data, 0, BLK_SECTOR);
    memset(&q, 0, sizeof(q));
    q.tag = BLK_READ;
    q.word[0] = (long)spare;
    q.word[1] = 1;
    int same = wrote && ipc_call(port, &q, &a) == 0 && a.word[0] == 0;
    for (int i = 0; same && i < BLK_SECTOR; ++i)
        if (buf->data[i] != (unsigned char)(i * 7 + 3))
            same = 0;
    if (same) {
        printf("  ok  a sector written came back the same\n");
    } else {
        printf("  FAIL the write did not survive a read\n");
        ++failures;
    }

    return failures;
}
