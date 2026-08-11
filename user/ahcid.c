/* ahcid - the SATA controller, found and described.
 *
 * This is the first half of a disk driver: everything up to the point where it
 * would move data. It finds the controller on the bus, maps its registers,
 * takes ownership from the firmware if the firmware still has it, and reports
 * which ports have something attached and what the controller can do.
 *
 * Deliberately stopping there. The half that issues commands writes to disks,
 * and a disk driver that looks right is worth nothing - it has to be watched
 * doing the right thing, and the harness that would watch it needs the read
 * path to exist before the write path is worth trusting. Discovery is the part
 * that can be checked by reading what it prints, so it is the part that goes
 * first.
 *
 * The pieces this needs already existed: io_permit for config space, since PCI
 * configuration is two ports, and map_physical for the registers, since AHCI
 * puts them in memory rather than in the I/O space the old controller used.
 */

#include <blk.h>
#include <driver.h>
#include <ipc.h>
#include <shm.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define PCI_ADDRESS 0xCF8
#define PCI_DATA    0xCFC

/* Where AHCI lives in the class codes: a mass storage controller (1), of the
 * SATA kind (6), speaking AHCI rather than something vendor-specific (1). */
#define CLASS_STORAGE 0x01
#define SUBCLASS_SATA 0x06
#define PROGIF_AHCI   0x01

/* Host registers, by byte offset into the mapped area. */
#define HBA_CAP   0x00      /* what the controller can do          */
#define HBA_GHC   0x04      /* global control; bit 31 is AHCI mode */
#define HBA_PI    0x0C      /* which ports are implemented         */
#define HBA_VS    0x10      /* the version of the specification    */
#define HBA_CAP2  0x24
#define HBA_BOHC  0x28      /* handover, when the firmware has it  */

/* And per port, at 0x100 + port * 0x80. */
#define PORT_BASE(n)  (0x100u + (unsigned)(n) * 0x80u)
#define PORT_CLB      0x00  /* command list, and its high half     */
#define PORT_CLBU     0x04
#define PORT_FB       0x08  /* where the device's replies land     */
#define PORT_FBU      0x0C
#define PORT_IS       0x10  /* what happened                       */
#define PORT_CMD      0x18  /* start, stop, and whether it is going*/
#define PORT_TFD      0x20  /* the device's status and error       */
#define PORT_SIG      0x24  /* what kind of device it is           */
#define PORT_SSTS     0x28  /* device detection and speed          */
#define PORT_SERR     0x30
#define PORT_CI       0x38  /* set a bit to issue that slot        */

#define CMD_ST        (1u << 0)     /* run the command list        */
#define CMD_FRE       (1u << 4)     /* accept replies              */
#define CMD_FR        (1u << 14)    /* replies are being accepted  */
#define CMD_CR        (1u << 15)    /* the list is running         */

#define TFD_ERR       (1u << 0)
#define TFD_DRQ       (1u << 3)
#define TFD_BSY       (1u << 7)

#define IS_TFES       (1u << 30)    /* the device refused it       */

#define ATA_READ_DMA_EX  0x25
#define ATA_WRITE_DMA_EX 0x35

#define SIG_ATA       0x00000101u
#define SIG_ATAPI     0xEB140101u

static unsigned cfg_read(unsigned bus, unsigned slot, unsigned fn, unsigned off)
{
    const unsigned address = 0x80000000u | (bus << 16) | (slot << 11) |
                             (fn << 8) | (off & 0xFC);
    outl(PCI_ADDRESS, address);
    return inl(PCI_DATA);
}

static void cfg_write(unsigned bus, unsigned slot, unsigned fn, unsigned off,
                      unsigned value)
{
    const unsigned address = 0x80000000u | (bus << 16) | (slot << 11) |
                             (fn << 8) | (off & 0xFC);
    outl(PCI_ADDRESS, address);
    outl(PCI_DATA, value);
}

