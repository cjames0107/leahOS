#include <leah/cpu.hpp>
#include <leah/net.hpp>
#include <leah/scheduler.hpp>
#include <leah/string.hpp>
#include <leah/tcp.hpp>

namespace tcp {
namespace {

struct [[gnu::packed]] Header {
    u16 src_port;       // all network order
    u16 dst_port;
    u32 seq;
    u32 ack;
    u8  offset;         // header length in dwords, in the high nibble
    u8  flags;
    u16 window;
    u16 checksum;
    u16 urgent;
};

// The pseudo-header the TCP checksum covers in addition to the segment: it ties
// a segment to the addresses that carried it, so one delivered to the wrong host
// fails the check rather than being accepted.
struct [[gnu::packed]] PseudoHeader {
    u32 src;
    u32 dst;
    u8  zero;
    u8  protocol;
    u16 tcp_length;
};

constexpr u8 kFin = 1 << 0;
constexpr u8 kSyn = 1 << 1;
constexpr u8 kRst = 1 << 2;
constexpr u8 kPsh = 1 << 3;
constexpr u8 kAck = 1 << 4;

constexpr usize kRxBuffer = 16384;
constexpr u16   kWindow   = kRxBuffer / 2;
constexpr u16   kMss      = 1400;       // comfortably inside a 1500-byte MTU

struct Connection {
    bool  used;
    State state;
    u32   remote_ip;
    u16   remote_port;
    u16   local_port;

    u32 snd_nxt;        // next sequence number we will send
    u32 snd_una;        // oldest unacknowledged
    u32 rcv_nxt;        // next sequence number we expect

    bool peer_finished; // their FIN arrived and was acknowledged

