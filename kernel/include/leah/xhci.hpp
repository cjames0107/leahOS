#pragma once

#include <leah/types.hpp>

// xHCI: the USB 3 host controller interface, and the only one worth writing.
//
// USB went through UHCI, OHCI and EHCI, each with its own register set and its
// own idea of how transfers are described. xHCI replaced all of them with one
// model: the driver builds rings of Transfer Request Blocks in memory, rings a
// doorbell, and reads completions off an event ring. Everything - control
// transfers, bulk, interrupt - is the same mechanism with a different TRB type,
// and the controller handles the split transactions and speed differences that
// made the older interfaces so involved.
//
// This brings the controller up, enumerates what is plugged in, and hands the
// class drivers (usb::hid, usb::storage) a way to talk to endpoints.

namespace xhci {

constexpr usize kMaxPorts   = 32;
constexpr usize kMaxDevices = 8;

// Speeds as the port status register reports them.
enum class Speed : u8 {
    Unknown = 0,
    Full    = 1,        // 12 Mbit/s
    Low     = 2,        // 1.5 Mbit/s
    High    = 3,        // 480 Mbit/s
    Super   = 4,        // 5 Gbit/s
};

struct Device {
    bool  present;
    u8    slot;         // the controller's slot id
    u8    port;         // 1-based, as the register numbering has it
    Speed speed;
    u16   vendor;
    u16   product;
    u8    device_class;
    u8    device_subclass;
    u8    device_protocol;

    // Endpoint addresses the class drivers need, picked out of the
    // configuration descriptor during enumeration. Zero when absent.
    u8    bulk_in;
    u8    bulk_out;
    u8    interrupt_in;
    u16   interrupt_packet;
};

// Find and start the controller, then enumerate every populated port. Returns
// false when there is no xHCI controller.
bool init();
bool available();

usize device_count();
const Device& device_at(usize index);

// --- transfers --------------------------------------------------------------

// A control transfer on endpoint 0. `data` may be null for a request with no
// data stage. Returns bytes transferred, or -1.
i64 control_transfer(u8 slot, u8 request_type, u8 request, u16 value, u16 index,
                     void* data, u16 length);

// Configure a device's endpoints from its configuration descriptor, so bulk and
// interrupt transfers can be issued against them.
bool configure_endpoints(u8 slot, const u8* config_descriptor, u16 length);

// A bulk or interrupt transfer on `endpoint` (the descriptor's bEndpointAddress,
// so the top bit selects direction). Returns bytes transferred, or -1.
i64 transfer(u8 slot, u8 endpoint, void* data, u32 length);

// --- interrupt endpoints ----------------------------------------------------
//
// These cannot be waited on. A keyboard completes its transfer only when a key
// changes, so a blocking read would hang until someone typed. Post one with
// submit_interrupt and collect it later with take_interrupt, which returns 0
// while nothing has arrived.
bool submit_interrupt(u8 slot, u8 endpoint, u32 length);
i64  take_interrupt(u8 slot, void* data, u32 length);

} // namespace xhci
