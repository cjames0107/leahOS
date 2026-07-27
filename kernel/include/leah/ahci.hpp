#pragma once

#include <leah/blockdev.hpp>
#include <leah/types.hpp>

// AHCI: SATA the way it was meant to be driven.
//
// The ATA driver moves every sector through the CPU a word at a time - that is
// what PIO means, and it is why reading a megabyte costs a megabyte of IN
// instructions. AHCI instead hands the controller a command list and a scatter/
// gather table in memory and lets it DMA straight to and from RAM; the CPU
// writes one bit to start it and reads one bit to see it finish.

namespace ahci {

constexpr usize kSectorSize = 512;

// Find an AHCI controller on the PCI bus, take ownership of it from the
// firmware, and bring up every port with a SATA disk attached.
bool init();
bool available();

usize drive_count();

// Capacity in 512-byte sectors, and the model string from IDENTIFY.
u64 sector_count(usize drive);
const char* model(usize drive);

bool read(usize drive, u64 lba, u32 count, void* buffer);
bool write(usize drive, u64 lba, u32 count, const void* buffer);

// The block-layer view, so a filesystem can be mounted on an AHCI disk exactly
// as it is on an ATA one.
class Device final : public block::Device {
public:
    explicit Device(usize drive_index);

    bool read(u64 lba, u32 count, void* buffer) override;
    bool write(u64 lba, u32 count, const void* buffer) override;
    u64 sector_count() const override;
    const char* name() const override { return "ahci"; }

private:
    usize m_index;
};

} // namespace ahci
