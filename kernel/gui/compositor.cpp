#include <leah/console.hpp>
#include <leah/framebuffer.hpp>
#include <leah/gui.hpp>
#include <leah/heap.hpp>
#include <leah/keyboard.hpp>
#include <leah/memory.hpp>
#include <leah/mouse.hpp>
#include <leah/pmm.hpp>
#include <leah/scheduler.hpp>
#include <leah/string.hpp>

namespace gui {
namespace {

// --- the palette ------------------------------------------------------------
//
// Two shades either side of the face colour is all a bevel needs: light above
// and left, dark below and right, and the eye reads it as raised. It is the
// whole visual language of this era of interface.
u32 g_desktop;
u32 g_face;          // window and chrome grey
u32 g_light;         // top-left bevel
u32 g_shadow;        // bottom-right bevel
u32 g_outline;
u32 g_title_active;
u32 g_title_idle;
u32 g_title_text;
u32 g_text;

constexpr u32 kTitleHeight  = 18;
constexpr u32 kBorder       = 3;
constexpr u32 kCloseSize    = 12;

struct Window {
    bool  used;
    u32   owner;                 // the process that opened it
    i32   x, y;                  // top-left of the *frame*
    u32   width, height;         // the content area
    char  title[kTitleLength];

    u32*    pixels;              // content, width * height
    paddr_t pixels_phys;
    u32     pixels_bytes;

