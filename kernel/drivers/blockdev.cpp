#include <leah/ata.hpp>
#include <leah/blockdev.hpp>
#include <leah/heap.hpp>
#include <leah/string.hpp>

namespace block {

AtaDevice::AtaDevice(usize drive_index) : m_index(drive_index) {}

bool AtaDevice::read(u64 lba, u32 count, void* buffer)
{
    return ata::read(m_index, lba, count, buffer);
}

bool AtaDevice::write(u64 lba, u32 count, const void* buffer)
{
    return ata::write(m_index, lba, count, buffer);
}

u64 AtaDevice::sector_count() const
{
    return ata::drive_at(m_index).sectors;
}

Partition::Partition(Device* parent, u64 start_lba, u64 sectors)
    : m_parent(parent), m_start(start_lba), m_sectors(sectors)
{
}

bool Partition::read(u64 lba, u32 count, void* buffer)
{
    if (lba + count > m_sectors)
        return false;
    return m_parent->read(m_start + lba, count, buffer);
}

bool Partition::write(u64 lba, u32 count, const void* buffer)
{
    if (lba + count > m_sectors)
        return false;
    return m_parent->write(m_start + lba, count, buffer);
}

namespace {

struct [[gnu::packed]] MbrEntry {
    u8  status;
    u8  chs_first[3];
    u8  type;
    u8  chs_last[3];
    u32 lba_first;
    u32 sectors;
};

static_assert(sizeof(MbrEntry) == 16);

} // namespace

usize scan_partitions(Device& device, PartitionInfo* out, usize max)
{
    auto* sector = static_cast<u8*>(kmalloc(kSectorSize));
    if (sector == nullptr)
        return 0;

    usize found = 0;
    if (device.read(0, 1, sector) && sector[510] == 0x55 && sector[511] == 0xAA) {
        for (usize i = 0; i < 4 && found < max; ++i) {
            MbrEntry entry;
            // The table is not naturally aligned inside the sector, so copy it
            // out rather than casting a pointer into the middle of a buffer.
            memcpy(&entry, sector + 446 + i * sizeof(MbrEntry), sizeof(entry));

            if (entry.type == 0x00 || entry.sectors == 0)
                continue;

            out[found].type      = entry.type;
            out[found].bootable  = (entry.status & 0x80) != 0;
            out[found].start_lba = entry.lba_first;
            out[found].sectors   = entry.sectors;
            ++found;
        }
    }

    kfree(sector);
    return found;
}

const char* partition_type_name(u8 type)
{
    switch (type) {
    case 0x01: return "FAT12";
    case 0x04: case 0x06: return "FAT16";
    case 0x05: case 0x0F: return "extended";
    case 0x07: return "NTFS/exFAT";
    case 0x0B: return "FAT32 (CHS)";
    case 0x0C: return "FAT32 (LBA)";
    case 0x82: return "Linux swap";
    case 0x83: return "Linux";
    case 0xEE: return "GPT protective";
    case 0xEF: return "EFI system";
    default:   return "unknown";
    }
}

} // namespace block
