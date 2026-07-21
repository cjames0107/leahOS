#include <leah/io.hpp>
#include <leah/pci.hpp>

namespace pci {
namespace {

constexpr u16 kConfigAddress = 0xCF8;
constexpr u16 kConfigData    = 0xCFC;

constexpr usize kMaxDevices = 64;
Device g_devices[kMaxDevices];
usize  g_count = 0;

u32 address_of(u8 bus, u8 slot, u8 function, u8 offset)
{
    // Bit 31 enables the config cycle; the offset is dword-aligned because the
    // data port is 32 bits wide.
    return 0x80000000u
         | static_cast<u32>(bus) << 16
         | static_cast<u32>(slot & 0x1F) << 11
         | static_cast<u32>(function & 0x07) << 8
         | (offset & 0xFC);
}

void probe_function(u8 bus, u8 slot, u8 function)
{
    const u16 vendor = read16(bus, slot, function, 0x00);
    if (vendor == 0xFFFF)
        return;                     // nothing there

    if (g_count >= kMaxDevices)
        return;

    Device& d = g_devices[g_count++];
    d.bus            = bus;
    d.slot           = slot;
    d.function       = function;
    d.vendor_id      = vendor;
    d.device_id      = read16(bus, slot, function, 0x02);
    d.revision       = read8(bus, slot, function, 0x08);
    d.prog_if        = read8(bus, slot, function, 0x09);
    d.subclass       = read8(bus, slot, function, 0x0A);
    d.class_code     = read8(bus, slot, function, 0x0B);
    d.header_type    = read8(bus, slot, function, 0x0E);
    d.interrupt_line = read8(bus, slot, function, 0x3C);
}

} // namespace

u32 read32(u8 bus, u8 slot, u8 function, u8 offset)
{
    io::out32(kConfigAddress, address_of(bus, slot, function, offset));
    return io::in32(kConfigData);
}

u16 read16(u8 bus, u8 slot, u8 function, u8 offset)
{
    return static_cast<u16>(read32(bus, slot, function, offset) >> ((offset & 2) * 8));
}

u8 read8(u8 bus, u8 slot, u8 function, u8 offset)
{
    return static_cast<u8>(read32(bus, slot, function, offset) >> ((offset & 3) * 8));
}

void write32(u8 bus, u8 slot, u8 function, u8 offset, u32 value)
{
    io::out32(kConfigAddress, address_of(bus, slot, function, offset));
    io::out32(kConfigData, value);
}

u64 bar_address(const Device& device, u8 index, bool& is_io)
{
    const u8 offset = static_cast<u8>(0x10 + index * 4);
    const u32 bar = read32(device.bus, device.slot, device.function, offset);

    is_io = (bar & 1) != 0;
    if (is_io)
        return bar & ~0x3u;

    // A 64-bit BAR (type 0b10) borrows the next slot for its high half.
    const u8 type = static_cast<u8>(bar >> 1 & 0x3);
    const u64 low = bar & ~0xFu;
    if (type == 0x2) {
        const u32 high = read32(device.bus, device.slot, device.function,
                                static_cast<u8>(offset + 4));
        return low | static_cast<u64>(high) << 32;
    }
    return low;
}

void enumerate()
{
    g_count = 0;

    // Brute force over every possible bus. Recursing through bridges is
    // tidier, but 64k config reads cost nothing once and cannot miss a device
    // behind a bridge we failed to notice.
    for (u32 bus = 0; bus < 256; ++bus) {
        for (u8 slot = 0; slot < 32; ++slot) {
            if (read16(static_cast<u8>(bus), slot, 0, 0x00) == 0xFFFF)
                continue;

            probe_function(static_cast<u8>(bus), slot, 0);

            // Bit 7 of the header type says the device has more functions.
            const u8 header = read8(static_cast<u8>(bus), slot, 0, 0x0E);
            if ((header & 0x80) == 0)
                continue;

            for (u8 function = 1; function < 8; ++function)
                probe_function(static_cast<u8>(bus), slot, function);
        }
    }
}

usize device_count() { return g_count; }

const Device& device_at(usize index) { return g_devices[index]; }

const Device* find(u8 class_code, u8 subclass)
{
    for (usize i = 0; i < g_count; ++i) {
        if (g_devices[i].class_code == class_code && g_devices[i].subclass == subclass)
            return &g_devices[i];
    }
    return nullptr;
}

const char* class_name(u8 class_code, u8 subclass)
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

} // namespace pci
