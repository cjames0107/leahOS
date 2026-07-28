#pragma once

#include <leah/types.hpp>

// The window server: it owns the screen, and everything else asks it for a
// rectangle to draw in.
//
// This lives in the kernel rather than in a userland process, which is not how
// a mature system would do it - but a userland server needs shared memory and a
// message transport between processes, and there is neither yet. The split that
// matters is still here: a client never touches the framebuffer, only its own
// window's pixels, and it cannot see or draw over anyone else's.
//
// The look is deliberately of its period - grey chrome, bevelled edges, a title
// bar and a close box - because that style is what a 16-colour-ish palette and a
// bitmap font are actually good at.

namespace gui {

constexpr usize kMaxWindows = 16;
constexpr usize kTitleLength = 32;

enum class EventType : u32 {
    None = 0,
    MouseDown,
    MouseUp,
    MouseMove,
    Key,
    Close,          // the close box was clicked; the client should exit
};

// Delivered to a client. Coordinates are relative to the window's content area,
// so a client never has to know where on screen it happens to be.
struct [[gnu::packed]] Event {
    u32 type;
    u32 window;
    i32 x;
    i32 y;
    u32 button;     // 1 left, 2 right
    u32 key;        // the character, for Key events
};

// Bring the server up and start compositing. False when there is no linear
// framebuffer to draw on.
bool init();
bool active();

// Tear the desktop down and give the screen back to the text console. Happens
// on its own when the last window closes.
void shutdown();

// --- the client side --------------------------------------------------------

// Open a window. Returns its id, or -1. The size is the *content* area; the
// server adds the frame around it.
i32 create_window(i32 x, i32 y, u32 width, u32 height, const char* title);

void destroy_window(i32 id);

// Close everything a thread group owns. Called when a process exits, so a
// client that was killed rather than closed does not leave a window on the
// desktop with nobody behind it.
void close_windows_of(u32 tgid);

// The window's pixel buffer, as the kernel sees it: `width * height` packed
// 32-bit pixels. The syscall layer maps these frames into the client.
u32* buffer_of(i32 id);
paddr_t buffer_phys(i32 id);
u32 buffer_bytes(i32 id);

// Tell the server the contents changed and should be shown.
void present(i32 id);

// Take the next event for a window, or return false when there is none.
bool poll_event(i32 id, Event& out);

// Which process owns a window, so a client cannot present or destroy another's.
u32 owner_of(i32 id);

} // namespace gui
