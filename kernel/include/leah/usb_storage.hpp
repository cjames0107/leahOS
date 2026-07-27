#pragma once

#include <leah/blockdev.hpp>
#include <leah/types.hpp>

// USB mass storage, the bulk-only transport.
//
// The protocol is deliberately thin: wrap a SCSI command in a 31-byte Command
// Block Wrapper, push it down the bulk OUT endpoint, move the data, then read a
// 13-byte Command Status Wrapper back from the bulk IN endpoint. Every USB stick
// and external disk speaks it, and the commands inside are ordinary SCSI - the
// same INQUIRY and READ(10) a SATA disk would answer.

namespace usb::storage {

constexpr usize kSectorSize = 512;

// Claim any mass-storage device the xHCI driver enumerated. Returns how many
// were brought up.
usize init();

usize drive_count();
u64 sector_count(usize drive);
const char* model(usize drive);

bool read(usize drive, u64 lba, u32 count, void* buffer);
bool write(usize drive, u64 lba, u32 count, const void* buffer);

// The block-layer view, so a filesystem can be mounted on a USB disk.
class Device final : public block::Device {
public:
    explicit Device(usize drive_index);

    bool read(u64 lba, u32 count, void* buffer) override;
    bool write(u64 lba, u32 count, const void* buffer) override;
    u64 sector_count() const override;
    const char* name() const override { return "usb"; }

private:
    usize m_index;
};

} // namespace usb::storage
