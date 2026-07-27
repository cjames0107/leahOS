#include <leah/string.hpp>
#include <leah/usb_storage.hpp>
#include <leah/xhci.hpp>

namespace usb::storage {
namespace {

// The Command Block Wrapper and Command Status Wrapper that bracket every
// transfer. Both signatures are ASCII on the wire ("USBC" and "USBS"), which
// makes a desynchronised bulk pipe obvious in a trace.
constexpr u32 kCbwSignature = 0x43425355;
constexpr u32 kCswSignature = 0x53425355;

struct [[gnu::packed]] Cbw {
    u32 signature;
    u32 tag;
    u32 transfer_length;
    u8  flags;              // 0x80 for a device-to-host transfer
    u8  lun;
    u8  command_length;
    u8  command[16];
};

struct [[gnu::packed]] Csw {
    u32 signature;
    u32 tag;
    u32 residue;
    u8  status;             // 0 passed, 1 failed, 2 phase error
};

constexpr u8 kScsiTestUnitReady = 0x00;
constexpr u8 kScsiInquiry       = 0x12;
constexpr u8 kScsiReadCapacity  = 0x25;
constexpr u8 kScsiRead10        = 0x28;
constexpr u8 kScsiWrite10       = 0x2A;

struct Drive {
    u8   slot;
    u8   bulk_in;
    u8   bulk_out;
    u64  sectors;
    u32  block_size;
    char model[33];
};

constexpr usize kMaxDrives = 4;
Drive g_drives[kMaxDrives];
usize g_drive_count = 0;
u32   g_tag = 1;

// Big-endian, because SCSI predates the argument and never joined it.
void put_be32(u8* out, u32 value)
{
    out[0] = static_cast<u8>(value >> 24);
    out[1] = static_cast<u8>(value >> 16);
    out[2] = static_cast<u8>(value >> 8);
    out[3] = static_cast<u8>(value);
}
u32 get_be32(const u8* in)
{
    return static_cast<u32>(in[0]) << 24 | static_cast<u32>(in[1]) << 16 |
           static_cast<u32>(in[2]) << 8 | static_cast<u32>(in[3]);
}

// One command, start to finish: wrapper out, data either way, status back.
bool run_scsi(Drive& drive, const u8* command, u8 command_length,
              void* data, u32 data_length, bool device_to_host)
{
    Cbw cbw{};
    cbw.signature       = kCbwSignature;
    cbw.tag             = g_tag++;
    cbw.transfer_length = data_length;
    cbw.flags           = device_to_host ? 0x80 : 0x00;
    cbw.lun             = 0;
    cbw.command_length  = command_length;
    memcpy(cbw.command, command, command_length);

    if (xhci::transfer(drive.slot, drive.bulk_out, &cbw, sizeof(cbw)) !=
        static_cast<i64>(sizeof(cbw)))
        return false;

    if (data_length > 0) {
        const u8 endpoint = device_to_host ? drive.bulk_in : drive.bulk_out;
        if (xhci::transfer(drive.slot, endpoint, data, data_length) < 0)
            return false;
    }

    Csw csw{};
    if (xhci::transfer(drive.slot, drive.bulk_in, &csw, sizeof(csw)) !=
        static_cast<i64>(sizeof(csw)))
        return false;

    // A wrong signature means the pipe has lost sync, which is worth
    // distinguishing from the device simply reporting a failure.
    if (csw.signature != kCswSignature || csw.tag != cbw.tag)
        return false;
    return csw.status == 0;
}

bool inquiry(Drive& drive)
{
    u8 command[6] = { kScsiInquiry, 0, 0, 0, 36, 0 };
    u8 response[36] = {};
    if (!run_scsi(drive, command, sizeof(command), response, sizeof(response), true))
        return false;

    // Bytes 8-15 are the vendor, 16-31 the product; both space padded.
    usize n = 0;
    for (usize i = 8; i < 32 && n < sizeof(drive.model) - 1; ++i)
        drive.model[n++] = static_cast<char>(response[i]);
    drive.model[n] = '\0';
    for (int i = static_cast<int>(n) - 1; i >= 0 && drive.model[i] == ' '; --i)
        drive.model[i] = '\0';
    return true;
}

bool read_capacity(Drive& drive)
{
    u8 command[10] = { kScsiReadCapacity, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    u8 response[8] = {};
    if (!run_scsi(drive, command, sizeof(command), response, sizeof(response), true))
        return false;

    // The reply gives the *last* addressable block, not the count.
    drive.sectors    = static_cast<u64>(get_be32(response)) + 1;
    drive.block_size = get_be32(response + 4);
    return drive.block_size == kSectorSize && drive.sectors > 0;
}

bool transfer_blocks(usize index, u64 lba, u32 count, void* buffer, bool read_op)
{
    if (index >= g_drive_count || count == 0)
        return false;
    Drive& drive = g_drives[index];

    auto* bytes = static_cast<u8*>(buffer);
    while (count > 0) {
        // Bounded per command so one transfer stays a sane size; READ(10) could
        // carry far more, but nothing here asks for it.
        const u32 chunk = count > 64 ? 64u : count;

        u8 command[10] = {};
        command[0] = read_op ? kScsiRead10 : kScsiWrite10;
        put_be32(command + 2, static_cast<u32>(lba));
        command[7] = static_cast<u8>(chunk >> 8);
        command[8] = static_cast<u8>(chunk);

        if (!run_scsi(drive, command, sizeof(command), bytes,
                      chunk * kSectorSize, read_op))
            return false;

        bytes += chunk * kSectorSize;
        lba   += chunk;
        count -= chunk;
    }
    return true;
}

} // namespace

usize init()
{
    for (usize i = 0; i < xhci::device_count() && g_drive_count < kMaxDrives; ++i) {
        const xhci::Device& device = xhci::device_at(i);
        // Class 8 is mass storage; protocol 0x50 is the bulk-only transport,
        // which is the only one worth implementing.
        if (device.device_class != 0x08 || device.device_protocol != 0x50)
            continue;
        if (device.bulk_in == 0 || device.bulk_out == 0)
            continue;

        Drive& drive = g_drives[g_drive_count];
        drive.slot     = device.slot;
        drive.bulk_in  = device.bulk_in;
        drive.bulk_out = device.bulk_out;

        // A freshly attached device answers the first command with a unit
        // attention; asking twice gets past it.
        u8 ready[6] = { kScsiTestUnitReady, 0, 0, 0, 0, 0 };
        run_scsi(drive, ready, sizeof(ready), nullptr, 0, false);
        run_scsi(drive, ready, sizeof(ready), nullptr, 0, false);

        if (!inquiry(drive) || !read_capacity(drive))
            continue;
        ++g_drive_count;
    }
    return g_drive_count;
}

usize drive_count() { return g_drive_count; }

u64 sector_count(usize drive)
{
    return drive < g_drive_count ? g_drives[drive].sectors : 0;
}

const char* model(usize drive)
{
    return drive < g_drive_count ? g_drives[drive].model : "";
}

bool read(usize drive, u64 lba, u32 count, void* buffer)
{
    return transfer_blocks(drive, lba, count, buffer, true);
}

bool write(usize drive, u64 lba, u32 count, const void* buffer)
{
    return transfer_blocks(drive, lba, count, const_cast<void*>(buffer), false);
}

Device::Device(usize drive_index) : m_index(drive_index) {}

bool Device::read(u64 lba, u32 count, void* buffer)
{
    return usb::storage::read(m_index, lba, count, buffer);
}

bool Device::write(u64 lba, u32 count, const void* buffer)
{
    return usb::storage::write(m_index, lba, count, buffer);
}

u64 Device::sector_count() const { return usb::storage::sector_count(m_index); }

} // namespace usb::storage
