#include <leah/ata.hpp>
#include <leah/io.hpp>
#include <leah/spinlock.hpp>
#include <leah/string.hpp>

namespace ata {
namespace {

// Register offsets from a channel's I/O base.
constexpr u16 kRegData       = 0;
constexpr u16 kRegError      = 1;   // read
constexpr u16 kRegFeatures   = 1;   // write
constexpr u16 kRegSectorCount = 2;
constexpr u16 kRegLbaLow     = 3;
constexpr u16 kRegLbaMid     = 4;
constexpr u16 kRegLbaHigh    = 5;
constexpr u16 kRegDrive      = 6;
constexpr u16 kRegStatus     = 7;   // read
constexpr u16 kRegCommand    = 7;   // write

// Status bits
constexpr u8 kStatusError = 1 << 0;
constexpr u8 kStatusDrq   = 1 << 3;
constexpr u8 kStatusFault = 1 << 5;
constexpr u8 kStatusReady = 1 << 6;
constexpr u8 kStatusBusy  = 1 << 7;

// Commands
constexpr u8 kCmdReadPio     = 0x20;
constexpr u8 kCmdReadPioExt  = 0x24;
constexpr u8 kCmdWritePio    = 0x30;
constexpr u8 kCmdWritePioExt = 0x34;
constexpr u8 kCmdFlush       = 0xE7;
constexpr u8 kCmdFlushExt    = 0xEA;
constexpr u8 kCmdIdentify    = 0xEC;

constexpr u64 kLba28Limit = 1ull << 28;

struct Channel {
    u16 io;
    u16 control;
};

constexpr Channel kChannels[2] = {
    { 0x1F0, 0x3F6 },   // primary
    { 0x170, 0x376 },   // secondary
};

struct DriveInfo {
    Drive info;
    u8    channel;
    bool  slave;
};

DriveInfo g_drives[kMaxDrives]{};
usize g_count = 0;

// One lock per channel, held across the whole command.
//
// A channel has one set of registers, so a transfer is not a sequence of
// independent steps - it is one conversation, and a second command issued
// part-way through ends the first. Nothing enforced that: the caller selected a
// drive, issued READ, and then spun reading sectors, and if it was preempted in
// the middle (the kernel lock is handed on when that happens) another task could
// walk straight into the same registers. Both then got someone else's sectors,
// or nothing at all.
//
// Interrupts are masked while it is held for the reason the header gives: a
// holder that is preempted leaves anyone spinning with interrupts already off
// waiting forever. Both wait loops here are bounded, so a dead drive costs a
// bounded stall rather than the machine.
sync::Spinlock g_channel_lock[2];

const Channel& channel_of(const DriveInfo& d) { return kChannels[d.channel]; }

// A status read takes ~100ns to settle, and the spec wants 400ns before the
// value means anything. Four reads of the alternate status register is the
// canonical way to spend that time without side effects - unlike the primary
// status register, reading this one does not acknowledge an interrupt.
void settle(const Channel& channel)
{
    for (int i = 0; i < 4; ++i)
        (void)io::in8(channel.control);
}

void select(const DriveInfo& d, u8 mode, u8 lba_high_nibble = 0)
{
    const Channel& channel = channel_of(d);
    io::out8(channel.io + kRegDrive,
             static_cast<u8>(mode | (d.slave ? 0x10 : 0x00) | (lba_high_nibble & 0x0F)));
    settle(channel);
}

// Spin until the drive is no longer busy. Bounded, because a missing or wedged
// drive would otherwise hang the kernel at boot.
bool wait_not_busy(const Channel& channel)
{
    for (u32 i = 0; i < 1000000; ++i) {
        if ((io::in8(channel.io + kRegStatus) & kStatusBusy) == 0)
            return true;
    }
    return false;
}

bool wait_for_data(const Channel& channel)
{
    for (u32 i = 0; i < 1000000; ++i) {
        const u8 status = io::in8(channel.io + kRegStatus);
        if ((status & kStatusBusy) != 0)
            continue;
        if ((status & (kStatusError | kStatusFault)) != 0)
            return false;
        if ((status & kStatusDrq) != 0)
            return true;
    }
    return false;
}

// IDENTIFY returns model and serial strings as 16-bit words with the two bytes
// swapped, a leftover from the days of big-endian ATA controllers.
void copy_ata_string(char* dest, const u16* words, usize word_count)
{
    for (usize i = 0; i < word_count; ++i) {
        dest[i * 2]     = static_cast<char>(words[i] >> 8);
        dest[i * 2 + 1] = static_cast<char>(words[i] & 0xFF);
    }
    dest[word_count * 2] = '\0';

    for (isize i = static_cast<isize>(word_count * 2) - 1; i >= 0; --i) {
        if (dest[i] != ' ')
            break;
        dest[i] = '\0';
    }
}

bool identify(u8 channel_index, bool slave, Drive& out)
{
    const Channel& channel = kChannels[channel_index];

    DriveInfo probe{};
    probe.channel = channel_index;
    probe.slave   = slave;

    select(probe, 0xA0);

    io::out8(channel.io + kRegSectorCount, 0);
    io::out8(channel.io + kRegLbaLow, 0);
    io::out8(channel.io + kRegLbaMid, 0);
    io::out8(channel.io + kRegLbaHigh, 0);
    io::out8(channel.io + kRegCommand, kCmdIdentify);

    // An all-zero status means the channel is floating - no device at all.
    if (io::in8(channel.io + kRegStatus) == 0)
        return false;

    if (!wait_not_busy(channel))
        return false;

    // A device that puts a signature in these registers is ATAPI or SATA
    // pretending to be neither; IDENTIFY does not apply to it.
    if (io::in8(channel.io + kRegLbaMid) != 0 || io::in8(channel.io + kRegLbaHigh) != 0)
        return false;

    if (!wait_for_data(channel))
        return false;

    u16 id[256];
    for (usize i = 0; i < 256; ++i)
        id[i] = io::in16(channel.io + kRegData);

    out.present = true;
    out.lba48   = (id[83] & (1 << 10)) != 0;

    const u64 lba28 = static_cast<u64>(id[60]) | static_cast<u64>(id[61]) << 16;
    const u64 lba48 = static_cast<u64>(id[100])
                    | static_cast<u64>(id[101]) << 16
                    | static_cast<u64>(id[102]) << 32
                    | static_cast<u64>(id[103]) << 48;

    out.sectors = out.lba48 && lba48 != 0 ? lba48 : lba28;

    copy_ata_string(out.serial, &id[10], 10);
    copy_ata_string(out.model, &id[27], 20);
    return true;
}

// Set up the address registers and issue a read or write. LBA48 doubles each
// register by writing the high byte first and the low byte second - the drive
// keeps a one-deep FIFO per register.
bool begin_transfer(const DriveInfo& d, u64 lba, u32 count, bool writing)
{
    const Channel& channel = channel_of(d);
    const bool ext = d.info.lba48 && (lba + count > kLba28Limit || count > 256);

    if (!wait_not_busy(channel))
        return false;

    if (ext) {
        select(d, 0x40);
        io::out8(channel.io + kRegSectorCount, static_cast<u8>(count >> 8));
        io::out8(channel.io + kRegLbaLow,  static_cast<u8>(lba >> 24));
        io::out8(channel.io + kRegLbaMid,  static_cast<u8>(lba >> 32));
        io::out8(channel.io + kRegLbaHigh, static_cast<u8>(lba >> 40));
        io::out8(channel.io + kRegSectorCount, static_cast<u8>(count));
        io::out8(channel.io + kRegLbaLow,  static_cast<u8>(lba));
        io::out8(channel.io + kRegLbaMid,  static_cast<u8>(lba >> 8));
        io::out8(channel.io + kRegLbaHigh, static_cast<u8>(lba >> 16));
        io::out8(channel.io + kRegCommand, writing ? kCmdWritePioExt : kCmdReadPioExt);
    } else {
        if (lba + count > kLba28Limit)
            return false;
        // LBA28 smuggles the top four address bits into the drive register.
        select(d, 0xE0, static_cast<u8>(lba >> 24));
        io::out8(channel.io + kRegSectorCount, static_cast<u8>(count));
        io::out8(channel.io + kRegLbaLow,  static_cast<u8>(lba));
        io::out8(channel.io + kRegLbaMid,  static_cast<u8>(lba >> 8));
        io::out8(channel.io + kRegLbaHigh, static_cast<u8>(lba >> 16));
        io::out8(channel.io + kRegCommand, writing ? kCmdWritePio : kCmdReadPio);
    }
    return true;
}

} // namespace

void init()
{
    g_count = 0;

    for (u8 c = 0; c < 2; ++c) {
        // nIEN: we poll, so tell the drive not to raise interrupts. Leaving
        // them on would deliver IRQ 14/15 to a vector with no handler.
        io::out8(kChannels[c].control, 0x02);

        for (u8 s = 0; s < 2; ++s) {
            Drive drive{};
            if (!identify(c, s != 0, drive))
                continue;
            if (g_count >= kMaxDrives)
                return;

            g_drives[g_count].info    = drive;
            g_drives[g_count].channel = c;
            g_drives[g_count].slave   = s != 0;
            ++g_count;
        }
    }
}

usize drive_count() { return g_count; }

const Drive& drive_at(usize index) { return g_drives[index].info; }

u64 capacity_bytes(usize index)
{
    return g_drives[index].info.sectors * kSectorSize;
}

bool read(usize index, u64 lba, u32 count, void* buffer)
{
    if (index >= g_count || count == 0)
        return false;

    const DriveInfo& d = g_drives[index];
    const Channel& channel = channel_of(d);
    sync::IrqScopedLock guard(g_channel_lock[d.channel]);

    if (!begin_transfer(d, lba, count, false))
        return false;

    auto* out = static_cast<u16*>(buffer);
    for (u32 sector = 0; sector < count; ++sector) {
        if (!wait_for_data(channel))
            return false;
        for (usize word = 0; word < kSectorSize / 2; ++word)
            *out++ = io::in16(channel.io + kRegData);
    }
    return true;
}

bool write(usize index, u64 lba, u32 count, const void* buffer)
{
    if (index >= g_count || count == 0)
        return false;

    const DriveInfo& d = g_drives[index];
    const Channel& channel = channel_of(d);
    sync::IrqScopedLock guard(g_channel_lock[d.channel]);

    if (!begin_transfer(d, lba, count, true))
        return false;

    const auto* in = static_cast<const u16*>(buffer);
    for (u32 sector = 0; sector < count; ++sector) {
        if (!wait_for_data(channel))
            return false;
        for (usize word = 0; word < kSectorSize / 2; ++word)
            io::out16(channel.io + kRegData, *in++);
    }

    // Without an explicit flush the data can sit in the drive's write cache,
    // and a reset or power loss would lose an acknowledged write.
    const bool ext = d.info.lba48 && (lba + count > kLba28Limit || count > 256);
    io::out8(channel.io + kRegCommand, ext ? kCmdFlushExt : kCmdFlush);
    return wait_not_busy(channel);
}

} // namespace ata
