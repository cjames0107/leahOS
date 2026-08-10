/* blockd - the disk's driver, in ring 3.
 *
 * ATA in programmed I/O: the drive is told which sectors to fetch and then the
 * data is read a word at a time out of a port. Slower than letting it write
 * straight to memory, and chosen anyway - it needs nothing but the eight ports
 * this process was granted, which means the whole driver is reviewable against
 * one thing it is allowed to touch.
 *
 * Which channel it drives is an argument, because while the kernel still has a
 * filesystem of its own the two must not both be poking the same registers.
 */

#include <blk.h>
#include <driver.h>
#include <ipc.h>
#include <shm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Register offsets from the channel's base. */
#define REG_DATA     0
#define REG_ERROR    1
#define REG_COUNT    2
#define REG_LBA_LOW  3
#define REG_LBA_MID  4
#define REG_LBA_HIGH 5
#define REG_DRIVE    6
#define REG_STATUS   7
#define REG_COMMAND  7

#define ST_ERR  0x01
#define ST_DRQ  0x08
#define ST_DF   0x20
#define ST_BSY  0x80

#define CMD_READ_PIO  0x20
#define CMD_WRITE_PIO 0x30
#define CMD_FLUSH     0xE7
#define CMD_IDENTIFY  0xEC

/* Every drive the two legacy channels can hold. The system's own disk is
 * whichever one was asked for on the command line and is always index 0, so
 * that a caller from before there was a choice still names it correctly. */
struct drive {
    int           present;
    unsigned      io, control;
    int           slave;
    unsigned long sectors;
    char          model[44];
};

static struct drive g_disk[BLK_MAX_DISKS];
static unsigned     g_disks;
static unsigned     g_at = 0xFFFFFFFFu;  /* which one the ports are set for */

/* The drive currently being talked to. identify() and the read and write paths
 * were written against these and still are; what changed is that they are no
 * longer the only ones there could be. */
static unsigned g_io;
static unsigned g_control;
static int      g_slave;
static unsigned long g_sectors;
static char     g_model[44];

/* Point the globals above at one of the drives. */
static int use_disk(unsigned which)
{
    if (which >= g_disks || !g_disk[which].present)
        return -1;
    if (which == g_at)
        return 0;
    g_io      = g_disk[which].io;
    g_control = g_disk[which].control;
    g_slave   = g_disk[which].slave;
    g_sectors = g_disk[which].sectors;
    memcpy(g_model, g_disk[which].model, sizeof(g_model));
    g_at = which;
    return 0;
}
static struct blk_shared* g_shared;

/* A status read settles in about 400ns, which is four reads of the alternate
 * status register - the conventional way to wait that long without a clock. */
static void settle(void)
{
    for (int i = 0; i < 4; ++i)
        (void)inb(g_control);
}

static int wait_not_busy(void)
{
    for (unsigned i = 0; i < 1000000; ++i)
        if ((inb(g_io + REG_STATUS) & ST_BSY) == 0)
            return 0;
    return -1;
}

static int wait_for_data(void)
{
    for (unsigned i = 0; i < 1000000; ++i) {
        const unsigned char s = inb(g_io + REG_STATUS);
        if (s & ST_BSY)
            continue;
        if (s & (ST_ERR | ST_DF))
            return -1;
        if (s & ST_DRQ)
            return 0;
    }
    return -1;
}

static void select_drive(unsigned char mode, unsigned char high_lba)
{
    outb(g_io + REG_DRIVE,
         (unsigned char)(mode | (g_slave ? 0x10 : 0x00) | (high_lba & 0x0F)));
    settle();
}

