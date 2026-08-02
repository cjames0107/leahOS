/* lspci - what is on the bus.
 *
 * The kernel used to enumerate PCI at boot and print exactly this list, and by
 * the end that was the only thing it did with the results: every driver scans
 * config space for itself, because every driver is a process now and the one
 * thing it must not do is trust a table someone else built about hardware it
 * is about to drive. So the enumeration left with them, and what is left of it
 * is this - a program you run when you want to know, rather than a paragraph
 * printed at every boot whether anyone was reading or not.
 */

#include <driver.h>
#include <stdio.h>

#define PCI_ADDRESS 0xCF8
#define PCI_DATA    0xCFC

static unsigned cfg_read(unsigned bus, unsigned slot, unsigned fn, unsigned off)
{
    outl(PCI_ADDRESS,
         0x80000000u | bus << 16 | slot << 11 | fn << 8 | (off & 0xFC));
    return inl(PCI_DATA);
}

static const char* class_name(unsigned char class_code, unsigned char subclass)
{
    switch (class_code) {
    case 0x00: return "unclassified";
    case 0x01:
        switch (subclass) {
        case 0x01: return "IDE controller";
        case 0x06: return "SATA controller";
        case 0x08: return "NVMe controller";
        default:   return "storage controller";
        }
    case 0x02: return "network controller";
    case 0x03: return "display controller";
    case 0x04: return "multimedia controller";
    case 0x05: return "memory controller";
    case 0x06:
        switch (subclass) {
        case 0x00: return "host bridge";
        case 0x01: return "ISA bridge";
        case 0x04: return "PCI-to-PCI bridge";
        default:   return "bridge";
        }
    case 0x07: return "communication controller";
    case 0x08: return "system peripheral";
    case 0x09: return "input controller";
    case 0x0C:
        switch (subclass) {
        case 0x03: return "USB controller";
        default:   return "serial bus controller";
        }
    default:   return "unknown";
    }
}

int main(void)
{
    unsigned bus, slot, fn, found = 0;

    /* The two config-space ports, and nothing else. A program that can read
     * the bus cannot thereby drive anything on it. */
    if (io_permit(PCI_ADDRESS, 8) != 0) {
        printf("lspci: only root can read config space\n");
        return 1;
    }

    for (bus = 0; bus < 4; ++bus) {
        for (slot = 0; slot < 32; ++slot) {
            /* Function 0 answers for whether the slot exists at all; the
             * multi-function bit in the header type says whether to look at
             * the other seven. */
            const unsigned id0 = cfg_read(bus, slot, 0, 0x00);
            unsigned functions = 1;
            if ((id0 & 0xFFFF) == 0xFFFF)
                continue;
            if ((cfg_read(bus, slot, 0, 0x0C) >> 16 & 0x80) != 0)
                functions = 8;

            for (fn = 0; fn < functions; ++fn) {
                const unsigned id = cfg_read(bus, slot, fn, 0x00);
                unsigned cls;
                if ((id & 0xFFFF) == 0xFFFF)
                    continue;
                cls = cfg_read(bus, slot, fn, 0x08);
                printf("%02x:%02x.%u  %04x:%04x  %s\n", bus, slot, fn,
                       id & 0xFFFF, id >> 16,
                       class_name((unsigned char)(cls >> 24),
                                  (unsigned char)(cls >> 16)));
                ++found;
            }
        }
    }

    if (found == 0)
        printf("lspci: no devices\n");
    return 0;
}
