#include <leah/keyboard.hpp>
#include <leah/string.hpp>
#include <leah/usb_hid.hpp>
#include <leah/xhci.hpp>

namespace usb::hid {
namespace {

constexpr usize kMaxKeyboards = 2;

struct Keyboard {
    u8 slot;
    u8 endpoint;
    u8 previous[6];         // keys held in the last report, to find new presses
};

Keyboard g_keyboards[kMaxKeyboards];
usize    g_keyboard_count = 0;

// HID usage codes for the boot keyboard, unshifted and shifted. The table is
// indexed by usage id, which starts at 4 for 'a' - the layout is the standard
// one every boot-protocol keyboard reports, whatever the physical keys say.
constexpr char kUnshifted[] =
    "\0\0\0\0" "abcdefghijklmnopqrstuvwxyz" "1234567890" "\n\x1b\b\t "
    "-=[]\\\0;'`,./";
constexpr char kShifted[] =
    "\0\0\0\0" "ABCDEFGHIJKLMNOPQRSTUVWXYZ" "!@#$%^&*()" "\n\x1b\b\t "
    "_+{}|\0:\"~<>?";

char translate(u8 usage, bool shift)
{
    const char* table = shift ? kShifted : kUnshifted;
    const usize length = shift ? sizeof(kShifted) : sizeof(kUnshifted);
    if (usage >= length - 1)
        return 0;
    return table[usage];
}

} // namespace

usize init()
{
    for (usize i = 0; i < xhci::device_count() && g_keyboard_count < kMaxKeyboards;
         ++i) {
        const xhci::Device& device = xhci::device_at(i);
        // Class 3 is HID; subclass 1 means it supports the boot protocol, and
        // protocol 1 is specifically a keyboard.
        if (device.device_class != 0x03 || device.device_subclass != 0x01 ||
            device.device_protocol != 0x01)
            continue;
        if (device.interrupt_in == 0)
            continue;

        // SET_PROTOCOL(boot) and SET_IDLE(0). Both are class requests to the
        // interface: 0x21 is host-to-device, class, interface.
        xhci::control_transfer(device.slot, 0x21, 0x0B, 0, 0, nullptr, 0);
        xhci::control_transfer(device.slot, 0x21, 0x0A, 0, 0, nullptr, 0);

        Keyboard& keyboard = g_keyboards[g_keyboard_count++];
        keyboard.slot     = device.slot;
        keyboard.endpoint = device.interrupt_in;
        memset(keyboard.previous, 0, sizeof(keyboard.previous));

        // Get the first read in flight so there is something to collect.
        xhci::submit_interrupt(keyboard.slot, keyboard.endpoint, 8);
    }
    return g_keyboard_count;
}

usize keyboard_count() { return g_keyboard_count; }

void poll()
{
    for (usize i = 0; i < g_keyboard_count; ++i) {
        Keyboard& keyboard = g_keyboards[i];

        // Post a read if none is outstanding, then see whether an earlier one
        // has landed. Never blocks: a keyboard with no key pressed simply has
        // nothing to report, and waiting for one would hang the console.
        u8 report[8] = {};
        const i64 got = xhci::take_interrupt(keyboard.slot, report, sizeof(report));
        if (got <= 0) {
            if (got < 0)
                xhci::submit_interrupt(keyboard.slot, keyboard.endpoint, 8);
            continue;
        }
        xhci::submit_interrupt(keyboard.slot, keyboard.endpoint, 8);

        // Byte 0 is the modifier bitmap; either shift key is bits 1 and 5.
        const bool shift = (report[0] & 0x22) != 0;

        // Bytes 2-7 are the keys currently held, in no particular order. A key
        // is newly pressed if it was not in the previous report - which is what
        // turns "held" into "typed" without repeating on every poll.
        for (usize k = 0; k < 6; ++k) {
            const u8 usage = report[2 + k];
            if (usage == 0)
                continue;
            bool was_held = false;
            for (usize p = 0; p < 6; ++p) {
                if (keyboard.previous[p] == usage)
                    was_held = true;
            }
            if (!was_held)
                keyboard::inject_char(translate(usage, shift));
        }
        memcpy(keyboard.previous, report + 2, 6);
    }
}

} // namespace usb::hid