static int begin(unsigned long lba, unsigned count, int writing)
{
    if (wait_not_busy() != 0)
        return -1;
    /* LBA28 only. The addresses this system uses are nowhere near the limit,
     * and the 48-bit form is a second code path to get wrong for no gain. */
    if (lba + count > 0x0FFFFFFFul)
        return -1;
    select_drive(0xE0, (unsigned char)(lba >> 24));
    outb(g_io + REG_COUNT, (unsigned char)count);
    outb(g_io + REG_LBA_LOW, (unsigned char)lba);
    outb(g_io + REG_LBA_MID, (unsigned char)(lba >> 8));
    outb(g_io + REG_LBA_HIGH, (unsigned char)(lba >> 16));
    outb(g_io + REG_COMMAND, writing ? CMD_WRITE_PIO : CMD_READ_PIO);
    return 0;
}

static int read_sectors(unsigned long lba, unsigned count, unsigned char* out)
{
    if (begin(lba, count, 0) != 0)
        return -1;
    unsigned short* p = (unsigned short*)out;
    for (unsigned s = 0; s < count; ++s) {
        /* Still a sector at a time: the drive raises DRQ once per sector and
         * the status has to be read between them. Within a sector the words
         * move in one instruction - see insw in driver.h for why that matters
         * so much more than it looks like it should. */
        if (wait_for_data() != 0)
            return -1;
        insw(g_io + REG_DATA, p, BLK_SECTOR / 2);
        p += BLK_SECTOR / 2;
    }
    return 0;
}

static int write_sectors(unsigned long lba, unsigned count,
                         const unsigned char* in)
{
    if (begin(lba, count, 1) != 0)
        return -1;
    const unsigned short* p = (const unsigned short*)in;
    for (unsigned s = 0; s < count; ++s) {
        if (wait_for_data() != 0)
            return -1;
        outsw(g_io + REG_DATA, p, BLK_SECTOR / 2);
        p += BLK_SECTOR / 2;
    }
    /* Without a flush the data can sit in the drive's own cache, and a reset
     * would lose a write this driver has already said it made. */
    outb(g_io + REG_COMMAND, CMD_FLUSH);
    return wait_not_busy();
}

/* IDENTIFY returns the model as 16-bit words with the two bytes the wrong way
 * round, which is the one thing everybody remembers about ATA. */
