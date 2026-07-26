#include <leah/console.hpp>
#include <leah/e1000.hpp>
#include <leah/memory.hpp>
#include <leah/pci.hpp>
#include <leah/pmm.hpp>
#include <leah/string.hpp>
#include <leah/vmm.hpp>

namespace e1000 {
namespace {

// --- register offsets from BAR0 --------------------------------------------
constexpr u32 kCtrl   = 0x0000;
constexpr u32 kStatus = 0x0008;
constexpr u32 kEerd   = 0x0014;
constexpr u32 kIcr    = 0x00C0;
constexpr u32 kIms    = 0x00D0;
constexpr u32 kImc    = 0x00D8;
constexpr u32 kRctl   = 0x0100;
constexpr u32 kTctl   = 0x0400;
constexpr u32 kTipg   = 0x0410;
constexpr u32 kRdbal  = 0x2800;
constexpr u32 kRdbah  = 0x2804;
constexpr u32 kRdlen  = 0x2808;
constexpr u32 kRdh    = 0x2810;
constexpr u32 kRdt    = 0x2818;
constexpr u32 kTdbal  = 0x3800;
constexpr u32 kTdbah  = 0x3804;
constexpr u32 kTdlen  = 0x3808;
constexpr u32 kTdh    = 0x3810;
constexpr u32 kTdt    = 0x3818;
constexpr u32 kRal0   = 0x5400;
constexpr u32 kRah0   = 0x5404;

// --- control bits ----------------------------------------------------------
constexpr u32 kCtrlReset  = 1u << 26;
constexpr u32 kCtrlSlu    = 1u << 6;    // set link up

constexpr u32 kRctlEn     = 1u << 1;
constexpr u32 kRctlUpe    = 1u << 3;    // unicast promiscuous
constexpr u32 kRctlBam    = 1u << 15;   // accept broadcast
constexpr u32 kRctlSecrc  = 1u << 26;   // strip the Ethernet CRC
// RCTL BSIZE bits left at 0 => 2048-byte buffers.

constexpr u32 kTctlEn     = 1u << 1;
constexpr u32 kTctlPsp    = 1u << 3;    // pad short packets

constexpr u32 kImsRxt0    = 1u << 7;    // receiver timer interrupt

// --- descriptors -----------------------------------------------------------
struct [[gnu::packed]] RxDesc {
    u64 addr;
    u16 length;
    u16 checksum;
    u8  status;
    u8  errors;
    u16 special;
};

struct [[gnu::packed]] TxDesc {
    u64 addr;
    u16 length;
    u8  cso;
    u8  cmd;
    u8  status;
    u8  css;
    u16 special;
};

constexpr u8 kRxStatusDd = 1 << 0;      // descriptor done

constexpr u8 kTxCmdEop  = 1 << 0;       // end of packet
constexpr u8 kTxCmdIfcs = 1 << 1;       // insert FCS
constexpr u8 kTxCmdRs   = 1 << 3;       // report status
constexpr u8 kTxStatusDd = 1 << 0;

constexpr usize kRxCount   = 32;
constexpr usize kTxCount   = 32;
constexpr usize kBufSize   = 2048;

volatile u8* g_regs = nullptr;
u8  g_mac[kMacLength] = {};
bool g_up = false;
Receiver g_receiver = nullptr;

RxDesc* g_rx = nullptr;              // ring, in the direct map
TxDesc* g_tx = nullptr;
paddr_t g_rx_phys = 0;
paddr_t g_tx_phys = 0;
u8* g_rx_buffers = nullptr;          // kRxCount * kBufSize, direct map
u8* g_tx_buffers = nullptr;
paddr_t g_rx_buf_phys = 0;
paddr_t g_tx_buf_phys = 0;
usize g_tx_tail = 0;

void write_reg(u32 offset, u32 value)
{
    *reinterpret_cast<volatile u32*>(g_regs + offset) = value;
}

u32 read_reg(u32 offset)
{
    return *reinterpret_cast<volatile u32*>(g_regs + offset);
}

// The MAC lives in the Receive Address registers after reset on QEMU's e1000;
// RAH's high bit marks it valid.
bool read_mac()
{
    const u32 low = read_reg(kRal0);
    const u32 high = read_reg(kRah0);
    if ((high & (1u << 31)) == 0)
        return false;
    g_mac[0] = low & 0xFF;
    g_mac[1] = low >> 8 & 0xFF;
    g_mac[2] = low >> 16 & 0xFF;
    g_mac[3] = low >> 24 & 0xFF;
    g_mac[4] = high & 0xFF;
    g_mac[5] = high >> 8 & 0xFF;
    return true;
}

// A page of contiguous physical memory, returned as a direct-map pointer with
// its physical base written out (the NIC's DMA needs the physical address).
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

void setup_rx()
{
    g_rx = static_cast<RxDesc*>(alloc_dma(kRxCount * sizeof(RxDesc), g_rx_phys));
    g_rx_buffers = static_cast<u8*>(alloc_dma(kRxCount * kBufSize, g_rx_buf_phys));

    for (usize i = 0; i < kRxCount; ++i) {
        g_rx[i].addr = g_rx_buf_phys + i * kBufSize;
        g_rx[i].status = 0;
    }

    write_reg(kRdbal, static_cast<u32>(g_rx_phys));
    write_reg(kRdbah, static_cast<u32>(g_rx_phys >> 32));
    write_reg(kRdlen, kRxCount * sizeof(RxDesc));
    write_reg(kRdh, 0);
    write_reg(kRdt, kRxCount - 1);      // hand every descriptor to the card

    // Receive unicast promiscuously: QEMU's e1000 receive-address filter drops
    // our unicast replies even with RAL0/RAH0 holding our own MAC, and on a
    // point-to-point virtual link there is no other traffic to sift out anyway.
    write_reg(kRctl, kRctlEn | kRctlUpe | kRctlBam | kRctlSecrc);
}

void setup_tx()
{
    g_tx = static_cast<TxDesc*>(alloc_dma(kTxCount * sizeof(TxDesc), g_tx_phys));
    g_tx_buffers = static_cast<u8*>(alloc_dma(kTxCount * kBufSize, g_tx_buf_phys));

    for (usize i = 0; i < kTxCount; ++i)
        g_tx[i].status = kTxStatusDd;   // free to use

    write_reg(kTdbal, static_cast<u32>(g_tx_phys));
    write_reg(kTdbah, static_cast<u32>(g_tx_phys >> 32));
    write_reg(kTdlen, kTxCount * sizeof(TxDesc));
    write_reg(kTdh, 0);
    write_reg(kTdt, 0);
    write_reg(kTctl, kTctlEn | kTctlPsp | (0x0F << 4) | (0x40 << 12));
    write_reg(kTipg, 10 | (8 << 10) | (6 << 20));
    g_tx_tail = 0;
}

// Drain every completed receive descriptor, handing each frame to the stack,
// then return the descriptors to the card.
void handle_receive()
{
    u32 tail = read_reg(kRdt);
    for (;;) {
        const u32 index = (tail + 1) % kRxCount;
        RxDesc& d = g_rx[index];
        if ((d.status & kRxStatusDd) == 0)
            break;

        const u8* frame = g_rx_buffers + index * kBufSize;
        if (g_receiver != nullptr && d.length > 0)
            g_receiver(frame, d.length);

        d.status = 0;
        tail = index;
        write_reg(kRdt, tail);
    }
}

} // namespace

bool init()
{
    const pci::Device* dev = pci::find(0x02, 0x00);      // network controller
    if (dev == nullptr)
        return false;

    // Enable memory space and bus mastering so the card can DMA.
    constexpr u8 kCommand = 0x04;
    u32 command = pci::read32(dev->bus, dev->slot, dev->function, kCommand);
    command |= (1 << 1) | (1 << 2);                       // memory space, bus master
    pci::write32(dev->bus, dev->slot, dev->function, kCommand, command);

    bool is_io = false;
    const u64 bar0 = pci::bar_address(*dev, 0, is_io);
    if (is_io || bar0 == 0)
        return false;

    // Map the register window uncached, high up.
    if (!vmm::map_mmio(memory::kDeviceMmioBase, bar0, 0x20000))
        return false;
    g_regs = reinterpret_cast<volatile u8*>(memory::kDeviceMmioBase);

    // Reset, then bring the link up.
    write_reg(kImc, 0xFFFFFFFF);                          // mask all interrupts
    write_reg(kCtrl, read_reg(kCtrl) | kCtrlReset);
    for (int i = 0; i < 1000000; ++i)                      // let the reset settle
        asm volatile("");                                 // and do not optimise away
    write_reg(kImc, 0xFFFFFFFF);
    write_reg(kCtrl, read_reg(kCtrl) | kCtrlSlu);

    if (!read_mac())
        return false;

    // Clear the multicast table filter.
    for (u32 i = 0; i < 128; ++i)
        write_reg(0x5200 + i * 4, 0);

    setup_rx();
    setup_tx();

    // Receive is polled rather than interrupt-driven: with no BIOS to program
    // PCI interrupt routing, the card's reported IRQ line cannot be trusted, and
    // hooking the wrong one would clobber another device. The NIC DMAs frames
    // and sets the descriptor-done bit regardless of interrupts, so poll() reads
    // them straight from the ring.
    read_reg(kIcr);                                       // clear any pending cause

    g_up = true;
    return true;
}

void poll()
{
    if (g_up)
        handle_receive();
}

bool available() { return g_up; }

const u8* mac() { return g_mac; }

void set_receiver(Receiver receiver) { g_receiver = receiver; }

bool send(const void* frame, u16 length)
{
    if (!g_up || length == 0 || length > kBufSize)
        return false;

    TxDesc& d = g_tx[g_tx_tail];
    if ((d.status & kTxStatusDd) == 0)
        return false;                                     // ring full

    memcpy(g_tx_buffers + g_tx_tail * kBufSize, frame, length);
    d.addr   = g_tx_buf_phys + g_tx_tail * kBufSize;
    d.length = length;
    d.cmd    = kTxCmdEop | kTxCmdIfcs | kTxCmdRs;
    d.status = 0;

    g_tx_tail = (g_tx_tail + 1) % kTxCount;
    write_reg(kTdt, g_tx_tail);
    return true;
}

} // namespace e1000
