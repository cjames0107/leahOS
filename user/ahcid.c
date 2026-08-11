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

#include <driver.h>
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
#define PORT_SSTS     0x28  /* device detection and speed          */
#define PORT_SIG      0x24  /* what kind of device it is           */

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

static unsigned hba(unsigned off)
{
    return g_hba[off / 4];
}

static void hba_set(unsigned off, unsigned value)
{
    g_hba[off / 4] = value;
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

    /* Nothing to serve yet, so nothing to wait for. */
    return 0;
}