static int identify(void)
{
    if (wait_not_busy() != 0)
        return -1;
    select_drive(0xA0, 0);
    outb(g_io + REG_COUNT, 0);
    outb(g_io + REG_LBA_LOW, 0);
    outb(g_io + REG_LBA_MID, 0);
    outb(g_io + REG_LBA_HIGH, 0);
    outb(g_io + REG_COMMAND, CMD_IDENTIFY);
    if (inb(g_io + REG_STATUS) == 0)
        return -1;                  /* nothing there at all */
    if (wait_for_data() != 0)
        return -1;

    unsigned short id[256];
    for (int i = 0; i < 256; ++i)
        id[i] = inw(g_io + REG_DATA);

    for (int i = 0; i < 20; ++i) {
        g_model[i * 2]     = (char)(id[27 + i] >> 8);
        g_model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    g_model[40] = '\0';
    for (int i = 39; i >= 0 && g_model[i] == ' '; --i)
        g_model[i] = '\0';

    g_sectors = (unsigned long)id[60] | ((unsigned long)id[61] << 16);
    return g_sectors > 0 ? 0 : -1;
}

int main(int argc, char** argv)
{
    /* The root disk by default: primary channel, second drive. That is where
     * the filesystem is, and this is now the only driver that can reach it -
     * the kernel has none. The arguments are still there for pointing a second
     * copy at some other disk. */
    const int channel = argc > 1 ? atoi_simple(argv[1]) : 0;
    const int slave   = argc > 2 ? atoi_simple(argv[2]) : 1;

    /* Both channels, because a second filesystem is on one of them. */
    if (io_permit(0x1F0, 8) != 0 || io_permit(0x3F6, 1) != 0 ||
        io_permit(0x170, 8) != 0 || io_permit(0x376, 1) != 0) {
        printf("blockd: refused the drives' ports\n");
        return 1;
    }

    /* The one that was asked for goes first, so it is disk 0 - the index every
     * caller written before there was a choice already asks for. Then every
     * other drive the two channels hold, in the order the hardware has them. */
    static const struct { unsigned io, control; int slave; } kSlots[] = {
        { 0x1F0, 0x3F6, 0 }, { 0x1F0, 0x3F6, 1 },
        { 0x170, 0x376, 0 }, { 0x170, 0x376, 1 },
    };
    const unsigned wanted = (unsigned)(channel == 0 ? 0 : 2) +
                            (slave != 0 ? 1u : 0u);

    for (unsigned pass = 0; pass < 2; ++pass) {
        for (unsigned i = 0; i < 4 && g_disks < BLK_MAX_DISKS; ++i) {
            if ((pass == 0) != (i == wanted))
                continue;               /* the asked-for one first, then the rest */
            g_io      = kSlots[i].io;
            g_control = kSlots[i].control;
            g_slave   = kSlots[i].slave;

            /* Polled, not interrupt driven: an unhandled interrupt on a shared
             * line is a way to wedge the machine. */
            outb(g_control, 0x02);
            settle();
            if (identify() != 0)
                continue;

            struct drive* d = &g_disk[g_disks++];
            d->present = 1;
            d->io = g_io;
            d->control = g_control;
            d->slave = g_slave;
            d->sectors = g_sectors;
            memcpy(d->model, g_model, sizeof(d->model));
        }
    }

    if (g_disks == 0) {
        printf("blockd: no drive on either channel\n");
        return 1;
    }
    g_at = 0xFFFFFFFFu;
    use_disk(0);

    const int shm = shm_open(BLK_SHM_KEY, sizeof(struct blk_shared), SHM_PUBLIC);
    g_shared = shm < 0 ? 0 : (struct blk_shared*)shm_map(shm);
    if (g_shared == 0) {
        printf("blockd: cannot publish the transfer buffer\n");
        return 1;
    }

    const int port = port_create(IPC_PORT_BLOCK);
    if (port < 0) {
        printf("blockd: a driver already has the disk\n");
        return 1;
    }

    printf("blockd[%d]: %s, %lu sectors, in ring 3\n",
           getpid(), g_disk[0].model, g_disk[0].sectors);
    for (unsigned i = 1; i < g_disks; ++i)
        printf("blockd: disk %u, %s, %lu sectors\n",
               i, g_disk[i].model, g_disk[i].sectors);

    for (;;) {
        struct ipc_message m, r;
        unsigned from = 0;
        const int handle = ipc_recv(port, &m, &from);
        if (handle < 0)
            return 1;

        memset(&r, 0, sizeof(r));
        r.tag = m.tag;
        /* Which disk, for every request that names one. */
        if (m.tag == BLK_INFO || m.tag == BLK_READ || m.tag == BLK_WRITE) {
            if (use_disk((unsigned)m.word[2]) != 0) {
                r.word[0] = -1;
                ipc_reply(handle, &r);
                continue;
            }
        }

        if (m.tag == BLK_INFO) {
            r.word[0] = (long)g_sectors;
            r.word[1] = BLK_SECTOR;
            unsigned n = 0;
            while (g_model[n] != '\0' && n < sizeof(r.data) - 1) {
                r.data[n] = g_model[n];
                ++n;
            }
            r.bytes = n;
        } else if (m.tag == BLK_READ || m.tag == BLK_WRITE) {
            const unsigned long lba = (unsigned long)m.word[0];
            unsigned count = (unsigned)m.word[1];
            if (count == 0 || count > BLK_MAX_COUNT) {
                r.word[0] = -1;
            } else {
                r.word[0] = (m.tag == BLK_READ)
                    ? read_sectors(lba, count, g_shared->data)
                    : write_sectors(lba, count, g_shared->data);
            }
        } else {
            r.word[0] = -1;
        }
        ipc_reply(handle, &r);
    }
}
