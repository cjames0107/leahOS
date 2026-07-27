#include <leah/ahci.hpp>
#include <leah/memory.hpp>
#include <leah/pci.hpp>
#include <leah/pmm.hpp>
#include <leah/string.hpp>
#include <leah/vmm.hpp>

namespace ahci {
namespace {

// --- host bus adapter registers ---------------------------------------------
constexpr u32 kHbaCap = 0x00;       // capabilities; low 5 bits are ports - 1
constexpr u32 kHbaGhc = 0x04;       // global host control
constexpr u32 kHbaPi  = 0x0C;       // which port slots are implemented

constexpr u32 kGhcAhciEnable = 1u << 31;
constexpr u32 kGhcReset      = 1u << 0;

// --- per-port registers, at 0x100 + port * 0x80 ------------------------------
constexpr u32 kPortBase   = 0x100;
constexpr u32 kPortStride = 0x80;

constexpr u32 kPortClb   = 0x00;    // command list base (low/high)
constexpr u32 kPortClbu  = 0x04;
constexpr u32 kPortFb    = 0x08;    // FIS receive base
constexpr u32 kPortFbu   = 0x0C;
constexpr u32 kPortIs    = 0x10;    // interrupt status
constexpr u32 kPortIe    = 0x14;
constexpr u32 kPortCmd   = 0x18;
constexpr u32 kPortTfd   = 0x20;    // task file data: the drive's status byte
constexpr u32 kPortSig   = 0x24;    // signature: what kind of device
constexpr u32 kPortSsts  = 0x28;    // SATA status
constexpr u32 kPortSerr  = 0x30;
constexpr u32 kPortCi    = 0x38;    // command issue: one bit per slot

constexpr u32 kCmdStart       = 1u << 0;    // ST
constexpr u32 kCmdFisReceive  = 1u << 4;    // FRE
constexpr u32 kCmdFisRunning  = 1u << 14;   // FR
constexpr u32 kCmdListRunning = 1u << 15;   // CR

constexpr u32 kTfdBusy = 1u << 7;
constexpr u32 kTfdDrq  = 1u << 3;
constexpr u32 kTfdErr  = 1u << 0;

constexpr u32 kSigSata = 0x00000101;        // a plain SATA disk

// --- FIS and command structures ----------------------------------------------
constexpr u8 kFisTypeH2D = 0x27;            // host to device register FIS

constexpr u8 kAtaIdentify   = 0xEC;
constexpr u8 kAtaReadDmaEx  = 0x25;
constexpr u8 kAtaWriteDmaEx = 0x35;

struct [[gnu::packed]] FisH2D {
    u8  type;
    u8  flags;          // bit 7 set means this FIS carries a command
    u8  command;
    u8  feature_low;
    u8  lba0, lba1, lba2, device;
    u8  lba3, lba4, lba5, feature_high;
    u8  count_low, count_high;
    u8  icc, control;
    u8  reserved[4];
};

// One entry per command slot; 32 of them make the command list.
struct [[gnu::packed]] CommandHeader {
    u16 flags;          // low 5 bits: command FIS length in dwords
    u16 prdt_length;    // how many scatter/gather entries follow
    volatile u32 bytes_transferred;
    u64 command_table;  // physical address of the CommandTable below
    u32 reserved[4];
};

struct [[gnu::packed]] PrdtEntry {
    u64 address;        // physical, must be word aligned
    u32 reserved;
    u32 count;          // byte count - 1, bit 31 asks for an interrupt
};

struct [[gnu::packed]] CommandTable {
    u8 command_fis[64];
    u8 atapi_command[16];
    u8 reserved[48];
    PrdtEntry prdt[8];  // 8 * 4 MiB is far more than any request here needs
};

constexpr usize kMaxPorts  = 32;
constexpr usize kMaxDrives = 4;

// A DMA buffer big enough for the largest transfer the block layer asks for.
constexpr usize kTransferSectors = 128;
constexpr usize kTransferBytes   = kTransferSectors * kSectorSize;

struct Drive {
    u32  port;
    u64  sectors;
    char model[41];

