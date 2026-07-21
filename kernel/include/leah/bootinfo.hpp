#pragma once

#include <leah/types.hpp>

// What stage 2 leaves behind for the kernel. The layout here must match
// boot/layout.inc exactly - there is no negotiation, just an agreed address.

namespace boot {

enum class RegionType : u32 {
    Usable          = 1,
    Reserved        = 2,
    AcpiReclaimable = 3,
    AcpiNvs         = 4,
    Bad             = 5,
};

const char* region_type_name(RegionType type);

struct [[gnu::packed]] MemoryRegion {
    u64 base;
    u64 length;
    RegionType type;
    u32 acpi_attributes;
};

static_assert(sizeof(MemoryRegion) == 24, "E820 entries are 24 bytes");

// Written by stage 2 at E820_COUNT: a count, twelve bytes of slack so the
// array starts on a tidy 16-byte boundary, then the entries themselves.
struct [[gnu::packed]] MemoryMap {
    u32 count;
    u8  reserved[12];
    MemoryRegion regions[];
};

} // namespace boot
