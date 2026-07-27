#include <leah/console.hpp>
#include <leah/memory.hpp>
#include <leah/pci.hpp>
#include <leah/pmm.hpp>
#include <leah/string.hpp>
#include <leah/apic.hpp>
#include <leah/timer.hpp>
#include <leah/vmm.hpp>
#include <leah/xhci.hpp>

namespace xhci {
namespace {

// --- capability registers (at the start of the register window) -------------
constexpr u32 kCapLength   = 0x00;   // u8; where the operational registers begin
constexpr u32 kHcsParams1  = 0x04;   // slots 7:0, interrupters 18:8, ports 31:24
constexpr u32 kHccParams1  = 0x10;   // AC64 bit 0, context size bit 2
constexpr u32 kDbOff       = 0x14;
constexpr u32 kRtsOff      = 0x18;

// --- operational registers, relative to CAPLENGTH ---------------------------
constexpr u32 kUsbCmd  = 0x00;
constexpr u32 kUsbSts  = 0x04;
constexpr u32 kCrcr    = 0x18;       // command ring control, 64-bit
constexpr u32 kDcbaap  = 0x30;       // device context base address array, 64-bit
constexpr u32 kConfig  = 0x38;
constexpr u32 kPortBase = 0x400;     // port n (1-based) at kPortBase + (n-1)*0x10

constexpr u32 kCmdRun   = 1u << 0;
constexpr u32 kCmdReset = 1u << 1;

constexpr u32 kStsHalted        = 1u << 0;
constexpr u32 kStsControllerNotReady = 1u << 11;

// --- port status and control ------------------------------------------------
constexpr u32 kPortConnected = 1u << 0;
constexpr u32 kPortEnabled   = 1u << 1;
constexpr u32 kPortReset     = 1u << 4;
constexpr u32 kPortPower     = 1u << 9;
// Change bits are write-1-to-clear, and sit alongside bits that are not - so
// every write has to mask them off or it clears things by accident.
constexpr u32 kPortChangeMask = (1u << 17) | (1u << 18) | (1u << 20) |
                                (1u << 21) | (1u << 22);
constexpr u32 kPortWriteMask = ~(kPortChangeMask | kPortEnabled);

// --- TRB types --------------------------------------------------------------
constexpr u32 kTrbNormal        = 1;
constexpr u32 kTrbSetupStage    = 2;
constexpr u32 kTrbDataStage     = 3;
constexpr u32 kTrbStatusStage   = 4;
constexpr u32 kTrbLink          = 6;
constexpr u32 kTrbEnableSlot    = 9;
constexpr u32 kTrbAddressDevice = 11;
constexpr u32 kTrbConfigureEndpoint = 12;
constexpr u32 kTrbTransferEvent = 32;
constexpr u32 kTrbCommandComplete = 33;

constexpr u32 kTrbCycle = 1u << 0;
constexpr u32 kTrbIoc   = 1u << 5;   // interrupt on completion
constexpr u32 kTrbIdt   = 1u << 6;   // immediate data (the setup packet)

constexpr u32 kCompletionSuccess = 1;

struct [[gnu::packed]] Trb {
    u64 parameter;
    u32 status;
    u32 control;
};

constexpr usize kRingSize = 64;      // TRBs per ring, including the link

struct Ring {
    Trb*    trbs;
    paddr_t phys;
    u32     enqueue;
    u32     cycle;
};

volatile u8* g_regs = nullptr;
volatile u8* g_op   = nullptr;
volatile u8* g_run  = nullptr;
volatile u32* g_doorbells = nullptr;

u32 g_max_ports = 0;
u32 g_max_slots = 0;
u32 g_context_size = 32;             // 64 when HCCPARAMS1 bit 2 is set

u64*    g_dcbaa = nullptr;
paddr_t g_dcbaa_phys = 0;

Ring g_command;
Ring g_event;
paddr_t g_erst_phys = 0;
u32  g_event_dequeue = 0;
u32  g_event_cycle = 1;

// Per-slot state: its context, and the transfer rings for the endpoints in use.
struct Slot {
    u8*     context;                 // the device context (output)
    paddr_t context_phys;
    u8*     input;                   // the input context, reused per command
    paddr_t input_phys;
    Ring    rings[32];               // indexed by endpoint context id (dci)
    bool    ring_ready[32];
};

Slot g_slots[kMaxDevices + 1];

Device g_devices[kMaxDevices];
usize  g_device_count = 0;
bool   g_up = false;

u32 read32(volatile u8* base, u32 offset)
{
    return *reinterpret_cast<volatile u32*>(base + offset);
}
void write32(volatile u8* base, u32 offset, u32 value)
{
    *reinterpret_cast<volatile u32*>(base + offset) = value;
}
u64 read64(volatile u8* base, u32 offset)
{
    return *reinterpret_cast<volatile u64*>(base + offset);
}
void write64(volatile u8* base, u32 offset, u64 value)
{
    *reinterpret_cast<volatile u64*>(base + offset) = value;
}

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

// A ring ends with a Link TRB pointing back at its own start, which is what
// makes it a ring rather than a list: the controller follows it round instead
// of running off the end.
bool init_ring(Ring& ring)
{
    ring.trbs = static_cast<Trb*>(alloc_dma(kRingSize * sizeof(Trb), ring.phys));
    if (ring.trbs == nullptr)
        return false;
    ring.enqueue = 0;
    ring.cycle = 1;

    Trb& link = ring.trbs[kRingSize - 1];
    link.parameter = ring.phys;
    link.status = 0;
    // Toggle Cycle: crossing the link flips the producer cycle bit, which is how
    // the controller tells a fresh TRB from a stale one on the next lap.
    link.control = (kTrbLink << 10) | (1u << 1);
    return true;
}

void ring_push(Ring& ring, u64 parameter, u32 status, u32 control)
{
    Trb& trb = ring.trbs[ring.enqueue];
    trb.parameter = parameter;
    trb.status = status;
    trb.control = control | (ring.cycle ? kTrbCycle : 0);

    ++ring.enqueue;
    if (ring.enqueue == kRingSize - 1) {
        // Hand the link TRB to the controller with the current cycle, then flip.
        Trb& link = ring.trbs[kRingSize - 1];
        link.control = (link.control & ~kTrbCycle) | (ring.cycle ? kTrbCycle : 0);
        ring.enqueue = 0;
        ring.cycle ^= 1;
    }
}

// The device context index an endpoint address maps to. Endpoint 0 is 1; after
// that each endpoint number has two slots, OUT then IN. Getting this wrong
// points the controller at the wrong context and nothing transfers.
u32 endpoint_dci(u8 address)
{
    const u32 number = address & 0x0F;
    if (number == 0)
        return 1;
    return number * 2 + ((address & 0x80) != 0 ? 1 : 0);
}

void ring_doorbell(u8 slot, u32 target)
{
    g_doorbells[slot] = target;
}

// An interrupt transfer that has been posted and not yet completed.
//
// A bulk or control transfer can be waited on, because the device answers
// immediately. An interrupt endpoint cannot: a keyboard completes its transfer
// only when a key changes, so waiting would block until someone typed. These
// are posted and left, and the completion is picked up whenever the event ring
// is next drained.
struct Pending {
    bool    active;
    bool    complete;
    u8      endpoint;
    u8*     bounce;
    paddr_t bounce_phys;
    u32     length;
    u32     transferred;
};

Pending g_pending[kMaxDevices + 1]{};

// Record a transfer event against a posted interrupt transfer. Returns true
// when it belonged to one, so the waiting caller knows to keep looking.
bool claim_interrupt_event(u8 slot, u32 endpoint_id, u32 residual)
{
    if (slot == 0 || slot > kMaxDevices)
        return false;
    Pending& pending = g_pending[slot];
    if (!pending.active || endpoint_dci(pending.endpoint) != endpoint_id)
        return false;
    pending.transferred = pending.length > residual ? pending.length - residual
                                                    : pending.length;
    pending.complete = true;
    return true;
}

// Wait for the next event, returning its completion code, and reporting the
// slot it referred to. Polled rather than interrupt-driven, like the NIC.
bool wait_for_event(u32 expected_type, u32& completion, u8& slot_out,
                    u32& transferred)
{
    for (int spin = 0; spin < 20'000'000; ++spin) {
        Trb& trb = g_event.trbs[g_event_dequeue];
        if ((trb.control & kTrbCycle) != g_event_cycle) {
            asm volatile("pause");
            continue;                   // not yet written by the controller
        }

        const u32 type = (trb.control >> 10) & 0x3F;
        const u32 endpoint_id = (trb.control >> 16) & 0x1F;
        completion = (trb.status >> 24) & 0xFF;
        slot_out = static_cast<u8>((trb.control >> 24) & 0xFF);
        transferred = trb.status & 0x1FFFFF;

        // A completion belonging to a posted interrupt transfer is filed away
        // rather than handed to whoever happens to be waiting - the event ring
        // is shared, so without this a control transfer would consume the
        // keyboard's completion and call it its own.
        const bool claimed = type == kTrbTransferEvent &&
                             claim_interrupt_event(slot_out, endpoint_id,
                                                   transferred);

        ++g_event_dequeue;
        if (g_event_dequeue == kRingSize) {
            g_event_dequeue = 0;
            g_event_cycle ^= 1;
        }

        // Tell the controller how far we have consumed, with the busy bit set.
        write64(g_run, 0x38,
                (g_event.phys + g_event_dequeue * sizeof(Trb)) | (1ull << 3));

        if (type == expected_type && !claimed)
            return true;
        // Port status changes arrive unbidden, and claimed interrupt
        // completions belong to someone else; skip both and keep looking.
    }
    return false;
}

// Drain whatever the controller has posted without waiting for anything in
// particular. Completions for posted interrupt transfers are filed as they go
// past; everything else is discarded, which is correct here because the only
// waiters use wait_for_event.
void drain_events()
{
    for (int i = 0; i < 64; ++i) {
        Trb& trb = g_event.trbs[g_event_dequeue];
        if ((trb.control & kTrbCycle) != g_event_cycle)
            return;

        const u32 type = (trb.control >> 10) & 0x3F;
        const u32 endpoint_id = (trb.control >> 16) & 0x1F;
        const u8  slot = static_cast<u8>((trb.control >> 24) & 0xFF);
        const u32 residual = trb.status & 0x1FFFFF;
        if (type == kTrbTransferEvent)
            claim_interrupt_event(slot, endpoint_id, residual);

        ++g_event_dequeue;
        if (g_event_dequeue == kRingSize) {
            g_event_dequeue = 0;
            g_event_cycle ^= 1;
        }
        write64(g_run, 0x38,
                (g_event.phys + g_event_dequeue * sizeof(Trb)) | (1ull << 3));
    }
}

// Issue a command on the command ring and wait for its completion event.
bool run_command(u64 parameter, u32 control, u8& slot_out)
{
    ring_push(g_command, parameter, 0, control | kTrbIoc);
    ring_doorbell(0, 0);

    u32 completion = 0;
    u32 transferred = 0;
    if (!wait_for_event(kTrbCommandComplete, completion, slot_out, transferred))
        return false;
    return completion == kCompletionSuccess;
}

volatile u8* port_regs(u32 port)          // port is 1-based
{
    return g_op + kPortBase + (port - 1) * 0x10;
}

u32* context_at(u8* base, u32 index)
{
    return reinterpret_cast<u32*>(base + index * g_context_size);
}

u16 default_packet_size(Speed speed)
{
    switch (speed) {
    case Speed::Low:   return 8;
    case Speed::Super: return 512;
    default:           return 64;      // full and high speed
    }
}

// Reset a port and wait for the controller to enable it. USB 3 ports train
// themselves on connect, but a USB 2 port stays disabled until asked.
bool reset_port(u32 port)
{
    volatile u8* regs = port_regs(port);
    u32 status = read32(regs, 0);
    if ((status & kPortEnabled) != 0)
        return true;

    write32(regs, 0, (status & kPortWriteMask) | kPortReset);
    for (int i = 0; i < 1000; ++i) {
        apic::delay_us(1000);
        status = read32(regs, 0);
        if ((status & kPortReset) == 0 && (status & kPortEnabled) != 0) {
            // Acknowledge the change bits so the next event is a real one.
            write32(regs, 0, (status & kPortWriteMask) | (status & kPortChangeMask));
            return true;
        }
    }
    return false;
}

// Give a slot its contexts and tell the controller where the device lives.
bool address_device(u8 slot_id, u32 port, Speed speed)
{
    Slot& slot = g_slots[slot_id];

    slot.context = static_cast<u8*>(alloc_dma(g_context_size * 32,
                                              slot.context_phys));
    slot.input = static_cast<u8*>(alloc_dma(g_context_size * 33, slot.input_phys));
    if (slot.context == nullptr || slot.input == nullptr)
        return false;
    if (!init_ring(slot.rings[1]))
        return false;
    slot.ring_ready[1] = true;

    g_dcbaa[slot_id] = slot.context_phys;

    // Input control context: add the slot context and endpoint 0.
    u32* control = context_at(slot.input, 0);
    control[0] = 0;                          // drop nothing
    control[1] = (1u << 0) | (1u << 1);      // add slot + ep0

    u32* slot_ctx = context_at(slot.input, 1);
    slot_ctx[0] = (static_cast<u32>(speed) << 20) | (1u << 27);   // 1 context entry
    slot_ctx[1] = port << 16;

    u32* ep0 = context_at(slot.input, 2);
    ep0[1] = (3u << 1)                                   // CErr = 3
           | (4u << 3)                                   // EP type: control
           | (static_cast<u32>(default_packet_size(speed)) << 16);
    const u64 dequeue = slot.rings[1].phys | 1;          // dequeue cycle state
    ep0[2] = static_cast<u32>(dequeue);
    ep0[3] = static_cast<u32>(dequeue >> 32);
    ep0[4] = 8;                                          // average TRB length

    u8 completed_slot = 0;
    return run_command(slot.input_phys,
                       (kTrbAddressDevice << 10) | (static_cast<u32>(slot_id) << 24),
                       completed_slot);
}

} // namespace

i64 control_transfer(u8 slot_id, u8 request_type, u8 request, u16 value,
                     u16 index, void* data, u16 length)
{
    if (slot_id == 0 || slot_id > kMaxDevices)
        return -1;
    Slot& slot = g_slots[slot_id];
    if (!slot.ring_ready[1])
        return -1;
    Ring& ring = slot.rings[1];

    paddr_t data_phys = 0;
    u8* bounce = nullptr;
    if (length > 0) {
        bounce = static_cast<u8*>(alloc_dma(length, data_phys));
        if (bounce == nullptr)
            return -1;
        if ((request_type & 0x80) == 0 && data != nullptr)
            memcpy(bounce, data, length);
    }

    // The setup packet travels inside the TRB itself rather than through a
    // buffer, which is what the immediate-data bit means.
    const u64 setup = static_cast<u64>(request_type) |
                      static_cast<u64>(request) << 8 |
                      static_cast<u64>(value) << 16 |
                      static_cast<u64>(index) << 32 |
                      static_cast<u64>(length) << 48;
    const u32 in = (request_type & 0x80) != 0 ? 1 : 0;
    const u32 transfer_type = length == 0 ? 0u : (in ? 3u : 2u);

    ring_push(ring, setup, 8,
              kTrbIdt | (transfer_type << 16) | (kTrbSetupStage << 10));
    if (length > 0)
        ring_push(ring, data_phys, length, (in << 16) | (kTrbDataStage << 10));
    // The status stage runs opposite to the data stage.
    ring_push(ring, 0, 0, ((in ? 0u : 1u) << 16) | (kTrbStatusStage << 10) | kTrbIoc);

    ring_doorbell(slot_id, 1);

    u32 completion = 0;
    u32 residual = 0;
    u8 event_slot = 0;
    if (!wait_for_event(kTrbTransferEvent, completion, event_slot, residual)) {
        return -1;
    }
    if (completion != kCompletionSuccess && completion != 13)   // 13 = short packet
        return -1;

    const u32 moved = length > residual ? length - residual : length;
    if (in && data != nullptr && bounce != nullptr)
        memcpy(data, bounce, moved);
    return static_cast<i64>(moved);
}

// Walk a configuration descriptor and set up every endpoint it declares, so
// bulk and interrupt transfers can be issued against them. One Configure
// Endpoint command adds them all at once.
bool configure_endpoints(u8 slot_id, const u8* config, u16 length)
{
    if (slot_id == 0 || slot_id > kMaxDevices || length < 9)
        return false;
    Slot& slot = g_slots[slot_id];

    u32* control = context_at(slot.input, 0);
    memset(slot.input, 0, g_context_size * 33);
    control[0] = 0;
    control[1] = 1;                              // the slot context always
    u32 max_dci = 1;

    // Descriptors are a chain of {length, type, ...} records; endpoint records
    // are type 5.
    u16 offset = 0;
    while (offset + 2 <= length) {
        const u8 item_length = config[offset];
        const u8 item_type   = config[offset + 1];
        if (item_length < 2)
            break;

        if (item_type == 5 && offset + 6 <= length) {      // endpoint descriptor
            const u8  address    = config[offset + 2];
            const u8  attributes = config[offset + 3];
            const u16 packet     = static_cast<u16>(config[offset + 4] |
                                                    config[offset + 5] << 8);
            const u8  interval   = item_length > 6 ? config[offset + 6] : 0;

            const u32 dci = endpoint_dci(address);
            if (dci < 32 && !slot.ring_ready[dci] && init_ring(slot.rings[dci])) {
                slot.ring_ready[dci] = true;

                // xHCI numbers endpoint types with direction folded in: OUT
                // takes the raw transfer type, IN the same plus four.
                const u32 transfer = attributes & 0x03;
                const u32 type = (address & 0x80) != 0 ? transfer + 4 : transfer;

                u32* ep = context_at(slot.input, dci + 1);
                ep[0] = static_cast<u32>(interval) << 16;
                ep[1] = (3u << 1) | (type << 3) |
                        (static_cast<u32>(packet & 0x7FF) << 16);
                const u64 dequeue = slot.rings[dci].phys | 1;
                ep[2] = static_cast<u32>(dequeue);
                ep[3] = static_cast<u32>(dequeue >> 32);
                ep[4] = packet;

                control[1] |= 1u << dci;
                if (dci > max_dci)
                    max_dci = dci;
            }
        }
        offset += item_length;
    }

    // The slot context has to say how many endpoint contexts follow, or the
    // controller ignores the ones past its count.
    u32* slot_ctx = context_at(slot.input, 1);
    const u32* live = context_at(slot.context, 0);
    slot_ctx[0] = (live[0] & 0x07FFFFFF) | (max_dci << 27);
    slot_ctx[1] = live[1];

    u8 completed = 0;
    return run_command(slot.input_phys,
                       (kTrbConfigureEndpoint << 10) |
                           (static_cast<u32>(slot_id) << 24),
                       completed);
}

i64 transfer(u8 slot_id, u8 endpoint, void* data, u32 length)
{
    if (slot_id == 0 || slot_id > kMaxDevices)
        return -1;
    Slot& slot = g_slots[slot_id];
    const u32 dci = endpoint_dci(endpoint);
    if (dci >= 32 || !slot.ring_ready[dci])
        return -1;

    paddr_t phys = 0;
    u8* bounce = static_cast<u8*>(alloc_dma(length > 0 ? length : 1, phys));
    if (bounce == nullptr)
        return -1;
    const bool in = (endpoint & 0x80) != 0;
    if (!in && data != nullptr)
        memcpy(bounce, data, length);

    ring_push(slot.rings[dci], phys, length, (kTrbNormal << 10) | kTrbIoc);
    ring_doorbell(slot_id, dci);

    u32 completion = 0;
    u32 residual = 0;
    u8 event_slot = 0;
    if (!wait_for_event(kTrbTransferEvent, completion, event_slot, residual))
        return -1;
    if (completion != kCompletionSuccess && completion != 13)   // short packet
        return -1;

    const u32 moved = length > residual ? length - residual : length;
    if (in && data != nullptr)
        memcpy(data, bounce, moved);
    return static_cast<i64>(moved);
}

bool submit_interrupt(u8 slot_id, u8 endpoint, u32 length)
{
    if (slot_id == 0 || slot_id > kMaxDevices || length == 0 || length > 64)
        return false;
    Slot& slot = g_slots[slot_id];
    const u32 dci = endpoint_dci(endpoint);
    if (dci >= 32 || !slot.ring_ready[dci])
        return false;

    Pending& pending = g_pending[slot_id];
    if (pending.active)
        return false;                    // one in flight per device is enough

    if (pending.bounce == nullptr) {
        pending.bounce = static_cast<u8*>(alloc_dma(64, pending.bounce_phys));
        if (pending.bounce == nullptr)
            return false;
    }

    pending.endpoint    = endpoint;
    pending.length      = length;
    pending.transferred = 0;
    pending.complete    = false;
    pending.active      = true;

    ring_push(slot.rings[dci], pending.bounce_phys, length,
              (kTrbNormal << 10) | kTrbIoc);
    ring_doorbell(slot_id, dci);
    return true;
}

i64 take_interrupt(u8 slot_id, void* data, u32 length)
{
    if (slot_id == 0 || slot_id > kMaxDevices)
        return -1;
    Pending& pending = g_pending[slot_id];
    if (!pending.active)
        return -1;

    drain_events();
    if (!pending.complete)
        return 0;                        // nothing yet, which is not an error

    const u32 moved = pending.transferred < length ? pending.transferred : length;
    if (data != nullptr)
        memcpy(data, pending.bounce, moved);
    pending.active = false;
    pending.complete = false;
    return static_cast<i64>(moved);
}

bool init()
{
    // xHCI is class 0x0C (serial bus), subclass 0x03 (USB), programming
    // interface 0x30. The older host controllers share the class and subclass,
    // so the programming interface is what distinguishes them.
    const pci::Device* dev = nullptr;
    for (usize i = 0; i < pci::device_count(); ++i) {
        const pci::Device& d = pci::device_at(i);
        if (d.class_code == 0x0C && d.subclass == 0x03 && d.prog_if == 0x30) {
            dev = &d;
            break;
        }
    }
    if (dev == nullptr)
        return false;

    constexpr u8 kCommandReg = 0x04;
    u32 command = pci::read32(dev->bus, dev->slot, dev->function, kCommandReg);
    command |= (1 << 1) | (1 << 2);              // memory space, bus master
    pci::write32(dev->bus, dev->slot, dev->function, kCommandReg, command);

    bool is_io = false;
    const u64 bar = pci::bar_address(*dev, 0, is_io);
    if (is_io || bar == 0)
        return false;

    constexpr vaddr_t kXhciMmio = memory::kDeviceMmioBase + 0x00500000ull;
    if (!vmm::map_mmio(kXhciMmio, bar, 0x10000))
        return false;
    g_regs = reinterpret_cast<volatile u8*>(kXhciMmio);

    const u8 cap_length = *reinterpret_cast<volatile u8*>(g_regs + kCapLength);
    g_op  = g_regs + cap_length;
    g_run = g_regs + (read32(g_regs, kRtsOff) & ~0x1Fu);
    g_doorbells = reinterpret_cast<volatile u32*>(
        g_regs + (read32(g_regs, kDbOff) & ~0x3u));

    const u32 hcs1 = read32(g_regs, kHcsParams1);
    g_max_slots = hcs1 & 0xFF;
    g_max_ports = (hcs1 >> 24) & 0xFF;
    if (g_max_ports > kMaxPorts)
        g_max_ports = kMaxPorts;
    // A context is 32 bytes unless HCCPARAMS1 says otherwise; getting this wrong
    // misaligns every endpoint context in the device context.
    g_context_size = (read32(g_regs, kHccParams1) & (1u << 2)) != 0 ? 64 : 32;

    // Stop the controller, then reset it and wait for it to say it is ready.
    write32(g_op, kUsbCmd, read32(g_op, kUsbCmd) & ~kCmdRun);
    for (int i = 0; i < 1000000 && (read32(g_op, kUsbSts) & kStsHalted) == 0; ++i)
        asm volatile("pause");

    write32(g_op, kUsbCmd, read32(g_op, kUsbCmd) | kCmdReset);
    for (int i = 0; i < 1000000 && (read32(g_op, kUsbCmd) & kCmdReset) != 0; ++i)
        asm volatile("pause");
    for (int i = 0; i < 1000000 &&
                    (read32(g_op, kUsbSts) & kStsControllerNotReady) != 0; ++i)
        asm volatile("pause");

    // The device context base address array: one physical pointer per slot,
    // which the controller reads to find each device's context.
    g_dcbaa = static_cast<u64*>(alloc_dma((g_max_slots + 1) * sizeof(u64),
                                          g_dcbaa_phys));
    if (g_dcbaa == nullptr)
        return false;
    write32(g_op, kConfig, g_max_slots);
    write64(g_op, kDcbaap, g_dcbaa_phys);

    if (!init_ring(g_command))
        return false;
    write64(g_op, kCrcr, g_command.phys | 1);   // ring cycle state = 1

    // The event ring is described by a segment table rather than pointed at
    // directly, so a controller can be given several discontiguous segments.
    if (!init_ring(g_event))
        return false;
    g_event.cycle = 1;
    g_event_cycle = 1;
    g_event_dequeue = 0;

    auto* erst = static_cast<u64*>(alloc_dma(16, g_erst_phys));
    if (erst == nullptr)
        return false;
    erst[0] = g_event.phys;
    erst[1] = kRingSize;                        // entries in this segment

    write32(g_run, 0x28, 1);                    // ERSTSZ: one segment
    write64(g_run, 0x38, g_event.phys);         // ERDP
    write64(g_run, 0x30, g_erst_phys);          // ERSTBA, written last

    write32(g_op, kUsbCmd, read32(g_op, kUsbCmd) | kCmdRun);
    for (int i = 0; i < 1000000 && (read32(g_op, kUsbSts) & kStsHalted) != 0; ++i)
        asm volatile("pause");

    g_up = (read32(g_op, kUsbSts) & kStsHalted) == 0;
    if (!g_up)
        return false;

    // Walk the root ports. Anything connected gets reset, given a slot, and
    // asked what it is.
    for (u32 port = 1; port <= g_max_ports && g_device_count < kMaxDevices; ++port) {
        volatile u8* regs = port_regs(port);
        u32 status = read32(regs, 0);

        // Power the port if it is not already; a port with no power reports
        // nothing connected however hard it is asked.
        if ((status & kPortPower) == 0) {
            write32(regs, 0, (status & kPortWriteMask) | kPortPower);
            apic::delay_us(20000);
            status = read32(regs, 0);
        }
        if ((status & kPortConnected) == 0)
            continue;

        if (!reset_port(port))
            continue;

        status = read32(regs, 0);
        const auto speed = static_cast<Speed>((status >> 10) & 0x0F);

        u8 slot_id = 0;
        if (!run_command(0, kTrbEnableSlot << 10, slot_id) || slot_id == 0 ||
            slot_id > kMaxDevices)
            continue;
        if (!address_device(slot_id, port, speed))
            continue;

        // GET_DESCRIPTOR(device): 0x80 is device-to-host, and descriptor type 1
        // in the high byte of wValue is the device descriptor.
        u8 descriptor[18] = {};
        if (control_transfer(slot_id, 0x80, 6, 0x0100, 0, descriptor,
                             sizeof(descriptor)) < 8)
            continue;

        Device& device = g_devices[g_device_count++];
        device.present         = true;
        device.slot            = slot_id;
        device.port            = static_cast<u8>(port);
        device.speed           = speed;
        device.device_class    = descriptor[4];
        device.device_subclass = descriptor[5];
        device.device_protocol = descriptor[6];
        device.vendor  = static_cast<u16>(descriptor[8] | descriptor[9] << 8);
        device.product = static_cast<u16>(descriptor[10] | descriptor[11] << 8);

        // A device descriptor of class 0 means "look at the interface", which
        // is the usual case - so read the configuration and take the class from
        // the first interface it declares.
        u8 config[256] = {};
        const i64 got = control_transfer(slot_id, 0x80, 6, 0x0200, 0,
                                         config, sizeof(config));
        if (got >= 9) {
            const u16 total = static_cast<u16>(config[2] | config[3] << 8);
            const u16 have = static_cast<u16>(got < total ? got : total);
            for (u16 o = 0; o + 2 <= have; o += config[o] == 0 ? 2 : config[o]) {
                if (config[o + 1] == 4 && o + 8 <= have) {     // interface
                    if (device.device_class == 0) {
                        device.device_class    = config[o + 5];
                        device.device_subclass = config[o + 6];
                        device.device_protocol = config[o + 7];
                    }
                } else if (config[o + 1] == 5 && o + 6 <= have) {   // endpoint
                    const u8 address = config[o + 2];
                    const u8 kind = config[o + 3] & 0x03;
                    if (kind == 2) {                            // bulk
                        if ((address & 0x80) != 0)
                            device.bulk_in = address;
                        else
                            device.bulk_out = address;
                    } else if (kind == 3 && (address & 0x80) != 0) {
                        device.interrupt_in = address;
                        device.interrupt_packet =
                            static_cast<u16>(config[o + 4] | config[o + 5] << 8);
                    }
                }
            }
            // SET_CONFIGURATION, then hand the endpoints to the controller.
            control_transfer(slot_id, 0x00, 9, config[5], 0, nullptr, 0);
            configure_endpoints(slot_id, config, have);
        }
    }

    return true;
}

bool available() { return g_up; }

usize device_count() { return g_device_count; }
const Device& device_at(usize index) { return g_devices[index]; }

} // namespace xhci
