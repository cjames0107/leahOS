#pragma once

#include <leah/types.hpp>

// A sector-addressed device, and the partition view that sits on top of one.
//
// The filesystem code talks to this rather than to ata:: directly, so AHCI,
// NVMe or a ramdisk can be dropped in later without the FAT32 driver noticing.

namespace block {

constexpr usize kSectorSize = 512;

class Device {
public:
    virtual ~Device() = default;

    virtual bool read(u64 lba, u32 count, void* buffer) = 0;
    virtual bool write(u64 lba, u32 count, const void* buffer) = 0;
    virtual u64 sector_count() const = 0;
    virtual const char* name() const = 0;
};

// Wraps one of the drives ata::init() found.
class AtaDevice final : public Device {
public:
    explicit AtaDevice(usize drive_index);

    bool read(u64 lba, u32 count, void* buffer) override;
    bool write(u64 lba, u32 count, const void* buffer) override;
    u64 sector_count() const override;
    const char* name() const override { return "ata"; }

private:
    usize m_index;
};

// A window onto a parent device. Every access is bounds-checked and rebased,
// so a filesystem bug cannot reach outside its own partition - notably not
// into the kernel image sitting earlier on the same disk.
class Partition final : public Device {
public:
    Partition(Device* parent, u64 start_lba, u64 sectors);

    bool read(u64 lba, u32 count, void* buffer) override;
    bool write(u64 lba, u32 count, const void* buffer) override;
    u64 sector_count() const override { return m_sectors; }
    const char* name() const override { return "partition"; }

    u64 start_lba() const { return m_start; }

private:
    Device* m_parent;
    u64 m_start;
    u64 m_sectors;
};

// One entry of an MBR partition table.
struct PartitionInfo {
    u8  type;
    bool bootable;
    u64 start_lba;
    u64 sectors;
};

// Reads LBA 0 and decodes the four primary entries. Returns how many are in
// use; extended partitions are not followed.
usize scan_partitions(Device& device, PartitionInfo* out, usize max);

const char* partition_type_name(u8 type);

} // namespace block
