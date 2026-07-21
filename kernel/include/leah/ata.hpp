#pragma once

#include <leah/types.hpp>

// ATA/IDE disks over programmed I/O.
//
// PIO moves every byte through the CPU, so it is slow and will eventually be
// replaced by AHCI with DMA. It is also the only interface that needs no
// memory mapping, no interrupt plumbing and no controller-specific setup -
// which makes it the right thing to read a filesystem with first.

namespace ata {

constexpr usize kSectorSize = 512;
constexpr usize kMaxDrives  = 4;        // two channels, master and slave

struct Drive {
    bool present;
    bool lba48;
    u64  sectors;
    char model[41];
    char serial[21];
};

void init();

usize drive_count();
const Drive& drive_at(usize index);
u64 capacity_bytes(usize index);

// count is in sectors; buffer must hold count * kSectorSize bytes.
bool read(usize index, u64 lba, u32 count, void* buffer);
bool write(usize index, u64 lba, u32 count, const void* buffer);

} // namespace ata
