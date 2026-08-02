#include <leah/cpu.hpp>
#include <leah/interrupts.hpp>
#include <leah/io.hpp>
#include <leah/keyboard.hpp>
#include <leah/pic.hpp>
#include <leah/scheduler.hpp>

namespace keyboard {
namespace {

constexpr u16 kDataPort    = 0x60;
constexpr u16 kStatusPort  = 0x64;   // read
constexpr u16 kCommandPort = 0x64;   // write

constexpr u8 kStatusOutputFull = 0x01;   // data waiting for us
constexpr u8 kStatusInputFull  = 0x02;   // controller has not consumed our byte

// 8042 controller commands
constexpr u8 kCmdReadConfig      = 0x20;
constexpr u8 kCmdWriteConfig     = 0x60;
constexpr u8 kCmdDisablePort2    = 0xA7;
constexpr u8 kCmdDisablePort1    = 0xAD;
constexpr u8 kCmdEnablePort1     = 0xAE;

// Configuration byte bits
constexpr u8 kConfigPort1Interrupt = 1 << 0;
constexpr u8 kConfigPort1Clock     = 1 << 4;   // set = clock disabled
constexpr u8 kConfigTranslation    = 1 << 6;

// Keyboard (device, not controller) commands
constexpr u8 kKbdEnableScanning = 0xF4;
constexpr u8 kKbdAck            = 0xFA;

// The 8042 is slow enough that we have to wait on it, and broken enough on
// some machines that we must not wait forever.
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

void send_command(u8 command)
{
    wait_writable();
    io::out8(kCommandPort, command);
}

void send_data(u8 value)
{
    wait_writable();
    io::out8(kDataPort, value);
}

u8 receive_data()
{
    if (!wait_readable())
        return 0;
    return io::in8(kDataPort);
}

void flush_output()
{
    // Bounded, because an absent controller reads back 0xFF on every port and
    // 0xFF has the output-full bit set - so an unbounded drain never finishes
    // on a machine that has no 8042 at all. There is nothing to drain in that
    // case, which is exactly the case that used to hang here.
    for (u32 i = 0; i < kPollLimit; ++i) {
        if ((io::in8(kStatusPort) & kStatusOutputFull) == 0)
            return;
        (void)io::in8(kDataPort);
    }
}

// A machine with no PS/2 controller floats its ports high, so every read is
// 0xFF. That is not a status any real 8042 reports - it would mean every bit
// set at once, including the two error bits - so it is a reliable way to tell
// "no controller" from "controller busy".
bool controller_present()
{
    return io::in8(kStatusPort) != 0xFF;
}

bool g_present = true;

// Scancode set 1. A key press sends the code below; the matching release sends
// the same value with bit 7 set, which is how we track modifier state.
constexpr u8 kReleaseFlag = 0x80;

constexpr u8 kScancodeLeftShift  = 0x2A;
constexpr u8 kScancodeRightShift = 0x36;
constexpr u8 kScancodeCtrl       = 0x1D;
constexpr u8 kScancodeCapsLock   = 0x3A;

constexpr char kUnshifted[128] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,   'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,   '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
    0,   '*', 0,   ' ',
};

constexpr char kShifted[128] = {
    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,   'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0,   '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
    0,   '*', 0,   ' ',
};

bool g_shift = false;
bool g_ctrl  = false;
// The arrows and the other grey keys arrive as 0xE0 then an ordinary code, so
// the byte after the prefix has to be read in a different table from the one a
// plain scancode uses - 0x48 is 'up' after a prefix and the numeric keypad's 8
// without it.
bool g_extended = false;

// Delivered as control characters in the range the Ctrl+letter mapping does not
// use, so they travel through read() and the window protocol without needing a
// wider key type. Mirrored in user/libc/include/wproto.h.
constexpr char kUp    = 0x1C;
constexpr char kDown  = 0x1D;
constexpr char kLeft  = 0x1E;
constexpr char kRight = 0x1F;
bool g_caps  = false;

// Power-of-two size so the wrap is a mask rather than a modulo.
constexpr u32 kBufferSize = 256;
volatile char g_buffer[kBufferSize]{};
volatile u32  g_write = 0;
volatile u32  g_read  = 0;
// What a USB keyboard last reported holding. Kept apart from g_shift/g_ctrl so
// releasing a key on one keyboard cannot clear what is held on the other.
volatile u32  g_usb_mods = 0;

void push(char c)
{
    const u32 next = (g_write + 1) % kBufferSize;
    if (next == g_read)
        return;                 // full; drop rather than overwrite unread input
    g_buffer[g_write] = c;
    g_write = next;
}

// Split from the IRQ handler deliberately: everything here is a pure function
// of the scancode and the modifier state, so it can be exercised without an
// 8042 in the picture.
void handle_scancode(u8 scancode)
{
    /* The arrows and the other grey keys arrive as 0xE0 then an ordinary code.
     * The byte after the prefix means something different from the same byte
     * without one - 0x48 is 'up' after a prefix and the keypad's 8 without -
     * so it has to be decoded here rather than falling into the tables below. */
    if (scancode == 0xE0) {
        g_extended = true;
        return;
    }
    if (g_extended) {
        g_extended = false;
        if ((scancode & kReleaseFlag) == 0) {
            char c = 0;
            switch (scancode) {
            case 0x48: c = kUp;    break;
            case 0x50: c = kDown;  break;
            case 0x4B: c = kLeft;  break;
            case 0x4D: c = kRight; break;
            default: break;
            }
            if (c != 0) {
                push(c);
                scheduler::wake(scheduler::kKeyboardChannel);
            }
        }
        return;
    }

    if (scancode & kReleaseFlag) {
        const u8 released = static_cast<u8>(scancode & ~kReleaseFlag);
        if (released == kScancodeLeftShift || released == kScancodeRightShift)
            g_shift = false;
        else if (released == kScancodeCtrl)
            g_ctrl = false;
        return;
    }

    switch (scancode) {
    case kScancodeLeftShift:
    case kScancodeRightShift:
        g_shift = true;
        return;
    case kScancodeCtrl:
        g_ctrl = true;
        return;
    case kScancodeCapsLock:
        g_caps = !g_caps;
        return;
    default:
        break;
    }

    if (scancode >= 128)
        return;

    char c = g_shift ? kShifted[scancode] : kUnshifted[scancode];
    if (c == 0)
        return;

    // Caps lock affects letters only, and inverts rather than overrides shift.
    if (g_caps) {
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
        else if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }

    if (g_ctrl && c >= 'a' && c <= 'z')
        c = static_cast<char>(c - 'a' + 1);      // ^A..^Z
    else if (g_ctrl && c >= 'A' && c <= 'Z')
        c = static_cast<char>(c - 'A' + 1);

    push(c);

    // Wake any task blocked in read() on the console.
    scheduler::wake(scheduler::kKeyboardChannel);
}

void on_key(interrupts::Frame&)
{
    handle_scancode(io::in8(kDataPort));
}

} // namespace