    Event queue[16];
    u32   queue_head;
    u32   queue_tail;
};

Window g_windows[kMaxWindows];
// Front to back. Index 0 is the topmost window, which is also the focused one -
// keeping the two the same avoids a second ordering to get out of step.
i32 g_order[kMaxWindows];
usize g_window_count = 0;

bool g_active = false;      // the server owns the screen
bool g_started = false;     // the compositor thread exists
// Teardown is requested by whoever closes the last window, but carried out by
// the compositor thread. Doing it on the caller's thread would race: the
// compositor can be part-way through a blit and would paint the desktop back
// over the console we just cleared.
bool g_teardown = false;
bool g_damaged = true;

u32* g_back = nullptr;           // the whole screen, composed off-line
u32  g_screen_width = 0;
u32  g_screen_height = 0;

// Cursor state, and the patch of backbuffer it is currently covering.
i32 g_cursor_x = 0;
i32 g_cursor_y = 0;
i32 g_last_cursor_x = -1;
i32 g_last_cursor_y = -1;

// While a button is held over a window's content, that window keeps the
// pointer: motion is reported to it even once the pointer strays outside. A
// stroke that runs off the edge of a window therefore stops there rather than
// carrying on into whatever is underneath.
i32 g_mouse_grab = -1;

// Drag state: which window is being moved, and where the pointer grabbed it.
i32 g_dragging = -1;
i32 g_drag_dx = 0;
i32 g_drag_dy = 0;
bool g_last_left = false;

constexpr u32 kCursorWidth = 12;
constexpr u32 kCursorHeight = 19;

// A plain arrow. 0 is transparent, 1 the black outline, 2 the white fill -
// an outline is what makes a cursor visible over both light and dark windows.
const u8 kCursor[kCursorHeight][kCursorWidth] = {
    {1,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0,0},
    {1,2,1,0,0,0,0,0,0,0,0,0},
    {1,2,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0,0},
    {1,2,2,2,2,2,1,0,0,0,0,0},
    {1,2,2,2,2,2,2,1,0,0,0,0},
    {1,2,2,2,2,2,2,2,1,0,0,0},
    {1,2,2,2,2,2,2,2,2,1,0,0},
    {1,2,2,2,2,2,2,2,2,2,1,0},
    {1,2,2,2,2,2,2,1,1,1,1,1},
    {1,2,2,2,1,2,2,1,0,0,0,0},
    {1,2,2,1,1,2,2,1,0,0,0,0},
    {1,2,1,0,0,1,2,2,1,0,0,0},
    {1,1,0,0,0,1,2,2,1,0,0,0},
    {1,0,0,0,0,0,1,2,2,1,0,0},
    {0,0,0,0,0,0,1,2,2,1,0,0},
    {0,0,0,0,0,0,0,1,1,0,0,0},
};

// --- drawing into the backbuffer --------------------------------------------

void back_plot(i32 x, i32 y, u32 colour)
{
    if (x < 0 || y < 0 || static_cast<u32>(x) >= g_screen_width ||
        static_cast<u32>(y) >= g_screen_height)
        return;
    g_back[static_cast<u64>(y) * g_screen_width + static_cast<u32>(x)] = colour;
}

void fill(i32 x, i32 y, u32 width, u32 height, u32 colour)
{
    for (u32 row = 0; row < height; ++row) {
        for (u32 column = 0; column < width; ++column)
            back_plot(x + static_cast<i32>(column), y + static_cast<i32>(row), colour);
    }
}

// A raised (or, inverted, a sunken) rectangle: light on the top and left edges,
// shadow on the bottom and right.
void bevel(i32 x, i32 y, u32 width, u32 height, bool raised)
{
    const u32 top_left = raised ? g_light : g_shadow;
    const u32 bottom_right = raised ? g_shadow : g_light;
    for (u32 i = 0; i < width; ++i) {
        back_plot(x + static_cast<i32>(i), y, top_left);
        back_plot(x + static_cast<i32>(i), y + static_cast<i32>(height) - 1,
                  bottom_right);
    }
    for (u32 i = 0; i < height; ++i) {
        back_plot(x, y + static_cast<i32>(i), top_left);
        back_plot(x + static_cast<i32>(width) - 1, y + static_cast<i32>(i),
                  bottom_right);
    }
}

void draw_text(i32 x, i32 y, const char* text, u32 colour)
{
    for (usize i = 0; text[i] != '\0'; ++i) {
        for (u32 row = 0; row < framebuffer::kGlyphHeight; ++row) {
            const u8 bits = framebuffer::glyph_row(text[i], row);
            for (u32 column = 0; column < framebuffer::kGlyphWidth; ++column) {
                if ((bits & (0x80 >> column)) != 0) {
                    back_plot(x + static_cast<i32>(i * framebuffer::kGlyphWidth + column),
                              y + static_cast<i32>(row), colour);
                }
            }
        }
    }
}

u32 frame_width(const Window& w)  { return w.width + kBorder * 2; }
u32 frame_height(const Window& w) { return w.height + kBorder * 2 + kTitleHeight; }

// Where the close box sits, in screen coordinates. On the left, as this era of
// interface had it.
void close_box(const Window& w, i32& x, i32& y)
{
    x = w.x + static_cast<i32>(kBorder) + 3;
    y = w.y + static_cast<i32>(kBorder) + 3;
}

void draw_window(const Window& w, bool focused)
{
    const u32 fw = frame_width(w);
    const u32 fh = frame_height(w);

    fill(w.x, w.y, fw, fh, g_face);
    bevel(w.x, w.y, fw, fh, true);
    // A second, inner bevel is what gives the frame its thickness.
    bevel(w.x + 1, w.y + 1, fw - 2, fh - 2, true);

    // Title bar.
    const i32 title_x = w.x + static_cast<i32>(kBorder);
    const i32 title_y = w.y + static_cast<i32>(kBorder);
    const u32 title_w = fw - kBorder * 2;
    fill(title_x, title_y, title_w, kTitleHeight,
         focused ? g_title_active : g_title_idle);

    // Close box: a small raised square with a bar across it.
    i32 cx, cy;
    close_box(w, cx, cy);
    fill(cx, cy, kCloseSize, kCloseSize, g_face);
    bevel(cx, cy, kCloseSize, kCloseSize, true);
    for (u32 i = 3; i < kCloseSize - 3; ++i) {
        back_plot(cx + static_cast<i32>(i), cy + static_cast<i32>(kCloseSize / 2),
                  g_outline);
    }

    // Title text, centred vertically in the bar and clear of the close box.
    draw_text(cx + static_cast<i32>(kCloseSize) + 6,
              title_y + (static_cast<i32>(kTitleHeight) - 16) / 2 + 1,
              w.title, g_title_text);

    // The content area, sunken so it reads as inset into the frame.
    const i32 content_x = w.x + static_cast<i32>(kBorder);
    const i32 content_y = title_y + static_cast<i32>(kTitleHeight);
    for (u32 row = 0; row < w.height; ++row) {
        for (u32 column = 0; column < w.width; ++column) {
            back_plot(content_x + static_cast<i32>(column),
                      content_y + static_cast<i32>(row),
                      w.pixels[static_cast<u64>(row) * w.width + column]);
        }
    }
}

// Compose the whole screen: desktop, then windows back to front so the topmost
// is drawn last and wins.
void compose()
{
    for (u32 i = 0; i < g_screen_width * g_screen_height; ++i)
        g_back[i] = g_desktop;

    for (i32 i = static_cast<i32>(g_window_count) - 1; i >= 0; --i) {
        const i32 id = g_order[i];
        if (id >= 0 && g_windows[id].used)
            draw_window(g_windows[id], i == 0);
    }
}

void present_region(i32 x, i32 y, u32 width, u32 height)
{
    if (x < 0) { width = width > static_cast<u32>(-x) ? width + x : 0; x = 0; }
    if (y < 0) { height = height > static_cast<u32>(-y) ? height + y : 0; y = 0; }
    if (width == 0 || height == 0)
        return;
    if (static_cast<u32>(x) >= g_screen_width || static_cast<u32>(y) >= g_screen_height)
        return;
    if (x + width > g_screen_width)
        width = g_screen_width - static_cast<u32>(x);
    if (y + height > g_screen_height)
        height = g_screen_height - static_cast<u32>(y);

    framebuffer::blit(g_back + static_cast<u64>(y) * g_screen_width + x,
                      g_screen_width, static_cast<u32>(x), static_cast<u32>(y),
                      width, height);
}

// The cursor is drawn straight to the screen rather than into the backbuffer,
// so moving it does not mean recomposing - the area it used to cover is simply
// repainted from the backbuffer, which still holds the clean image.
void draw_cursor()
{
    for (u32 row = 0; row < kCursorHeight; ++row) {
        for (u32 column = 0; column < kCursorWidth; ++column) {
            const u8 shade = kCursor[row][column];
            if (shade == 0)
                continue;
            framebuffer::plot(static_cast<u32>(g_cursor_x + static_cast<i32>(column)),
                              static_cast<u32>(g_cursor_y + static_cast<i32>(row)),
                              shade == 1 ? 0x000000u : 0xFFFFFFu);
        }
    }
}

void erase_cursor()
{
    if (g_last_cursor_x < 0)
        return;
    present_region(g_last_cursor_x, g_last_cursor_y, kCursorWidth, kCursorHeight);
}

// --- window management ------------------------------------------------------

void push_event(Window& w, EventType type, i32 x, i32 y, u32 button, u32 key)
{
    const u32 next = (w.queue_tail + 1) % 16;
    if (next == w.queue_head)
        return;                     // full: drop rather than overwrite unread
    Event& event = w.queue[w.queue_tail];
    event.type = static_cast<u32>(type);
    event.window = static_cast<u32>(&w - g_windows);
    event.x = x;
    event.y = y;
    event.button = button;
    event.key = key;
    w.queue_tail = next;
}

void raise_window(i32 id)
{
    usize at = 0;
    while (at < g_window_count && g_order[at] != id)
        ++at;
    if (at >= g_window_count || at == 0)
        return;
    for (usize i = at; i > 0; --i)
        g_order[i] = g_order[i - 1];
    g_order[0] = id;
    g_damaged = true;
}

// The topmost window containing this point, or -1 for the desktop.
i32 window_at(i32 x, i32 y)
{
    for (usize i = 0; i < g_window_count; ++i) {
        const i32 id = g_order[i];
        if (id < 0 || !g_windows[id].used)
            continue;
        const Window& w = g_windows[id];
        if (x >= w.x && y >= w.y &&
            x < w.x + static_cast<i32>(frame_width(w)) &&
            y < w.y + static_cast<i32>(frame_height(w)))
            return id;
    }
    return -1;
}

// Whoever is on top has the keyboard. While the desktop is up it is the only
// consumer of the key buffer - the shell that launched it is blocked waiting
// for its children, so nothing else is reading.
void handle_keyboard()
{
    while (keyboard::has_input()) {
        const char c = keyboard::read();
        if (c == 0 || g_window_count == 0)
            continue;
        Window& w = g_windows[g_order[0]];
        if (w.used)
            push_event(w, EventType::Key, 0, 0, 0, static_cast<u32>(c));
    }
}

void handle_mouse()
{
    const mouse::State state = mouse::state();

    // The mouse driver reports in its own coordinate space; clamp to the screen
    // so the cursor cannot be lost off an edge.
    const i32 before_x = g_cursor_x;   // to tell real motion from a repeat
    const i32 before_y = g_cursor_y;

    i32 x = state.x;
    i32 y = state.y;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (static_cast<u32>(x) >= g_screen_width)  x = static_cast<i32>(g_screen_width) - 1;
    if (static_cast<u32>(y) >= g_screen_height) y = static_cast<i32>(g_screen_height) - 1;
    g_cursor_x = x;
    g_cursor_y = y;

    const bool pressed = state.left && !g_last_left;
    const bool released = !state.left && g_last_left;

    if (pressed) {
        const i32 id = window_at(x, y);
        if (id >= 0) {
            Window& w = g_windows[id];
            raise_window(id);

            i32 cx, cy;
            close_box(w, cx, cy);
            const bool on_close = x >= cx && y >= cy &&
                                  x < cx + static_cast<i32>(kCloseSize) &&
                                  y < cy + static_cast<i32>(kCloseSize);
            const bool on_title =
                y < w.y + static_cast<i32>(kBorder + kTitleHeight);

            if (on_close) {
                push_event(w, EventType::Close, 0, 0, 1, 0);
            } else if (on_title) {
                // Grab the title bar: remember where within the window the
                // pointer took hold, so it stays under the same spot.
                g_dragging = id;
                g_drag_dx = x - w.x;
                g_drag_dy = y - w.y;
            } else {
                const i32 content_x = w.x + static_cast<i32>(kBorder);
                const i32 content_y = w.y + static_cast<i32>(kBorder + kTitleHeight);
                push_event(w, EventType::MouseDown, x - content_x, y - content_y, 1, 0);
                g_mouse_grab = id;
            }
        }
    }

    // Motion goes to whoever holds the pointer, in that window's coordinates.
    if (g_mouse_grab >= 0 && g_windows[g_mouse_grab].used &&
        (x != before_x || y != before_y)) {
        Window& w = g_windows[g_mouse_grab];
        const i32 content_x = w.x + static_cast<i32>(kBorder);
        const i32 content_y = w.y + static_cast<i32>(kBorder + kTitleHeight);
        push_event(w, EventType::MouseMove, x - content_x, y - content_y, 1, 0);
    }

    if (released) {
        g_dragging = -1;
        // The release belongs to the grab holder, not to whatever the pointer
        // happens to be over now.
        const i32 id = g_mouse_grab >= 0 ? g_mouse_grab : window_at(x, y);
        g_mouse_grab = -1;
        if (id >= 0 && g_windows[id].used) {
            Window& w = g_windows[id];
            const i32 content_x = w.x + static_cast<i32>(kBorder);
            const i32 content_y = w.y + static_cast<i32>(kBorder + kTitleHeight);
            push_event(w, EventType::MouseUp, x - content_x, y - content_y, 1, 0);
        }
    }

    if (g_dragging >= 0 && g_windows[g_dragging].used) {
        Window& w = g_windows[g_dragging];
        i32 new_x = x - g_drag_dx;
        i32 new_y = y - g_drag_dy;

        // Keep the title bar reachable. A window dragged clean off an edge
        // would take its close box with it and could never be shut again, so
        // the bar - and with it the drag handle and the close box - always
        // stays on the screen.
        const i32 keep = static_cast<i32>(kBorder + kCloseSize) + 8;
        const i32 max_x = static_cast<i32>(g_screen_width) - keep;
        const i32 max_y = static_cast<i32>(g_screen_height) -
                          static_cast<i32>(kBorder + kTitleHeight);
        if (new_x < 0) new_x = 0;
        if (new_y < 0) new_y = 0;
        if (new_x > max_x) new_x = max_x;
        if (new_y > max_y) new_y = max_y;

        if (new_x != w.x || new_y != w.y) {
            w.x = new_x;
            w.y = new_y;
            g_damaged = true;
        }
    }

    g_last_left = state.left;
}

[[noreturn]] void compositor_thread(void*)
{
    for (;;) {
        if (g_teardown) {
            g_teardown = false;
            g_active = false;
            g_dragging = -1;
            g_mouse_grab = -1;
            g_last_cursor_x = -1;
            g_last_cursor_y = -1;

            // Nothing else draws after this point, so the cleared console is
            // the last thing on the screen.
            console::suspend_display(false);
            console::clear();
        }
        if (!g_active) {
            scheduler::yield();      // parked: the desktop is not up
            continue;
        }

        handle_keyboard();
        handle_mouse();

        if (g_damaged) {
            compose();
            present_region(0, 0, g_screen_width, g_screen_height);
            g_last_cursor_x = -1;       // the whole screen was repainted
            g_damaged = false;
        } else if (g_cursor_x != g_last_cursor_x || g_cursor_y != g_last_cursor_y) {
            // Only the cursor moved: repaint what it was covering and redraw it
            // in its new place. Recomposing for a pointer move would make the
            // whole screen's worth of work follow the mouse around.
            erase_cursor();
        } else {
            scheduler::yield();
            continue;
        }

        draw_cursor();
        g_last_cursor_x = g_cursor_x;
        g_last_cursor_y = g_cursor_y;
        scheduler::yield();
    }
}

} // namespace

bool init()
{
    if (g_active)
        return true;                 // started on demand; starting twice is fine
    if (!framebuffer::available())
        return false;

    g_screen_width = framebuffer::width();
    g_screen_height = framebuffer::height();

    if (g_back == nullptr) {
        const usize pixels = static_cast<usize>(g_screen_width) * g_screen_height;
        g_back = static_cast<u32*>(kmalloc(pixels * sizeof(u32)));
        if (g_back == nullptr)
            return false;
    }

    g_desktop      = framebuffer::rgb(0x00, 0x80, 0x80);   // the teal desktop
    g_face         = framebuffer::rgb(0xC0, 0xC0, 0xC0);
    g_light        = framebuffer::rgb(0xFF, 0xFF, 0xFF);
    g_shadow       = framebuffer::rgb(0x60, 0x60, 0x60);
    g_outline      = framebuffer::rgb(0x00, 0x00, 0x00);
    g_title_active = framebuffer::rgb(0x00, 0x00, 0x80);
    g_title_idle   = framebuffer::rgb(0x80, 0x80, 0x80);
    g_title_text   = framebuffer::rgb(0xFF, 0xFF, 0xFF);
    g_text         = framebuffer::rgb(0x00, 0x00, 0x00);

    memset(g_windows, 0, sizeof(g_windows));
    for (usize i = 0; i < kMaxWindows; ++i)
        g_order[i] = -1;
    g_window_count = 0;

    g_cursor_x = static_cast<i32>(g_screen_width) / 2;
    g_cursor_y = static_cast<i32>(g_screen_height) / 2;

    // The screen is the server's from here. Text still goes to the serial port,
    // so the boot log and any panic survive; it just stops being painted over
    // the desktop.
    console::suspend_display(true);

    g_active = true;
    g_damaged = true;

    // One thread for the lifetime of the system: it parks itself when the
    // desktop is torn down and picks up again if it is started a second time.
    if (!g_started) {
        scheduler::spawn("wserver", compositor_thread, nullptr);
        g_started = true;
    }
    return true;
}

bool active() { return g_active; }

i32 create_window(i32 x, i32 y, u32 width, u32 height, const char* title)
{
    if (!g_active || width == 0 || height == 0)
        return -1;
    if (width > g_screen_width || height > g_screen_height)
        return -1;
    if (g_window_count >= kMaxWindows)
        return -1;

    i32 id = -1;
    for (usize i = 0; i < kMaxWindows; ++i) {
        if (!g_windows[i].used) {
            id = static_cast<i32>(i);
            break;
        }
    }
    if (id < 0)
        return -1;

    Window& w = g_windows[id];
    memset(&w, 0, sizeof(w));

    // Contiguous, because the frames get mapped into the client as one run.
    const u32 bytes = width * height * sizeof(u32);
    const usize pages = (bytes + pmm::kPageSize - 1) / pmm::kPageSize;
    const paddr_t phys = pmm::alloc_contiguous(pages);
    if (phys == 0)
        return -1;

    w.used = true;
    w.owner = scheduler::current_tgid();
    w.x = x;
    w.y = y;
    w.width = width;
    w.height = height;
    w.pixels = reinterpret_cast<u32*>(memory::phys_to_direct(phys));
    w.pixels_phys = phys;
    w.pixels_bytes = static_cast<u32>(pages * pmm::kPageSize);

    usize n = 0;
    while (title != nullptr && title[n] != '\0' && n < kTitleLength - 1) {
        w.title[n] = title[n];
        ++n;
    }
    w.title[n] = '\0';

    // A window starts white rather than showing whatever was in the frames.
    for (u32 i = 0; i < width * height; ++i)
        w.pixels[i] = 0xFFFFFFu;

    // Newest on top, which is also focused.
    for (usize i = g_window_count; i > 0; --i)
        g_order[i] = g_order[i - 1];
    g_order[0] = id;
    ++g_window_count;

    g_damaged = true;
    return id;
}

void destroy_window(i32 id)
{
    if (id < 0 || id >= static_cast<i32>(kMaxWindows) || !g_windows[id].used)
        return;
    Window& w = g_windows[id];

    const usize pages = w.pixels_bytes / pmm::kPageSize;
    pmm::free_contiguous(w.pixels_phys, pages);
    w.used = false;

    usize at = 0;
    while (at < g_window_count && g_order[at] != id)
        ++at;
    if (at < g_window_count) {
        for (usize i = at; i + 1 < g_window_count; ++i)
            g_order[i] = g_order[i + 1];
        g_order[--g_window_count] = -1;
    }
    if (g_mouse_grab == id)
        g_mouse_grab = -1;
    if (g_dragging == id)
        g_dragging = -1;
    g_damaged = true;

    // An empty desktop is no use to anyone: hand the screen back to the text
    // console rather than leaving the user staring at teal with no prompt.
    if (g_window_count == 0)
        shutdown();
}

void close_windows_of(u32 tgid)
{
    if (!g_active || tgid == 0)
        return;
    for (usize i = 0; i < kMaxWindows; ++i) {
        if (g_windows[i].used && g_windows[i].owner == tgid)
            destroy_window(static_cast<i32>(i));
    }
}

void shutdown()
{
    if (g_active)
        g_teardown = true;
}

u32* buffer_of(i32 id)
{
    if (id < 0 || id >= static_cast<i32>(kMaxWindows) || !g_windows[id].used)
        return nullptr;
    return g_windows[id].pixels;
}

paddr_t buffer_phys(i32 id)
{
    if (id < 0 || id >= static_cast<i32>(kMaxWindows) || !g_windows[id].used)
        return 0;
    return g_windows[id].pixels_phys;
}

u32 buffer_bytes(i32 id)
{
    if (id < 0 || id >= static_cast<i32>(kMaxWindows) || !g_windows[id].used)
        return 0;
    return g_windows[id].pixels_bytes;
}

void present(i32 id)
{
    if (id < 0 || id >= static_cast<i32>(kMaxWindows) || !g_windows[id].used)
        return;
    g_damaged = true;
}

bool poll_event(i32 id, Event& out)
{
    if (id < 0 || id >= static_cast<i32>(kMaxWindows) || !g_windows[id].used)
        return false;
    Window& w = g_windows[id];
    if (w.queue_head == w.queue_tail)
        return false;
    out = w.queue[w.queue_head];
    w.queue_head = (w.queue_head + 1) % 16;
    return true;
}

u32 owner_of(i32 id)
{
    if (id < 0 || id >= static_cast<i32>(kMaxWindows) || !g_windows[id].used)
        return 0;
    return g_windows[id].owner;
}

} // namespace gui
