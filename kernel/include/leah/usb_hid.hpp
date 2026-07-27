#pragma once

#include <leah/types.hpp>

// USB HID, boot protocol only.
//
// A HID device describes its reports with a report descriptor, which is a small
// bytecode that has to be parsed to know what any given bit means. The boot
// protocol exists precisely to avoid that: a keyboard in boot mode always sends
// the same eight bytes - modifiers, a reserved byte, and up to six held keys -
// which is what a BIOS relies on and is all a console needs.

namespace usb::hid {

// Claim any HID keyboard the xHCI driver enumerated and put it in boot
// protocol. Returns how many were brought up.
usize init();

usize keyboard_count();

// Poll for a report and feed anything newly pressed into the keyboard buffer.
// Called from the same place the console read path polls, since USB is not
// interrupt-driven here.
void poll();

} // namespace usb::hid