void inject_char(char c)
{
    if (c == 0)
        return;
    push(c);
    scheduler::wake(scheduler::kKeyboardChannel);
}

void inject_scancode(u8 scancode)
{
    handle_scancode(scancode);
}

void init()
{
    // A USB-only machine has no 8042 to configure, and every byte written here
    // would go nowhere. Say so and leave; usbd brings up the keyboard on those
    // machines, and the queue below is fed the same either way.
    if (!controller_present()) {
        g_present = false;
        return;
    }

    // Do not assume the firmware left the controller in a usable state. It is
    // entitled to hand over with scanning disabled, and then no amount of
    // typing produces a single scancode - the bytes are dropped inside the
    // device, long before the PIC or the IDT get a say.

    // Quiet both ports while we reconfigure, so nothing arrives mid-sequence
    // and gets mistaken for a command response.
    send_command(kCmdDisablePort1);
    send_command(kCmdDisablePort2);
    flush_output();

    send_command(kCmdReadConfig);
    u8 config = receive_data();

    config |= kConfigPort1Interrupt;    // actually raise IRQ 1
    config &= ~kConfigPort1Clock;       // ungate the port's clock
    // Translation makes the controller convert the set 2 codes the hardware
    // really sends into the set 1 codes our tables are written against.
    // Turning it off would silently reinterpret every key.
    config |= kConfigTranslation;

    send_command(kCmdWriteConfig);
    send_data(config);

    send_command(kCmdEnablePort1);

    // Finally tell the keyboard itself to start reporting.
    send_data(kKbdEnableScanning);
    const u8 ack = receive_data();
    if (ack != kKbdAck)
        flush_output();                 // not fatal; some emulated 8042s stay quiet

    // The mouse port is left disabled: IRQ 12 has no handler yet.

    interrupts::register_irq(interrupts::kIrqKeyboard, on_key);
    pic::unmask(interrupts::kIrqKeyboard);
}

bool present() { return g_present; }

bool has_input()
{
    return g_read != g_write;
}

char read()
{
    if (g_read == g_write)
        return 0;
    const char c = g_buffer[g_read];
    g_read = (g_read + 1) % kBufferSize;
    return c;
}

char read_blocking()
{
    while (!has_input())
        cpu::wait_for_interrupt();
    return read();
}

/* The live modifier state, for anything that needs to know what was held while
 * something else happened - a click, not a keystroke. A keystroke already
 * carries its modifiers in the character it produced; a mouse press cannot. */
u32 modifiers()
{
    return (g_shift ? kModShift : 0u) | (g_ctrl ? kModCtrl : 0u) | g_usb_mods;
}

void set_usb_modifiers(u32 mods)
{
    g_usb_mods = mods & (kModShift | kModCtrl);
}

} // namespace keyboard