static volatile unsigned* g_hba;        /* the controller's registers */
/* Every port with a disk on it.
 *
 * The command list and the reply area are per port, because the controller
 * writes into them on its own schedule and two ports sharing either would be
 * two conversations in one notebook. The command table and the data buffer are
 * not: this driver issues one command and waits for it, so there is never a
 * second transfer in flight to want them. */
#define MAX_DISKS 4

struct disk {
    unsigned      port;
    unsigned long sectors;
    volatile unsigned char* cmd_list;
    volatile unsigned char* fis;
    uint64_t cmd_list_phys, fis_phys;
};

static struct disk g_disk[MAX_DISKS];
static unsigned    g_disks;

/* The structures the controller reads out of memory, so they have to be where
 * it can reach them: allocated once, physically contiguous, and never moved.
 * dma_alloc hands back both addresses because the controller needs the
 * physical one and this program needs the virtual one. */
static volatile unsigned char* g_cmd_table;     /* one command's detail */
static volatile unsigned char* g_buffer;        /* the data itself      */
static uint64_t g_cmd_table_phys, g_buffer_phys;

/* As much as the protocol will ever ask for in one request.
 *
 * This was a single page, which is smaller than the largest transfer a client
 * may send - so the filesystem's read-ahead, which asks for exactly the
 * maximum, was refused every time and fell back to one block. Every read of
 * the root disk was costing a rejected request and then a small one. A driver
 * whose buffer is smaller than its protocol is a driver that silently does the
 * slow thing. */
#define BUFFER_BYTES (BLK_SECTOR * BLK_MAX_COUNT)

static unsigned hba(unsigned off)
{
    return g_hba[off / 4];
}

static void hba_set(unsigned off, unsigned value)
{
    g_hba[off / 4] = value;
}

static void wr32(volatile unsigned char* at, unsigned off, unsigned value)
{
    *(volatile unsigned*)(at + off) = value;
}

/* Wait for a device to stop being busy. Returns 0 when it is ready. */
static int wait_ready(unsigned d)
{
    for (int spin = 0; spin < 2000000; ++spin) {
        const unsigned tfd = hba(PORT_BASE(g_disk[d].port) + PORT_TFD);
        if ((tfd & (TFD_BSY | TFD_DRQ)) == 0)
            return 0;
    }
    return -1;
}

/* Stop the port, point it at our structures, and start it again.
 *
 * The order is the controller's, not a preference: it will not accept new
 * addresses while it is running, and it will not start until it has stopped -
 * so both waits are real and skipping either leaves it reading whatever the
 * firmware left behind. */
static int port_start(unsigned d)
{
    const unsigned base = PORT_BASE(g_disk[d].port);

    hba_set(base + PORT_CMD, hba(base + PORT_CMD) & ~CMD_ST);
    for (int spin = 0; spin < 1000000; ++spin)
        if ((hba(base + PORT_CMD) & CMD_CR) == 0)
            break;
    hba_set(base + PORT_CMD, hba(base + PORT_CMD) & ~CMD_FRE);
    for (int spin = 0; spin < 1000000; ++spin)
        if ((hba(base + PORT_CMD) & CMD_FR) == 0)
            break;
    if ((hba(base + PORT_CMD) & (CMD_CR | CMD_FR)) != 0)
        return -1;

    hba_set(base + PORT_CLB,  (unsigned)(g_disk[d].cmd_list_phys & 0xFFFFFFFFu));
    hba_set(base + PORT_CLBU, (unsigned)(g_disk[d].cmd_list_phys >> 32));
    hba_set(base + PORT_FB,   (unsigned)(g_disk[d].fis_phys & 0xFFFFFFFFu));
    hba_set(base + PORT_FBU,  (unsigned)(g_disk[d].fis_phys >> 32));

    hba_set(base + PORT_SERR, hba(base + PORT_SERR));   /* write to clear */
    hba_set(base + PORT_IS, hba(base + PORT_IS));

    hba_set(base + PORT_CMD, hba(base + PORT_CMD) | CMD_FRE);
    hba_set(base + PORT_CMD, hba(base + PORT_CMD) | CMD_ST);
    return 0;
}

