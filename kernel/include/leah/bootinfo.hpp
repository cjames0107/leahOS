#pragma once

#include <leah/types.hpp>

// What stage 2 leaves behind for the kernel. Field offsets are mirrored by the
// BI_* constants in boot/layout.inc; the two must be changed together.

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

struct [[gnu::packed]] Info {
    u32 e820_count;         // 0x00
    u32 reserved0;          // 0x04
    u64 framebuffer;        // 0x08  physical, 0 when no VBE mode was set
    u32 pitch;              // 0x10  bytes per scanline, not pixels
    u32 width;              // 0x14
    u32 height;             // 0x18
    u8  bits_per_pixel;     // 0x1C
    u8  reserved1[3];       // 0x1D
    u32 font;               // 0x20  physical, 256 glyphs of 8x16
};

static_assert(sizeof(Info) == 0x24);

// The entries live in their own page so the header above can grow without
// colliding with them.
constexpr paddr_t kMemoryMapAddress = 0x21000;

inline const MemoryRegion* memory_map()
{
    return reinterpret_cast<const MemoryRegion*>(kMemoryMapAddress);
}

} // namespace boot