    u8    rx[kRxBuffer];
    usize rx_head;      // next byte to hand to the reader
    usize rx_tail;      // next free byte
};

Connection g_connections[kMaxConnections];
u16 g_next_port = 49152;                // the ephemeral range

usize rx_available(const Connection& c)
{
    return c.rx_tail >= c.rx_head ? c.rx_tail - c.rx_head
                                  : kRxBuffer - c.rx_head + c.rx_tail;
}

void rx_push(Connection& c, const u8* data, usize length)
{
    for (usize i = 0; i < length; ++i) {
        const usize next = (c.rx_tail + 1) % kRxBuffer;
        if (next == c.rx_head)
            return;                     // full: the window should have prevented this
        c.rx[c.rx_tail] = data[i];
        c.rx_tail = next;
    }
}

bool valid(int handle)
{
    return handle >= 0 && handle < static_cast<int>(kMaxConnections) &&
           g_connections[handle].used;
}

// Build and send one segment. `payload` may be empty, which is how a bare ACK,
// SYN or FIN goes out.
bool send_segment(Connection& c, u8 flags, const u8* payload, u16 payload_len)
{
    u8 buffer[sizeof(Header) + kMss];
    if (payload_len > kMss)
        return false;

    auto* header = reinterpret_cast<Header*>(buffer);
    memset(header, 0, sizeof(Header));
    header->src_port = net::hton16(c.local_port);
    header->dst_port = net::hton16(c.remote_port);
    header->seq      = net::hton32(c.snd_nxt);
    header->ack      = net::hton32(c.rcv_nxt);
    header->offset   = (sizeof(Header) / 4) << 4;
    header->flags    = flags;
    header->window   = net::hton16(kWindow);
    if (payload_len > 0)
        memcpy(buffer + sizeof(Header), payload, payload_len);

    const u16 total = static_cast<u16>(sizeof(Header) + payload_len);

    // The checksum spans the pseudo-header and the segment, so both are summed
    // into one contiguous scratch buffer rather than folded by hand.
    u8 scratch[sizeof(PseudoHeader) + sizeof(Header) + kMss];
    auto* pseudo = reinterpret_cast<PseudoHeader*>(scratch);
    pseudo->src        = net::hton32(net::our_ip());
    pseudo->dst        = net::hton32(c.remote_ip);
    pseudo->zero       = 0;
    pseudo->protocol   = net::kProtocolTcp;
    pseudo->tcp_length = net::hton16(total);
    memcpy(scratch + sizeof(PseudoHeader), buffer, total);
    // Computed with the checksum field zero, then written into the real header.
    header->checksum = net::checksum16(scratch, sizeof(PseudoHeader) + total);

    return net::send_ip(c.remote_ip, net::kProtocolTcp, buffer, total);
}

// Poll the receive path once and yield to the host. Every blocking call here
// drives the NIC itself, since nothing else does.
void pump()
{
    net::poll();
    asm volatile("hlt");
}

} // namespace

void receive(u32 src_ip, const u8* segment, u16 length)
{
    if (length < sizeof(Header))
        return;
    const auto* header = reinterpret_cast<const Header*>(segment);

    const u16 dst_port = net::ntoh16(header->dst_port);
    Connection* found = nullptr;
    for (usize i = 0; i < kMaxConnections; ++i) {
        Connection& c = g_connections[i];
        if (c.used && c.local_port == dst_port && c.remote_ip == src_ip &&
            c.remote_port == net::ntoh16(header->src_port)) {
            found = &c;
            break;
        }
    }
    if (found == nullptr)
        return;
    Connection& c = *found;

    const u32 seq = net::ntoh32(header->seq);
    const u32 ack = net::ntoh32(header->ack);
    const u8  flags = header->flags;

    if ((flags & kRst) != 0) {
        c.state = State::Closed;
        return;
    }

    const usize header_len = (header->offset >> 4) * 4;
    if (header_len > length)
        return;
    const u8* data = segment + header_len;
    const u16 data_len = static_cast<u16>(length - header_len);

    switch (c.state) {
    case State::SynSent:
        if ((flags & kSyn) != 0 && (flags & kAck) != 0 && ack == c.snd_nxt + 1) {
            c.snd_nxt += 1;                     // our SYN consumed a sequence
            c.snd_una = c.snd_nxt;
            c.rcv_nxt = seq + 1;                // their SYN consumes one too
            c.state = State::Established;
            send_segment(c, kAck, nullptr, 0);  // completes the handshake
        }
        break;

    case State::Established:
    case State::FinWait1:
    case State::FinWait2:
        if ((flags & kAck) != 0 && ack > c.snd_una)
            c.snd_una = ack;

        // Only in-order data is accepted. Anything else is dropped and will be
        // retransmitted; queueing it would mean implementing reassembly, and a
        // half-built reassembler is worse than an honest retransmission.
        if (data_len > 0 && seq == c.rcv_nxt) {
            rx_push(c, data, data_len);
            c.rcv_nxt += data_len;
            send_segment(c, kAck, nullptr, 0);
        } else if (data_len > 0) {
            send_segment(c, kAck, nullptr, 0);  // tell them where we actually are
        }

        if ((flags & kFin) != 0 && seq + data_len == c.rcv_nxt) {
            c.rcv_nxt += 1;                     // a FIN consumes a sequence
            c.peer_finished = true;
            send_segment(c, kAck, nullptr, 0);
            if (c.state == State::Established)
                c.state = State::CloseWait;
            else
                c.state = State::TimeWait;
        } else if (c.state == State::FinWait1 && (flags & kAck) != 0 &&
                   c.snd_una == c.snd_nxt) {
            c.state = State::FinWait2;
        }
        break;

    case State::LastAck:
        if ((flags & kAck) != 0)
            c.state = State::Closed;
        break;

    default:
        break;
    }
}

int connect(u32 ip, u16 port)
{
    if (!net::available())
        return -1;

    int handle = -1;
    for (usize i = 0; i < kMaxConnections; ++i) {
        if (!g_connections[i].used) {
            handle = static_cast<int>(i);
            break;
        }
    }
    if (handle < 0)
        return -1;

    Connection& c = g_connections[handle];
    memset(&c, 0, sizeof(c));
    c.used        = true;
    c.state       = State::SynSent;
    c.remote_ip   = ip;
    c.remote_port = port;
    c.local_port  = g_next_port++;
    if (g_next_port == 0)
        g_next_port = 49152;
    // A fixed initial sequence number would be a security problem on a real
    // network; the tick counter is at least not a constant.
    c.snd_nxt = 0x1000 + static_cast<u32>(c.local_port) * 7919;
    c.snd_una = c.snd_nxt;

    // See net.cpp's arp_resolve for why the wait looks like this: nothing else
    // drains the NIC, so this loop has to, and the hlt is what lets the host
    // side actually deliver.
    scheduler::NoPreemption no_preempt;
    cpu::InterruptEnableGuard irq;

    for (int attempt = 0; attempt < 5; ++attempt) {
        if (!send_segment(c, kSyn, nullptr, 0))
            break;
        for (int i = 0; i < 400; ++i) {
            pump();
            if (c.state == State::Established)
                return handle;
            if (c.state == State::Closed)
                break;                          // refused
        }
        if (c.state == State::Closed)
            break;
    }

    c.used = false;
    return -1;
}

i64 send(int handle, const void* data, usize length)
{
    if (!valid(handle))
        return -1;
    Connection& c = g_connections[handle];
    if (c.state != State::Established && c.state != State::CloseWait)
        return -1;

    scheduler::NoPreemption no_preempt;
    cpu::InterruptEnableGuard irq;

    const auto* bytes = static_cast<const u8*>(data);
    usize sent = 0;
    while (sent < length) {
        const u16 chunk = static_cast<u16>(
            length - sent > kMss ? kMss : length - sent);

        // Stop and wait: send a chunk, then hold until it is acknowledged.
        // A real window would keep several in flight; this keeps the sequence
        // bookkeeping small enough to be obviously right.
        bool acknowledged = false;
        for (int attempt = 0; attempt < 5 && !acknowledged; ++attempt) {
            if (!send_segment(c, kPsh | kAck, bytes + sent, chunk))
                return sent > 0 ? static_cast<i64>(sent) : -1;
            const u32 expected = c.snd_nxt + chunk;
            for (int i = 0; i < 400; ++i) {
                pump();
                if (c.snd_una >= expected) {
                    acknowledged = true;
                    break;
                }
                if (c.state == State::Closed)
                    return sent > 0 ? static_cast<i64>(sent) : -1;
            }
        }
        if (!acknowledged)
            return sent > 0 ? static_cast<i64>(sent) : -1;

        c.snd_nxt += chunk;
        sent += chunk;
    }
    return static_cast<i64>(sent);
}

i64 recv(int handle, void* data, usize length)
{
    if (!valid(handle))
        return -1;
    Connection& c = g_connections[handle];

    scheduler::NoPreemption no_preempt;
    cpu::InterruptEnableGuard irq;

    // Wait for something to arrive, unless the peer has already finished and
    // the buffer is drained - which is end of stream, not an error.
    for (int i = 0; i < 4000 && rx_available(c) == 0; ++i) {
        if (c.peer_finished || c.state == State::Closed)
            break;
        pump();
    }

    const usize available = rx_available(c);
    if (available == 0)
        return (c.peer_finished || c.state == State::Closed) ? 0 : -1;

    const usize take = available < length ? available : length;
    auto* out = static_cast<u8*>(data);
    for (usize i = 0; i < take; ++i) {
        out[i] = c.rx[c.rx_head];
        c.rx_head = (c.rx_head + 1) % kRxBuffer;
    }
    return static_cast<i64>(take);
}

void close(int handle)
{
    if (!valid(handle))
        return;
    Connection& c = g_connections[handle];

    if (c.state == State::Established || c.state == State::CloseWait) {
        scheduler::NoPreemption no_preempt;
        cpu::InterruptEnableGuard irq;

        const bool they_finished = c.state == State::CloseWait;
        send_segment(c, kFin | kAck, nullptr, 0);
        c.snd_nxt += 1;                         // our FIN consumes a sequence
        c.state = they_finished ? State::LastAck : State::FinWait1;

        // Wait for the exchange to finish, but do not hang on a peer that never
        // answers: the connection is being torn down either way.
        for (int i = 0; i < 800; ++i) {
            pump();
            if (c.state == State::Closed || c.state == State::TimeWait)
                break;
        }
    }

    c.used = false;
    c.state = State::Closed;
}

State state(int handle)
{
    return valid(handle) ? g_connections[handle].state : State::Closed;
}

} // namespace tcp