/* One transfer, in or out, of up to BUFFER_BYTES from the shared buffer.
 *
 * Slot zero every time: this issues one command and waits for it, so there is
 * never a second in flight to collide with. Thirty-two slots are for a driver
 * that keeps several going, which this is not yet. */
static int transfer(unsigned d, unsigned long lba, unsigned sectors, int write)
{
    if (d >= g_disks || sectors == 0 || sectors * 512u > BUFFER_BYTES)
        return -1;
    if (wait_ready(d) != 0)
        return -1;

    const unsigned base = PORT_BASE(g_disk[d].port);
    hba_set(base + PORT_IS, hba(base + PORT_IS));

    /* The header: a five-dword command FIS, one region to scatter into, and
     * the direction. */
    volatile unsigned* header = (volatile unsigned*)g_disk[d].cmd_list;
    header[0] = (5u) | (write ? (1u << 6) : 0u) | (1u << 16);
    header[1] = 0;                                  /* bytes moved so far */
    header[2] = (unsigned)(g_cmd_table_phys & 0xFFFFFFFFu);
    header[3] = (unsigned)(g_cmd_table_phys >> 32);
    header[4] = header[5] = header[6] = header[7] = 0;

    memset((void*)g_cmd_table, 0, 256);

    /* The command itself, as a host-to-device register FIS. */
    volatile unsigned char* fis = g_cmd_table;
    fis[0] = 0x27;                                  /* register FIS, h2d */
    fis[1] = 0x80;                                  /* this is a command */
    fis[2] = write ? ATA_WRITE_DMA_EX : ATA_READ_DMA_EX;
    fis[3] = 0;
    fis[4] = (unsigned char)(lba);
    fis[5] = (unsigned char)(lba >> 8);
    fis[6] = (unsigned char)(lba >> 16);
    fis[7] = 0x40;                                  /* LBA, not CHS      */
    fis[8] = (unsigned char)(lba >> 24);
    fis[9] = (unsigned char)(lba >> 32);
    fis[10] = (unsigned char)(lba >> 40);
    fis[11] = 0;
    fis[12] = (unsigned char)(sectors);
    fis[13] = (unsigned char)(sectors >> 8);

    /* And where the data is. The count is written one short of the length,
     * which is the controller's convention and not a mistake. */
    volatile unsigned* prdt = (volatile unsigned*)(g_cmd_table + 0x80);
    prdt[0] = (unsigned)(g_buffer_phys & 0xFFFFFFFFu);
    prdt[1] = (unsigned)(g_buffer_phys >> 32);
    prdt[2] = 0;
    prdt[3] = (sectors * 512u) - 1u;

    hba_set(base + PORT_CI, 1u);                    /* slot zero, go */

    for (int spin = 0; spin < 5000000; ++spin) {
        if ((hba(base + PORT_CI) & 1u) == 0)
            break;
        if ((hba(base + PORT_IS) & IS_TFES) != 0)
            return -1;
    }
    if ((hba(base + PORT_CI) & 1u) != 0)
        return -1;                                  /* never finished */
    if ((hba(base + PORT_IS) & IS_TFES) != 0 ||
        (hba(base + PORT_TFD) & TFD_ERR) != 0)
        return -1;
    return 0;
}

/* Ask the disk how big it is.
 *
 * IDENTIFY returns 512 bytes rather than moving sectors, but over AHCI it is
 * issued exactly like a read - the difference is entirely in the command byte
 * and in what comes back. */
