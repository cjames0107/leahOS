#pragma once

#include <leah/types.hpp>

// A small IPv4 stack over the e1000: Ethernet framing, ARP, and (added in
// layers above) IP, ICMP and UDP. Addresses are held in host order and
// byte-swapped only at the wire.

namespace net {

constexpr usize kMacLength = 6;

// QEMU's user-mode network: we are 10.0.2.15, the gateway and DNS are
// 10.0.2.2 and 10.0.2.3, the mask is /24.
constexpr u32 kOurIp     = 0x0A00020F;   // 10.0.2.15
constexpr u32 kGatewayIp = 0x0A000202;   // 10.0.2.2
constexpr u32 kNetmask   = 0xFFFFFF00;

inline u16 hton16(u16 v) { return static_cast<u16>(v << 8 | v >> 8); }
inline u16 ntoh16(u16 v) { return hton16(v); }
inline u32 hton32(u32 v)
{
    return (v & 0xFF) << 24 | (v & 0xFF00) << 8 | (v >> 8 & 0xFF00) | v >> 24;
}
inline u32 ntoh32(u32 v) { return hton32(v); }

// Bring up the NIC and the stack. Returns false if there is no NIC.
bool init();
bool available();

const u8* mac();
u32 our_ip();
u32 gateway_ip();
u32 netmask();

// What the netinfo syscall hands back to userland; the layout is shared with
// user/libc/include/net.h and the two must change together.
struct [[gnu::packed]] Info {
    u32 ip;
    u32 gateway;
    u32 netmask;
    u8  mac[kMacLength];
};

void info(Info& out);

// --- ARP --------------------------------------------------------------------

// Resolve an IPv4 address to a MAC, sending a request and waiting up to a short
// timeout for the reply. Returns false if it stays unresolved.
bool arp_resolve(u32 ip, u8* mac_out);

// Non-blocking cache lookup.
bool arp_lookup(u32 ip, u8* mac_out);

// The one-line "who has ... / it is at ..." trace, printed to the console for
// the boot self-test and the arp command.
void arp_print_cache();

// --- ICMP -------------------------------------------------------------------

// Send one ICMP echo request to `dst` and wait for the matching reply, running
// the NIC poll loop internally. Returns true on a reply, writing its IP TTL to
// ttl_out. Off-subnet destinations are routed via the gateway.
bool ping(u32 dst, u16 seq, u8* ttl_out);

// --- DNS --------------------------------------------------------------------

// Resolve a hostname to an IPv4 address via QEMU's DNS proxy, running the poll
// loop internally. Returns true and writes the address (host order) to out_ip
// on success.
bool resolve(const char* host, u32* out_ip);

} // namespace net
