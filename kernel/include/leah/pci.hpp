#pragma once

#include <leah/types.hpp>

// PCI configuration space over the legacy 0xCF8/0xCFC port pair.
//
// Enumeration has to happen before any storage or USB work: those controllers
// are found here, and their MMIO base addresses come out of the BARs below.

namespace pci {

struct Device {
    u8  bus;
    u8  slot;
    u8  function;

    u16 vendor_id;
    u16 device_id;

    u8  class_code;
    u8  subclass;
    u8  prog_if;
    u8  revision;
    u8  header_type;

    u8  interrupt_line;
};

u8  read8 (u8 bus, u8 slot, u8 function, u8 offset);
u16 read16(u8 bus, u8 slot, u8 function, u8 offset);
u32 read32(u8 bus, u8 slot, u8 function, u8 offset);
void write32(u8 bus, u8 slot, u8 function, u8 offset, u32 value);

// Base Address Register, masked to the actual address. is_io says whether it
// describes a port range rather than memory.
u64 bar_address(const Device& device, u8 index, bool& is_io);

void enumerate();

usize device_count();
const Device& device_at(usize index);
const Device* find(u8 class_code, u8 subclass);

const char* class_name(u8 class_code, u8 subclass);

} // namespace pci
