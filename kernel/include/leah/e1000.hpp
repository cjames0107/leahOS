#pragma once

#include <leah/types.hpp>

// Driver for the Intel 82540EM ("e1000"), the NIC QEMU gives us. It brings the
// card up, reads its MAC, and moves raw Ethernet frames: send() transmits one,
// and poll() hands received frames to a registered callback. Receive is polled
// rather than interrupt-driven. The protocol layers above it live in net/.

namespace e1000 {

constexpr usize kMacLength = 6;

using Receiver = void (*)(const u8* frame, u16 length);

// Finds the card on the PCI bus, maps its registers, resets it, and sets up the
// receive and transmit rings. Returns false if no e1000 is present.
bool init();
bool available();

const u8* mac();

// Transmit one Ethernet frame (already including the destination/source MACs
// and ethertype). Returns false if the ring is full or the driver is down.
bool send(const void* frame, u16 length);

// Register the function received frames are delivered to.
void set_receiver(Receiver receiver);

// Process any frames the card has DMA'd into the receive ring, delivering each
// to the receiver. Called from the poll loop, since receive is not interrupt
// driven.
void poll();

} // namespace e1000