    CommandHeader* command_list;    // direct-map pointers to DMA memory
    CommandTable*  command_table;
    u8*            fis;
    u8*            transfer;
    paddr_t command_list_phys;
    paddr_t command_table_phys;
    paddr_t fis_phys;
    paddr_t transfer_phys;
};

volatile u8* g_hba = nullptr;
Drive g_drives[kMaxDrives];
usize g_drive_count = 0;
bool  g_up = false;

u32 hba_read(u32 offset)
{
    return *reinterpret_cast<volatile u32*>(g_hba + offset);
}
void hba_write(u32 offset, u32 value)
{
    *reinterpret_cast<volatile u32*>(g_hba + offset) = value;
}

volatile u8* port_regs(u32 port)
{
    return g_hba + kPortBase + port * kPortStride;
}
u32 port_read(u32 port, u32 offset)
{
    return *reinterpret_cast<volatile u32*>(port_regs(port) + offset);
}
void port_write(u32 port, u32 offset, u32 value)
{
    *reinterpret_cast<volatile u32*>(port_regs(port) + offset) = value;
}

// Physically contiguous, returned as a direct-map pointer: the controller DMAs
// to physical addresses, but the kernel has to fill the structures first.
void* alloc_dma(usize bytes, paddr_t& phys_out)
{
    const usize pages = (bytes + pmm::kPageSize - 1) / pmm::kPageSize;
    const paddr_t phys = pmm::alloc_contiguous(pages);
    if (phys == 0)
        return nullptr;
    phys_out = phys;
    void* virt = reinterpret_cast<void*>(memory::phys_to_direct(phys));
    memset(virt, 0, pages * pmm::kPageSize);
    return virt;
}

// The port must be idle before its base addresses can be changed, and idle
// means both engines have actually stopped - not merely been asked to.
bool stop_port(u32 port)
{
    port_write(port, kPortCmd,
               port_read(port, kPortCmd) & ~(kCmdStart | kCmdFisReceive));
    for (int spin = 0; spin < 1000000; ++spin) {
        const u32 cmd = port_read(port, kPortCmd);
        if ((cmd & (kCmdListRunning | kCmdFisRunning)) == 0)
            return true;
    }
    return false;
}

void start_port(u32 port)
{
    while ((port_read(port, kPortCmd) & kCmdListRunning) != 0)
        asm volatile("pause");
    port_write(port, kPortCmd, port_read(port, kPortCmd) | kCmdFisReceive);
    port_write(port, kPortCmd, port_read(port, kPortCmd) | kCmdStart);
}

// Build one command and run it to completion. Everything here uses slot 0 and
// polls: with one request in flight at a time there is nothing for a queue to
// do, and polling keeps the driver free of an interrupt path that the rest of
// the kernel would have to be woken through.
bool run_command(Drive& drive, u8 command, u64 lba, u16 sectors, bool write)
{
    const u32 port = drive.port;

    port_write(port, kPortIs, 0xFFFFFFFF);      // clear stale status
    port_write(port, kPortSerr, port_read(port, kPortSerr));

    CommandHeader& header = drive.command_list[0];
    header.flags = static_cast<u16>(sizeof(FisH2D) / sizeof(u32));
    if (write)
        header.flags |= 1u << 6;                // bit 6: this is a write
    header.prdt_length = 1;
    header.bytes_transferred = 0;
    header.command_table = drive.command_table_phys;

    memset(drive.command_table, 0, sizeof(CommandTable));
    drive.command_table->prdt[0].address = drive.transfer_phys;
    drive.command_table->prdt[0].count =
        static_cast<u32>(sectors * kSectorSize - 1);

    auto* fis = reinterpret_cast<FisH2D*>(drive.command_table->command_fis);
    fis->type    = kFisTypeH2D;
    fis->flags   = 0x80;                        // carries a command
    fis->command = command;
    fis->lba0    = static_cast<u8>(lba);
    fis->lba1    = static_cast<u8>(lba >> 8);
    fis->lba2    = static_cast<u8>(lba >> 16);
    fis->device  = 0x40;                        // LBA mode
    fis->lba3    = static_cast<u8>(lba >> 24);
    fis->lba4    = static_cast<u8>(lba >> 32);
    fis->lba5    = static_cast<u8>(lba >> 40);
    fis->count_low  = static_cast<u8>(sectors);
    fis->count_high = static_cast<u8>(sectors >> 8);

    // Wait for the drive to be ready to accept anything at all.
    for (int spin = 0; spin < 1000000; ++spin) {
        if ((port_read(port, kPortTfd) & (kTfdBusy | kTfdDrq)) == 0)
            break;
        if (spin == 999999)
            return false;
    }

    port_write(port, kPortCi, 1);                // issue slot 0

    for (int spin = 0; spin < 100000000; ++spin) {
        if ((port_read(port, kPortCi) & 1) == 0)
            break;
        if ((port_read(port, kPortIs) & (1u << 30)) != 0)   // task file error
            return false;
    }
    if ((port_read(port, kPortCi) & 1) != 0)
        return false;                            // never completed
    return (port_read(port, kPortTfd) & kTfdErr) == 0;
}

// IDENTIFY returns 512 bytes describing the drive; the fields worth having are
// the model string and the 48-bit sector count.
bool identify(Drive& drive)
{
    if (!run_command(drive, kAtaIdentify, 0, 1, false))
        return false;

    const auto* words = reinterpret_cast<const u16*>(drive.transfer);

    // The model sits in words 27-46, byte-swapped within each word.
    for (usize i = 0; i < 20; ++i) {
        drive.model[i * 2]     = static_cast<char>(words[27 + i] >> 8);
        drive.model[i * 2 + 1] = static_cast<char>(words[27 + i] & 0xFF);
    }
    drive.model[40] = '\0';
    for (int i = 39; i >= 0 && drive.model[i] == ' '; --i)
        drive.model[i] = '\0';

    // Word 83 bit 10 says the 48-bit commands are supported, in which case the
    // real capacity is in words 100-103 rather than the 28-bit pair at 60.
    if ((words[83] & (1u << 10)) != 0) {
        drive.sectors = static_cast<u64>(words[100]) |
                        static_cast<u64>(words[101]) << 16 |
                        static_cast<u64>(words[102]) << 32 |
                        static_cast<u64>(words[103]) << 48;
    } else {
        drive.sectors = static_cast<u64>(words[60]) |
                        static_cast<u64>(words[61]) << 16;
    }
    return drive.sectors > 0;
}

bool setup_port(u32 port)
{
    if (g_drive_count >= kMaxDrives)
        return false;

    // A port only counts if the link is up (DET 3) and the device is active
    // (IPM 1), and only a plain disk signature is something we can read.
    const u32 ssts = port_read(port, kPortSsts);
    if ((ssts & 0x0F) != 3 || ((ssts >> 8) & 0x0F) != 1)
        return false;
    if (port_read(port, kPortSig) != kSigSata)
        return false;

    if (!stop_port(port))
        return false;

    Drive& drive = g_drives[g_drive_count];
    drive.port = port;

    drive.command_list = static_cast<CommandHeader*>(
        alloc_dma(sizeof(CommandHeader) * 32, drive.command_list_phys));
    drive.fis = static_cast<u8*>(alloc_dma(256, drive.fis_phys));
    drive.command_table = static_cast<CommandTable*>(
        alloc_dma(sizeof(CommandTable), drive.command_table_phys));
    drive.transfer = static_cast<u8*>(alloc_dma(kTransferBytes, drive.transfer_phys));
    if (drive.command_list == nullptr || drive.fis == nullptr ||
        drive.command_table == nullptr || drive.transfer == nullptr)
        return false;

    port_write(port, kPortClb,  static_cast<u32>(drive.command_list_phys));
    port_write(port, kPortClbu, static_cast<u32>(drive.command_list_phys >> 32));
    port_write(port, kPortFb,   static_cast<u32>(drive.fis_phys));
    port_write(port, kPortFbu,  static_cast<u32>(drive.fis_phys >> 32));
    port_write(port, kPortIe, 0);                // polled, not interrupt driven

    start_port(port);

    if (!identify(drive)) {
        stop_port(port);
        return false;
    }
    ++g_drive_count;
    return true;
}

} // namespace

bool init()
{
    const pci::Device* dev = pci::find(0x01, 0x06);      // mass storage, SATA
    if (dev == nullptr)
        return false;

    // Memory space and bus mastering, or the controller cannot DMA.
    constexpr u8 kCommandReg = 0x04;
    u32 command = pci::read32(dev->bus, dev->slot, dev->function, kCommandReg);
    command |= (1 << 1) | (1 << 2);
    pci::write32(dev->bus, dev->slot, dev->function, kCommandReg, command);

    // The register block is BAR5 for AHCI, not BAR0.
    bool is_io = false;
    const u64 abar = pci::bar_address(*dev, 5, is_io);
    if (is_io || abar == 0)
        return false;

    constexpr vaddr_t kAhciMmio = memory::kDeviceMmioBase + 0x00400000ull;
    if (!vmm::map_mmio(kAhciMmio, abar, 0x2000))
        return false;
    g_hba = reinterpret_cast<volatile u8*>(kAhciMmio);

    // Take the controller out of legacy IDE emulation and into AHCI mode.
    hba_write(kHbaGhc, hba_read(kHbaGhc) | kGhcAhciEnable);

    const u32 implemented = hba_read(kHbaPi);
    const u32 ports = (hba_read(kHbaCap) & 0x1F) + 1;
    for (u32 port = 0; port < ports && port < kMaxPorts; ++port) {
        if ((implemented & (1u << port)) != 0)
            setup_port(port);
    }

    g_up = g_drive_count > 0;
    return g_up;
}

bool available() { return g_up; }

usize drive_count() { return g_drive_count; }

u64 sector_count(usize drive)
{
    return drive < g_drive_count ? g_drives[drive].sectors : 0;
}

const char* model(usize drive)
{
    return drive < g_drive_count ? g_drives[drive].model : "";
}

bool read(usize index, u64 lba, u32 count, void* buffer)
{
    if (index >= g_drive_count || count == 0)
        return false;
    Drive& drive = g_drives[index];

    auto* out = static_cast<u8*>(buffer);
    while (count > 0) {
        const u32 chunk = count > kTransferSectors
                              ? static_cast<u32>(kTransferSectors) : count;
        if (!run_command(drive, kAtaReadDmaEx, lba, static_cast<u16>(chunk), false))
            return false;
        memcpy(out, drive.transfer, chunk * kSectorSize);
        out += chunk * kSectorSize;
        lba += chunk;
        count -= chunk;
    }
    return true;
}

bool write(usize index, u64 lba, u32 count, const void* buffer)
{
    if (index >= g_drive_count || count == 0)
        return false;
    Drive& drive = g_drives[index];

    const auto* in = static_cast<const u8*>(buffer);
    while (count > 0) {
        const u32 chunk = count > kTransferSectors
                              ? static_cast<u32>(kTransferSectors) : count;
        memcpy(drive.transfer, in, chunk * kSectorSize);
        if (!run_command(drive, kAtaWriteDmaEx, lba, static_cast<u16>(chunk), true))
            return false;
        in += chunk * kSectorSize;
        lba += chunk;
        count -= chunk;
    }
    return true;
}

Device::Device(usize drive_index) : m_index(drive_index) {}

bool Device::read(u64 lba, u32 count, void* buffer)
{
    return ahci::read(m_index, lba, count, buffer);
}

bool Device::write(u64 lba, u32 count, const void* buffer)
{
    return ahci::write(m_index, lba, count, buffer);
}

u64 Device::sector_count() const { return ahci::sector_count(m_index); }

} // namespace ahci