static int identify(unsigned d)
{
    if (wait_ready(d) != 0)
        return -1;
    const unsigned base = PORT_BASE(g_disk[d].port);
    hba_set(base + PORT_IS, hba(base + PORT_IS));

    volatile unsigned* header = (volatile unsigned*)g_disk[d].cmd_list;
    header[0] = 5u | (1u << 16);
    header[1] = 0;
    header[2] = (unsigned)(g_cmd_table_phys & 0xFFFFFFFFu);
    header[3] = (unsigned)(g_cmd_table_phys >> 32);
    header[4] = header[5] = header[6] = header[7] = 0;

    memset((void*)g_cmd_table, 0, 256);
    volatile unsigned char* fis = g_cmd_table;
    fis[0] = 0x27;
    fis[1] = 0x80;
    fis[2] = 0xEC;                      /* IDENTIFY DEVICE */
    fis[7] = 0;                         /* not an LBA command */

    volatile unsigned* prdt = (volatile unsigned*)(g_cmd_table + 0x80);
    prdt[0] = (unsigned)(g_buffer_phys & 0xFFFFFFFFu);
    prdt[1] = (unsigned)(g_buffer_phys >> 32);
    prdt[2] = 0;
    prdt[3] = 511u;

    hba_set(base + PORT_CI, 1u);
    for (int spin = 0; spin < 5000000; ++spin) {
        if ((hba(base + PORT_CI) & 1u) == 0)
            break;
        if ((hba(base + PORT_IS) & IS_TFES) != 0)
            return -1;
    }
    if ((hba(base + PORT_CI) & 1u) != 0)
        return -1;

    /* Words 100 to 103: the 48-bit count, which is the one to trust on any
     * disk large enough for the 28-bit one to have been rounded down. */
    const volatile unsigned char* id = g_buffer;
    unsigned long sectors = 0;
    for (int i = 0; i < 8; ++i)
        sectors |= (unsigned long)id[200 + i] << (8 * i);
    if (sectors == 0)
        for (int i = 0; i < 4; ++i)     /* fall back to the 28-bit count */
            sectors |= (unsigned long)id[120 + i] << (8 * i);
    g_disk[d].sectors = sectors;
    return sectors != 0 ? 0 : -1;
}

