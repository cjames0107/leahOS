#include <leah/keyboard.hpp>
#include <leah/scheduler.hpp>

// The queue a blocked reader sleeps on - and nothing that produces keys.
//
// Scancode set 1, the modifier tracking, the 8042 and its initialisation
// sequence all belong to ps2d now, in ring 3, beside the mouse that shares the
// same controller. usbd already fed this queue from ring 3; there are simply
// two producers now and no privileged one.
//
// What cannot leave is this: a task blocked reading the console is asleep on a
// channel, and waking it is scheduling. That is the whole of what the kernel
// keeps of a keyboard.

namespace keyboard {
namespace {

constexpr u32 kBufferSize = 256;

char g_buffer[kBufferSize];
volatile u32 g_read = 0;
volatile u32 g_write = 0;

// Published by whichever driver saw the key, so that "is shift held" has one
// answer whoever is asking and whatever is plugged in.
volatile u32 g_mods = 0;

} // namespace

void init() {}

u32 modifiers() { return g_mods; }

void set_usb_modifiers(u32 mods) { g_mods = mods; }

void inject_char(char c)
{
    if (c == 0)
        return;
    const u32 next = (g_write + 1) % kBufferSize;
    if (next == g_read)
        return;                 // full; drop rather than overwrite unread input
    g_buffer[g_write] = c;
    g_write = next;
    scheduler::wake(scheduler::kKeyboardChannel);
}

bool has_input() { return g_read != g_write; }

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
        scheduler::block_on(scheduler::kKeyboardChannel);
    return read();
}

} // namespace keyboard
