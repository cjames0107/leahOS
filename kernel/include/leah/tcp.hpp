#pragma once

#include <leah/types.hpp>

// TCP, enough of it to be a real client.
//
// What is here: the three-way handshake, sequence and acknowledgement tracking,
// a receive buffer, retransmission on timeout, and an orderly four-way close.
// What is not: congestion control, window scaling, selective acknowledgement,
// out-of-order reassembly, and passive open. A segment that arrives out of
// order is dropped rather than queued, which costs a retransmission and keeps
// the state machine honest instead of half-implementing reassembly.
//
// Like the rest of the stack this polls the NIC rather than being driven by an
// interrupt, so every blocking call here runs the receive path itself.

namespace tcp {

constexpr usize kMaxConnections = 8;

enum class State : u8 {
    Closed,
    SynSent,
    Established,
    FinWait1,       // we sent a FIN, waiting for it to be acknowledged
    FinWait2,       // our FIN is acknowledged, waiting for theirs
    CloseWait,      // they sent a FIN, we still have data to send
    LastAck,
    TimeWait,
};

// Open a connection to `ip`:`port`. Returns a handle, or -1 if the handshake
// did not complete.
int connect(u32 ip, u16 port);

// Send everything in the buffer, retransmitting as needed. Returns bytes sent,
// or -1 on a dead connection.
i64 send(int handle, const void* data, usize length);

// Read up to `length` bytes. Blocks until something arrives, and returns 0 at
// end of stream once the peer has closed. -1 on a dead connection.
i64 recv(int handle, void* data, usize length);

// Orderly shutdown: send a FIN and wait for the peer to finish.
void close(int handle);

State state(int handle);

// Delivered by the IP layer for every TCP segment addressed to us.
void receive(u32 src_ip, const u8* segment, u16 length);

} // namespace tcp