int main(void)
{
    if (io_permit(PCI_ADDRESS, 8) != 0) {
        printf("ahcid: refused the configuration ports\n");
        return 1;
    }

    /* Every function of every slot on the first bus. QEMU puts everything
     * there and walking further would only be thorough about a machine this
     * does not run on. */
    unsigned bar5 = 0;
    unsigned at_bus = 0, at_slot = 0, at_fn = 0;
    unsigned bus = 0, slot = 0, fn = 0;
    int found = 0;
    for (bus = 0; bus < 1 && !found; ++bus)
        for (slot = 0; slot < 32 && !found; ++slot)
            for (fn = 0; fn < 8 && !found; ++fn) {
                const unsigned id = cfg_read(bus, slot, fn, 0x00);
                if ((id & 0xFFFF) == 0xFFFF)
                    continue;
                const unsigned classes = cfg_read(bus, slot, fn, 0x08);
                if (((classes >> 24) & 0xFF) != CLASS_STORAGE ||
                    ((classes >> 16) & 0xFF) != SUBCLASS_SATA ||
                    ((classes >> 8) & 0xFF) != PROGIF_AHCI)
                    continue;
                bar5 = cfg_read(bus, slot, fn, 0x24) & ~0xFu;
                at_bus = bus; at_slot = slot; at_fn = fn;
                found = 1;
            }

    if (!found || bar5 == 0) {
        printf("ahcid: no AHCI controller\n");
        return 1;
    }

    /* Bus mastering, or the controller cannot fetch its own command lists -
     * which matters for the half of this that is not written yet, and costs
     * nothing to do now. */
    const unsigned command = cfg_read(at_bus, at_slot, at_fn, 0x04);
    cfg_write(at_bus, at_slot, at_fn, 0x04, command | 0x6u);

    g_hba = (volatile unsigned*)map_physical(bar5, 0x1100);
    if (g_hba == 0) {
        printf("ahcid: cannot map the registers at %08x\n", bar5);
        return 1;
    }

    /* If the firmware still owns the controller, ask for it. On a machine
     * where it never had it this reads as already ours and does nothing. */
    if ((hba(HBA_CAP2) & 0x1u) != 0) {
        hba_set(HBA_BOHC, hba(HBA_BOHC) | 0x2u);
        for (int spin = 0; spin < 100000; ++spin)
            if ((hba(HBA_BOHC) & 0x1u) == 0)
                break;
    }

    /* AHCI mode rather than the legacy emulation some controllers start in. */
    hba_set(HBA_GHC, hba(HBA_GHC) | 0x80000000u);

    const unsigned cap = hba(HBA_CAP);
    const unsigned version = hba(HBA_VS);
    printf("ahcid: AHCI %u.%u at %08x, %u ports, %u commands deep%s\n",
           (version >> 16) & 0xFFFF, (version >> 8) & 0xFF, bar5,
           (cap & 0x1Fu) + 1, ((cap >> 8) & 0x1Fu) + 1,
           (cap & (1u << 31)) ? ", 64-bit" : "");

    const unsigned implemented = hba(HBA_PI);
    for (unsigned port = 0; port < 32; ++port) {
        if ((implemented & (1u << port)) == 0)
            continue;
        const unsigned ssts = hba(PORT_BASE(port) + PORT_SSTS);
        const unsigned det = ssts & 0xFu, ipm = (ssts >> 8) & 0xFu;
        if (det != 3 || ipm != 1)
            continue;                   /* nothing there, or not awake */
        const unsigned sig = hba(PORT_BASE(port) + PORT_SIG);
        printf("ahcid: port %u, %s\n", port,
               sig == SIG_ATA ? "SATA disk"
             : sig == SIG_ATAPI ? "SATA optical drive"
                                : "something that is not a disk");
    }

    /* Every port with a disk on it, up to as many as this can hold. */
    for (unsigned port = 0; port < 32 && g_disks < MAX_DISKS; ++port) {
        if ((implemented & (1u << port)) == 0)
            continue;
        const unsigned ssts = hba(PORT_BASE(port) + PORT_SSTS);
        if ((ssts & 0xFu) != 3 || ((ssts >> 8) & 0xFu) != 1)
            continue;
        if (hba(PORT_BASE(port) + PORT_SIG) != SIG_ATA)
            continue;
        g_disk[g_disks++].port = port;
    }
    if (g_disks == 0) {
        printf("ahcid: no disk to drive\n");
        return 1;
    }

    /* One allocation, carved into pages. Each structure has an alignment the
     * controller insists on - a kilobyte for a command list, 256 bytes for a
     * reply area, 128 for a command table - and a page satisfies all of them,
     * so the pieces are put on page boundaries rather than arithmetic being
     * done to prove each one. */
    /* Two pages per disk for its command list and reply area, one for the
     * shared command table, and however many the data buffer needs. */
    const unsigned long buffer_pages = (BUFFER_BYTES + 4095ul) / 4096ul;
    const unsigned long want =
        ((unsigned long)g_disks * 2ul + 1ul + buffer_pages) * 4096ul;
    uint64_t phys = 0;
    unsigned char* dma = (unsigned char*)dma_alloc(want, &phys);
    if (dma == 0) {
        printf("ahcid: no memory the controller can reach\n");
        return 1;
    }
    memset(dma, 0, want);

    unsigned long at = 0;
    for (unsigned d = 0; d < g_disks; ++d) {
        g_disk[d].cmd_list      = dma + at;
        g_disk[d].cmd_list_phys = phys + at;
        at += 4096;
        g_disk[d].fis      = dma + at;
        g_disk[d].fis_phys = phys + at;
        at += 4096;
    }
    g_cmd_table = dma + at;  g_cmd_table_phys = phys + at;  at += 4096;
    g_buffer    = dma + at;  g_buffer_phys    = phys + at;

    /* Start each port and ask each disk how big it is. A port that will not
     * start, or a disk that will not answer, is dropped rather than allowed to
     * shift the numbering of the ones that did - which is why this compacts
     * the array instead of leaving a hole in it. */
    unsigned kept = 0;
    for (unsigned d = 0; d < g_disks; ++d) {
        if (d != kept)
            g_disk[kept] = g_disk[d];
        if (port_start(kept) != 0) {
            printf("ahcid: port %u will not start\n", g_disk[kept].port);
            continue;
        }
        if (identify(kept) != 0) {
            printf("ahcid: port %u would not say how big it is\n",
                   g_disk[kept].port);
            continue;
        }
        ++kept;
    }
    g_disks = kept;
    if (g_disks == 0) {
        printf("ahcid: no disk came up\n");
        return 1;
    }

    /* A read, and only a read.
     *
     * This used to write a pattern to a spare sector and read it back, which
     * was a fair test of both directions while the disk was a blank one kept
     * for the purpose. The disk is the root filesystem now, and a driver that
     * writes to a sector of its own choosing at every boot is a driver that
     * destroys whatever is at that sector - which is exactly what happened:
     * the volume mounted, and then nothing on it worked.
     *
     * The write path is still exercised, by everything the system does to its
     * own root. It does not need a rehearsal that damages the stage. */
    for (unsigned d = 0; d < g_disks; ++d) {
        memset((void*)g_buffer, 0, 512);
        const int ok = transfer(d, 0, 1, 0) == 0;
        printf("ahcid: disk %u on port %u, %lu sectors, DMA read %s\n",
               BLK_AHCI_BASE + d, g_disk[d].port, g_disk[d].sectors,
               ok ? "verified" : "FAILED");
        if (!ok)
            return 1;
    }

    /* Now serve. The protocol is the one the other driver speaks, on a port of
     * its own: a client that wants this disk asks here, and the disk index it
     * uses is the one this driver knows rather than the one the system does -
     * the subtraction is the caller's, because the caller is the only one that
     * knows about both. */
    const int shm = shm_open(BLK_SHM_KEY2, sizeof(struct blk_shared),
                             SHM_PUBLIC);
    struct blk_shared* shared = shm < 0 ? 0 : (struct blk_shared*)shm_map(shm);
    if (shared == 0) {
        printf("ahcid: cannot publish the transfer buffer\n");
        return 1;
    }

    const int port = port_create(IPC_PORT_BLOCK2);
    if (port < 0) {
        printf("ahcid: a driver already has this port\n");
        return 1;
    }
    printf("ahcid: serving %u disk%s from %u on port %d\n", g_disks,
           g_disks == 1 ? "" : "s", BLK_AHCI_BASE, IPC_PORT_BLOCK2);

    for (;;) {
        struct ipc_message m, r;
        unsigned from = 0;
        const int handle = ipc_recv(port, &m, &from);
        if (handle < 0)
            continue;

        memset(&r, 0, sizeof(r));
        r.tag = m.tag;
        r.word[0] = -1;

        /* Which of this driver's disks, and which region of the shared buffer
         * the bytes travel in. */
        const unsigned d = (unsigned)m.word[2];
        const unsigned slot = (unsigned)m.word[3] < BLK_SLOTS
                            ? (unsigned)m.word[3] : 0u;
        if (d >= g_disks) {
            ipc_reply(handle, &r);
            continue;
        }

        if (m.tag == BLK_INFO) {
            r.word[0] = (long)g_disk[d].sectors;
            r.word[1] = BLK_SECTOR;
            unsigned n = 0;
            while (n < sizeof(r.data) - 1 && "AHCI DISK"[n] != '\0') {
                r.data[n] = (unsigned char)"AHCI DISK"[n];
                ++n;
            }
            r.bytes = n;
        } else if (m.tag == BLK_READ || m.tag == BLK_WRITE) {
            const unsigned long lba = (unsigned long)m.word[0];
            unsigned count = (unsigned)m.word[1];
            if (count == 0 || count > BLK_MAX_COUNT ||
                count * BLK_SECTOR > BUFFER_BYTES) {
                ipc_reply(handle, &r);
                continue;
            }
            if (m.tag == BLK_WRITE)
                memcpy((void*)g_buffer, shared->data[slot], count * BLK_SECTOR);
            if (transfer(d, lba, count, m.tag == BLK_WRITE) == 0) {
                if (m.tag == BLK_READ)
                    memcpy(shared->data[slot], (const void*)g_buffer,
                           count * BLK_SECTOR);
                r.word[0] = 0;
            }
        }
        ipc_reply(handle, &r);
    }
}
