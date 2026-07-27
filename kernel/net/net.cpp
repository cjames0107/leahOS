#include <leah/console.hpp>
#include <leah/cpu.hpp>
#include <leah/e1000.hpp>
#include <leah/net.hpp>
#include <leah/scheduler.hpp>
#include <leah/string.hpp>

namespace net {
namespace {

constexpr u16 kEtherArp = 0x0806;
constexpr u16 kEtherIp  = 0x0800;

constexpr u16 kArpRequest = 1;
constexpr u16 kArpReply   = 2;

constexpr u8 kProtoIcmp = 1;
constexpr u8 kProtoUdp  = 17;

constexpr u8 kIcmpEchoReply   = 0;
constexpr u8 kIcmpEchoRequest = 8;

// QEMU's user-mode network runs a DNS proxy at 10.0.2.3 that forwards to the
// host resolver.
constexpr u32 kDnsIp   = 0x0A000203;
constexpr u16 kDnsPort = 53;

const u8 kBroadcast[kMacLength] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

struct [[gnu::packed]] EthHeader {
    u8  dst[kMacLength];
    u8  src[kMacLength];
    u16 ethertype;      // network order
};

struct [[gnu::packed]] ArpPacket {
    u16 htype;
    u16 ptype;
    u8  hlen;
    u8  plen;
    u16 oper;
    u8  sha[kMacLength];
    u32 spa;            // network order
    u8  tha[kMacLength];
    u32 tpa;            // network order
};

struct [[gnu::packed]] IpHeader {
    u8  ver_ihl;        // 0x45: IPv4, 5-word header
    u8  tos;
    u16 total_len;      // network order
    u16 id;
    u16 flags_frag;
    u8  ttl;
    u8  protocol;
    u16 checksum;
    u32 src;            // network order
    u32 dst;            // network order
};

struct [[gnu::packed]] IcmpHeader {
    u8  type;
    u8  code;
    u16 checksum;
    u16 id;             // network order
    u16 seq;            // network order
};

struct [[gnu::packed]] UdpHeader {
    u16 src_port;       // network order
    u16 dst_port;
    u16 length;
    u16 checksum;
};

// A tiny ARP cache. A handful of entries is plenty for a gateway plus a peer
// or two.
struct ArpEntry {
    u32 ip;
    u8  mac[kMacLength];
    bool valid;
};

constexpr usize kArpEntries = 8;
ArpEntry g_arp[kArpEntries];

bool g_up = false;

u16 g_ip_id = 0;

// State for the in-flight echo request the ping() loop is waiting on. Filled in
// by handle_icmp, which runs in the same poll loop, so no locking is needed.
u16  g_ping_id   = 0;
u16  g_ping_seq  = 0;
bool g_ping_seen = false;
u8   g_ping_ttl  = 0;

// State for the in-flight DNS query the resolve() loop is waiting on.
u16  g_dns_id       = 0;
u16  g_dns_src_port = 0;
bool g_dns_seen     = false;
u32  g_dns_result   = 0;

void cache_put(u32 ip, const u8* mac)
{
    for (usize i = 0; i < kArpEntries; ++i) {
        if (g_arp[i].valid && g_arp[i].ip == ip) {
            memcpy(g_arp[i].mac, mac, kMacLength);
            return;
        }
    }
    for (usize i = 0; i < kArpEntries; ++i) {
        if (!g_arp[i].valid) {
            g_arp[i].ip = ip;
            memcpy(g_arp[i].mac, mac, kMacLength);
            g_arp[i].valid = true;
            return;
        }
    }
    // Full: overwrite the first slot.
    g_arp[0].ip = ip;
    memcpy(g_arp[0].mac, mac, kMacLength);
}

// Fill an Ethernet header in `out` and return the offset where the payload
// starts.
usize build_eth(u8* out, const u8* dst_mac, u16 ethertype)
{
    auto* eth = reinterpret_cast<EthHeader*>(out);
    memcpy(eth->dst, dst_mac, kMacLength);
    memcpy(eth->src, e1000::mac(), kMacLength);
    eth->ethertype = hton16(ethertype);
    return sizeof(EthHeader);
}

void send_arp(u16 oper, const u8* target_mac, u32 target_ip)
{
    u8 frame[sizeof(EthHeader) + sizeof(ArpPacket)];
    const usize offset = build_eth(frame, target_mac, kEtherArp);

    auto* arp = reinterpret_cast<ArpPacket*>(frame + offset);
    arp->htype = hton16(1);             // Ethernet
    arp->ptype = hton16(kEtherIp);
    arp->hlen  = kMacLength;
    arp->plen  = 4;
    arp->oper  = hton16(oper);
    memcpy(arp->sha, e1000::mac(), kMacLength);
    arp->spa = hton32(kOurIp);
    memcpy(arp->tha, target_mac, kMacLength);
    arp->tpa = hton32(target_ip);

    e1000::send(frame, sizeof(frame));
}

void handle_arp(const ArpPacket* arp)
{
    const u32 sender_ip = ntoh32(arp->spa);
    cache_put(sender_ip, arp->sha);         // learn from any ARP traffic

    if (ntoh16(arp->oper) == kArpRequest && ntoh32(arp->tpa) == kOurIp)
        send_arp(kArpReply, arp->sha, sender_ip);
}

bool on_subnet(u32 ip) { return (ip & kNetmask) == (kOurIp & kNetmask); }

// The internet checksum (RFC 1071). Summing the buffer in memory order and
// storing the result straight back into the (network-order) checksum field is
// correct on any endianness; all our headers are an even number of bytes.
u16 checksum16(const void* data, usize length)
{
    const u16* word = static_cast<const u16*>(data);
    u32 sum = 0;
    while (length > 1) {
        sum += *word++;
        length -= 2;
    }
    if (length > 0)
        sum += *reinterpret_cast<const u8*>(word);
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return static_cast<u16>(~sum);
}

// Wrap `payload` in an IPv4 header and ship it, resolving the next hop (the
// destination itself when on-subnet, otherwise the gateway) to a MAC first.
bool send_ip(u32 dst_ip, u8 protocol, const void* payload, u16 payload_len)
{
    const u32 next_hop = on_subnet(dst_ip) ? dst_ip : kGatewayIp;
    u8 dst_mac[kMacLength];
    if (!arp_resolve(next_hop, dst_mac))
        return false;

    u8 frame[sizeof(EthHeader) + sizeof(IpHeader) + 1480];
    if (payload_len > sizeof(frame) - sizeof(EthHeader) - sizeof(IpHeader))
        return false;

    const usize offset = build_eth(frame, dst_mac, kEtherIp);
    auto* ip = reinterpret_cast<IpHeader*>(frame + offset);
    ip->ver_ihl    = 0x45;
    ip->tos        = 0;
    ip->total_len  = hton16(static_cast<u16>(sizeof(IpHeader) + payload_len));
    ip->id         = hton16(++g_ip_id);
    ip->flags_frag = hton16(0x4000);        // don't fragment
    ip->ttl        = 64;
    ip->protocol   = protocol;
    ip->checksum   = 0;
    ip->src        = hton32(kOurIp);
    ip->dst        = hton32(dst_ip);
    ip->checksum   = checksum16(ip, sizeof(IpHeader));

    memcpy(frame + offset + sizeof(IpHeader), payload, payload_len);
    return e1000::send(frame, static_cast<u16>(offset + sizeof(IpHeader) + payload_len));
}

void handle_icmp(u32 src_ip, u8 ttl, const u8* data, u16 length)
{
    if (length < sizeof(IcmpHeader))
        return;
    const auto* icmp = reinterpret_cast<const IcmpHeader*>(data);

    if (icmp->type == kIcmpEchoRequest) {
        // Echo it back: same payload, type flipped to reply, checksum redone.
        u8 reply[sizeof(IcmpHeader) + 64];
        if (length > sizeof(reply))
            return;
        memcpy(reply, data, length);
        auto* out = reinterpret_cast<IcmpHeader*>(reply);
        out->type     = kIcmpEchoReply;
        out->checksum = 0;
        out->checksum = checksum16(reply, length);
        send_ip(src_ip, kProtoIcmp, reply, length);
    } else if (icmp->type == kIcmpEchoReply) {
        if (ntoh16(icmp->id) == g_ping_id && ntoh16(icmp->seq) == g_ping_seq) {
            g_ping_ttl  = ttl;
            g_ping_seen = true;
        }
    }
}

// Send a UDP datagram. The checksum is left zero, which IPv4 explicitly permits
// ("not computed") and every receiver must accept - it saves carrying a
// pseudo-header here.
bool send_udp(u32 dst_ip, u16 src_port, u16 dst_port, const void* payload, u16 len)
{
    u8 datagram[sizeof(UdpHeader) + 512];
    if (len > sizeof(datagram) - sizeof(UdpHeader))
        return false;

    auto* udp = reinterpret_cast<UdpHeader*>(datagram);
    udp->src_port = hton16(src_port);
    udp->dst_port = hton16(dst_port);
    udp->length   = hton16(static_cast<u16>(sizeof(UdpHeader) + len));
    udp->checksum = 0;
    memcpy(datagram + sizeof(UdpHeader), payload, len);

    return send_ip(dst_ip, kProtoUdp, datagram, static_cast<u16>(sizeof(UdpHeader) + len));
}

// Encode "www.example.com" as DNS labels: 3www7example3com0. Returns the length
// written, or 0 if the name is malformed or too long.
usize dns_encode_name(const char* host, u8* out)
{
    usize pos = 0;
    const char* label = host;
    while (*label != '\0') {
        const char* dot = label;
        while (*dot != '\0' && *dot != '.')
            ++dot;
        const usize label_len = static_cast<usize>(dot - label);
        if (label_len == 0 || label_len > 63 || pos + 1 + label_len >= 254)
            return 0;
        out[pos++] = static_cast<u8>(label_len);
        for (usize i = 0; i < label_len; ++i)
            out[pos++] = static_cast<u8>(label[i]);
        label = (*dot == '.') ? dot + 1 : dot;
    }
    out[pos++] = 0;
    return pos;
}

// Step over a DNS name at `pos`, following the label lengths and stopping at a
// compression pointer (which terminates the name). Returns the offset just past
// the name, or 0 if it runs off the message.
usize dns_skip_name(const u8* msg, u16 len, usize pos)
{
    while (pos < len) {
        const u8 b = msg[pos];
        if (b == 0)
            return pos + 1;
        if ((b & 0xC0) == 0xC0)
            return pos + 2;                 // pointer: two bytes, name ends here
        pos += 1 + b;
    }
    return 0;
}

void handle_dns(const u8* msg, u16 len)
{
    if (len < 12)
        return;
    const u16 id = static_cast<u16>(msg[0] << 8 | msg[1]);
    if (id != g_dns_id)
        return;

    const u16 questions = static_cast<u16>(msg[4] << 8 | msg[5]);
    const u16 answers   = static_cast<u16>(msg[6] << 8 | msg[7]);

    usize pos = 12;
    for (u16 q = 0; q < questions; ++q) {
        pos = dns_skip_name(msg, len, pos);
        if (pos == 0 || pos + 4 > len)
            return;
        pos += 4;                           // QTYPE + QCLASS
    }

    for (u16 a = 0; a < answers; ++a) {
        pos = dns_skip_name(msg, len, pos);
        if (pos == 0 || pos + 10 > len)
            return;
        const u16 type   = static_cast<u16>(msg[pos] << 8 | msg[pos + 1]);
        const u16 rdlen  = static_cast<u16>(msg[pos + 8] << 8 | msg[pos + 9]);
        pos += 10;
        if (pos + rdlen > len)
            return;
        if (type == 1 && rdlen == 4) {      // an A record: the address we want
            g_dns_result = static_cast<u32>(msg[pos]) << 24 |
                           static_cast<u32>(msg[pos + 1]) << 16 |
                           static_cast<u32>(msg[pos + 2]) << 8 |
                           static_cast<u32>(msg[pos + 3]);
            g_dns_seen = true;
            return;
        }
        pos += rdlen;
    }
    g_dns_seen = true;                      // a reply arrived, just no A record
}

void handle_udp(u32 src_ip, const u8* data, u16 length)
{
    if (length < sizeof(UdpHeader))
        return;
    const auto* udp = reinterpret_cast<const UdpHeader*>(data);
    if (src_ip == kDnsIp && ntoh16(udp->dst_port) == g_dns_src_port)
        handle_dns(data + sizeof(UdpHeader),
                   static_cast<u16>(length - sizeof(UdpHeader)));
}

void handle_ip(const u8* frame, u16 length)
{
    if (length < sizeof(EthHeader) + sizeof(IpHeader))
        return;
    const auto* ip = reinterpret_cast<const IpHeader*>(frame + sizeof(EthHeader));
    if ((ip->ver_ihl >> 4) != 4)
        return;
    if (ntoh32(ip->dst) != kOurIp)
        return;                             // not addressed to us

    const usize header_len = (ip->ver_ihl & 0x0F) * 4;
    const u16 total = ntoh16(ip->total_len);
    if (header_len < sizeof(IpHeader) || total < header_len ||
        sizeof(EthHeader) + total > length)
        return;

    const u8* l4 = frame + sizeof(EthHeader) + header_len;
    const u16 l4_len = static_cast<u16>(total - header_len);

    if (ip->protocol == kProtoIcmp)
        handle_icmp(ntoh32(ip->src), ip->ttl, l4, l4_len);
    else if (ip->protocol == kProtoUdp)
        handle_udp(ntoh32(ip->src), l4, l4_len);
}

// Delivered by the NIC's poll loop for every received frame.
void receive(const u8* frame, u16 length)
{
    if (length < sizeof(EthHeader))
        return;
    const auto* eth = reinterpret_cast<const EthHeader*>(frame);
    const u16 ethertype = ntoh16(eth->ethertype);

    if (ethertype == kEtherArp && length >= sizeof(EthHeader) + sizeof(ArpPacket))
        handle_arp(reinterpret_cast<const ArpPacket*>(frame + sizeof(EthHeader)));
    else if (ethertype == kEtherIp)
        handle_ip(frame, length);
}

void print_ip(u32 ip)
{
    console::printf("%u.%u.%u.%u",
                    ip >> 24 & 0xFF, ip >> 16 & 0xFF, ip >> 8 & 0xFF, ip & 0xFF);
}

} // namespace

bool init()
{
    memset(g_arp, 0, sizeof(g_arp));
    if (!e1000::init())
        return false;
    e1000::set_receiver(receive);
    g_up = true;
    return true;
}

bool available() { return g_up; }

const u8* mac() { return e1000::mac(); }

u32 our_ip() { return kOurIp; }
u32 gateway_ip() { return kGatewayIp; }
u32 netmask() { return kNetmask; }

void info(Info& out)
{
    out.ip      = kOurIp;
    out.gateway = kGatewayIp;
    out.netmask = kNetmask;
    memcpy(out.mac, e1000::mac(), kMacLength);
}

bool arp_lookup(u32 ip, u8* mac_out)
{
    for (usize i = 0; i < kArpEntries; ++i) {
        if (g_arp[i].valid && g_arp[i].ip == ip) {
            memcpy(mac_out, g_arp[i].mac, kMacLength);
            return true;
        }
    }
    return false;
}

bool arp_resolve(u32 ip, u8* mac_out)
{
    if (arp_lookup(ip, mac_out))
        return true;

    // Hold the poll loop on this CPU: nothing else drains the NIC, so being
    // scheduled away mid-wait would strand the reply in the ring. Interrupts are
    // enabled so the timer can wake the hlt (a syscall enters with them masked).
    scheduler::NoPreemption no_preempt;
    cpu::InterruptEnableGuard irq;

    // Request, then poll the NIC's receive ring for the reply. Each hlt yields
    // to QEMU's host-side network backend (a busy-poll starves it and nothing is
    // ever delivered); the reply then lands in the ring for the next poll. The
    // budget is counted in poll iterations rather than timer ticks because the
    // virtual timer fast-forwards across a hlt, so a tick-based deadline can
    // expire before the reply arrives in real time. Re-send periodically in case
    // an early request was dropped.
    constexpr int kIterations = 2000;
    for (int i = 0; i < kIterations; ++i) {
        if (i % 100 == 0)
            send_arp(kArpRequest, kBroadcast, ip);
        e1000::poll();
        if (arp_lookup(ip, mac_out))
            return true;
        asm volatile("hlt");                             // yield to the host
    }
    return false;
}

void arp_print_cache()
{
    for (usize i = 0; i < kArpEntries; ++i) {
        if (!g_arp[i].valid)
            continue;
        console::write("    ");
        print_ip(g_arp[i].ip);
        console::printf(" is at %02x:%02x:%02x:%02x:%02x:%02x\n",
                        g_arp[i].mac[0], g_arp[i].mac[1], g_arp[i].mac[2],
                        g_arp[i].mac[3], g_arp[i].mac[4], g_arp[i].mac[5]);
    }
}

bool ping(u32 dst, u16 seq, u8* ttl_out)
{
    u8 packet[sizeof(IcmpHeader) + 32];
    auto* icmp = reinterpret_cast<IcmpHeader*>(packet);
    icmp->type     = kIcmpEchoRequest;
    icmp->code     = 0;
    icmp->checksum = 0;
    icmp->id       = hton16(0x4C4F);        // "LO", any fixed identifier
    icmp->seq      = hton16(seq);
    for (usize i = 0; i < 32; ++i)
        packet[sizeof(IcmpHeader) + i] = static_cast<u8>('a' + i % 26);
    icmp->checksum = checksum16(packet, sizeof(packet));

    g_ping_id   = 0x4C4F;
    g_ping_seq  = seq;
    g_ping_seen = false;

    if (!send_ip(dst, kProtoIcmp, packet, sizeof(packet)))
        return false;

    scheduler::NoPreemption no_preempt;
    cpu::InterruptEnableGuard irq;
    // Poll for the reply, yielding to the host between reads (see arp_resolve).
    for (int i = 0; i < 2000; ++i) {
        e1000::poll();
        if (g_ping_seen) {
            if (ttl_out != nullptr)
                *ttl_out = g_ping_ttl;
            return true;
        }
        asm volatile("hlt");
    }
    return false;
}

bool resolve(const char* host, u32* out_ip)
{
    u8 query[300];
    query[0] = 0x4C; query[1] = 0x4F;       // transaction id
    query[2] = 0x01; query[3] = 0x00;       // flags: recursion desired
    query[4] = 0x00; query[5] = 0x01;       // one question
    query[6] = query[7] = query[8] = query[9] = query[10] = query[11] = 0;

    const usize name_len = dns_encode_name(host, query + 12);
    if (name_len == 0)
        return false;
    usize pos = 12 + name_len;
    query[pos++] = 0x00; query[pos++] = 0x01;   // QTYPE  = A
    query[pos++] = 0x00; query[pos++] = 0x01;   // QCLASS = IN

    g_dns_id       = 0x4C4F;
    g_dns_src_port = 0xC000;                 // any ephemeral port
    g_dns_seen     = false;
    g_dns_result   = 0;

    scheduler::NoPreemption no_preempt;
    cpu::InterruptEnableGuard irq;
    // Poll for the reply as elsewhere (see arp_resolve), resending periodically
    // in case a datagram is lost. On success this returns as soon as the reply
    // lands; the budget only bounds how long an unanswered query waits.
    for (int i = 0; i < 250; ++i) {
        if (i % 80 == 0) {
            if (!send_udp(kDnsIp, g_dns_src_port, kDnsPort, query,
                          static_cast<u16>(pos)))
                return false;
        }
        e1000::poll();
        if (g_dns_seen) {
            if (g_dns_result == 0)
                return false;               // reply held no A record
            if (out_ip != nullptr)
                *out_ip = g_dns_result;
            return true;
        }
        asm volatile("hlt");
    }
    return false;
}

} // namespace net
