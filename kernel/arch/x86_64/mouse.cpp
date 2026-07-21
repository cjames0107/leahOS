#include <leah/interrupts.hpp>
#include <leah/io.hpp>
#include <leah/mouse.hpp>
#include <leah/pic.hpp>

namespace mouse {
namespace {

constexpr u16 kDataPort   = 0x60;
constexpr u16 kStatusPort = 0x64;
constexpr u16 kCommandPort = 0x64;

constexpr u8 kStatusOutputFull = 0x01;
constexpr u8 kStatusInputFull  = 0x02;
// The controller sets this on bytes that came from the second port, which is
// how a mouse packet is told apart from a keystroke on the shared data port.
constexpr u8 kStatusFromMouse  = 0x20;

constexpr u8 kCmdEnablePort2      = 0xA8;
constexpr u8 kCmdReadConfig       = 0x20;
constexpr u8 kCmdWriteConfig      = 0x60;
constexpr u8 kCmdWriteToPort2     = 0xD4;

constexpr u8 kConfigPort2Interrupt = 1 << 1;
constexpr u8 kConfigPort2Clock     = 1 << 5;   // set = clock disabled

constexpr u8 kMouseSetDefaults  = 0xF6;
constexpr u8 kMouseEnableReport = 0xF4;
constexpr u8 kMouseAck          = 0xFA;

constexpr u32 kPollLimit = 100000;

bool wait_writable()
{
    for (u32 i = 0; i < kPollLimit; ++i) {
        if ((io::in8(kStatusPort) & kStatusInputFull) == 0)
            return true;
        io::wait();
    }
    return false;
}

bool wait_readable()
{
    for (u32 i = 0; i < kPollLimit; ++i) {
        if ((io::in8(kStatusPort) & kStatusOutputFull) != 0)
            return true;
        io::wait();
    }
    return false;
}

u8 read_data()
{
    if (!wait_readable())
        return 0;
    return io::in8(kDataPort);
}

// Anything destined for the mouse has to be prefixed with 0xD4, otherwise the
// controller treats it as a keyboard command.
u8 send_to_mouse(u8 value)
{
    wait_writable();
    io::out8(kCommandPort, kCmdWriteToPort2);
    wait_writable();
    io::out8(kDataPort, value);
    return read_data();
}

State g_state{};
u64   g_packets = 0;

u8 g_packet[3]{};
u8 g_index = 0;

void on_packet(interrupts::Frame&)
{
    const u8 status = io::in8(kStatusPort);
    if ((status & kStatusOutputFull) == 0 || (status & kStatusFromMouse) == 0)
        return;                         // not ours

    const u8 byte = io::in8(kDataPort);

    // Byte 0 always has bit 3 set. If it is clear we are out of phase - drop
    // the byte rather than decoding garbage into cursor movement.
    if (g_index == 0 && (byte & 0x08) == 0)
        return;

    g_packet[g_index++] = byte;
    if (g_index < 3)
        return;
    g_index = 0;

    const u8 flags = g_packet[0];

    // Overflow means the counters saturated; the deltas are meaningless.
    if ((flags & 0xC0) != 0)
        return;

    // 9-bit two's complement: the sign lives in the flags byte.
    i32 dx = g_packet[1];
    i32 dy = g_packet[2];
    if (flags & 0x10)
        dx |= ~0xFF;
    if (flags & 0x20)
        dy |= ~0xFF;

    g_state.x += dx;
    g_state.y -= dy;            // the mouse reports up as positive; screens do not
    g_state.left   = (flags & 0x01) != 0;
    g_state.right  = (flags & 0x02) != 0;
    g_state.middle = (flags & 0x04) != 0;

    ++g_packets;
}

} // namespace

void init()
{
    wait_writable();
    io::out8(kCommandPort, kCmdEnablePort2);

    wait_writable();
    io::out8(kCommandPort, kCmdReadConfig);
    u8 config = read_data();

    config |= kConfigPort2Interrupt;    // raise IRQ 12
    config &= ~kConfigPort2Clock;       // ungate the port

    wait_writable();
    io::out8(kCommandPort, kCmdWriteConfig);
    wait_writable();
    io::out8(kDataPort, config);

    send_to_mouse(kMouseSetDefaults);
    send_to_mouse(kMouseEnableReport);

    g_index = 0;
    interrupts::register_irq(12, on_packet);

    // IRQ 12 lives on the slave PIC, which is invisible to the CPU unless the
    // cascade line is open too - pic::unmask handles that.
    pic::unmask(12);
}

State state() { return g_state; }
u64 packet_count() { return g_packets; }

} // namespace mouse
