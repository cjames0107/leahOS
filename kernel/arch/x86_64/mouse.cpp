#include <leah/framebuffer.hpp>
#include <leah/mouse.hpp>

// Where the pointer is - and nothing about how it got there.
//
// The PS/2 controller, its initialisation sequence and its three-byte packets
// are ps2d's now, in ring 3, along with the keyboard that shares the same
// 8042. What stays is the position itself, because InputPoll answers from it
// and the window server should not have to know which driver is moving the
// pointer this week - a USB mouse would report through the same call.
//
// The clamp stays too. The bound is the framebuffer's size, the framebuffer is
// still the kernel's, and a pointer that accumulates past the edge has to be
// dragged all the way back before it appears to move again.

namespace mouse {
namespace {

State g_state{};

void clamp(i32& x, i32& y)
{
    if (!framebuffer::available())
        return;                         // nothing to clamp against yet
    const i32 max_x = static_cast<i32>(framebuffer::width()) - 1;
    const i32 max_y = static_cast<i32>(framebuffer::height()) - 1;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > max_x) x = max_x;
    if (y > max_y) y = max_y;
}

} // namespace

void init() {}

State state() { return g_state; }

void set_state(i32 x, i32 y, u32 buttons)
{
    clamp(x, y);
    g_state.x = x;
    g_state.y = y;
    g_state.left   = (buttons & 1) != 0;
    g_state.right  = (buttons & 2) != 0;
    g_state.middle = (buttons & 4) != 0;
}

} // namespace mouse
