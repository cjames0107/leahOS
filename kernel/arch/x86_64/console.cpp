#include <leah/console.hpp>
#include <leah/spinlock.hpp>
#include <leah/framebuffer.hpp>
#include <leah/io.hpp>
#include <leah/string.hpp>

#include <stdarg.h>

namespace console {
namespace {

// --- backends --------------------------------------------------------------
//
// Two display targets behind one cursor. The framebuffer is used when stage 2
// managed to set a VBE mode; the VGA text buffer is the fallback, and is what
// keeps the kernel debuggable on hardware where the mode set failed.

constexpr u16 kVgaWidth  = 80;
constexpr u16 kVgaHeight = 25;
constexpr u64 kVgaBuffer = 0xB8000;
constexpr u16 kCrtcIndex = 0x3D4;
constexpr u16 kCrtcData  = 0x3D5;

volatile u16* const g_vga = reinterpret_cast<volatile u16*>(kVgaBuffer);

bool g_graphical = false;
u32  g_columns = kVgaWidth;
u32  g_rows    = kVgaHeight;

// The console has state - a cursor, a scroll position - that two CPUs writing
// at once will corrupt. A lock of its own rather than the kernel lock: panic
// prints from a CPU that may hold neither, and interleaved output from two
// panicking cores is how the first SMP bug here announced itself.
sync::Spinlock g_console_lock;

// True while the window server owns the framebuffer, and which process that is.
bool g_display_suspended = false;
u32  g_display_owner = 0;

u32 g_row    = 0;
u32 g_column = 0;
u8  g_attr   = static_cast<u8>(Color::LightGray);

// The IBM PC's 16 colours, as the RGB a framebuffer needs. Keeping the palette
// means code written against the text console renders identically either way.
constexpr u32 kPalette[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

constexpr u16 vga_cell(char c, u8 attr)
{
    return static_cast<u16>(static_cast<u8>(c)) | static_cast<u16>(attr) << 8;
}

u32 foreground_rgb() { return kPalette[g_attr & 0x0F]; }
u32 background_rgb() { return kPalette[g_attr >> 4 & 0x0F]; }

void render(u32 row, u32 column, char c)
{
    if (g_graphical)
        framebuffer::draw_glyph(column, row, c, foreground_rgb(), background_rgb());
    else
        g_vga[row * kVgaWidth + column] = vga_cell(c, g_attr);
}

void update_cursor()
{
    // The CRTC cursor only exists in text mode. In graphics mode the caret is
    // drawn by whoever wants one; nothing here pretends otherwise.
    if (g_graphical)
        return;

    const u16 pos = static_cast<u16>(g_row * kVgaWidth + g_column);
    io::out8(kCrtcIndex, 0x0F);
    io::out8(kCrtcData, static_cast<u8>(pos & 0xFF));
    io::out8(kCrtcIndex, 0x0E);
    io::out8(kCrtcData, static_cast<u8>(pos >> 8));
}

void scroll()
{
    if (g_graphical) {
        framebuffer::scroll_up(background_rgb());
    } else {
        for (u32 row = 1; row < kVgaHeight; ++row) {
            for (u32 col = 0; col < kVgaWidth; ++col)
                g_vga[(row - 1) * kVgaWidth + col] = g_vga[row * kVgaWidth + col];
        }
        for (u32 col = 0; col < kVgaWidth; ++col)
            g_vga[(kVgaHeight - 1) * kVgaWidth + col] = vga_cell(' ', g_attr);
    }
    g_row = g_rows - 1;
}

void display_raw(char c)
{
    switch (c) {
    case '\n':
        g_column = 0;
        ++g_row;
        break;
    case '\r':
        g_column = 0;
        break;
    case '\t':
        g_column = (g_column + 8) & ~7u;
        break;
    case '\b':
        if (g_column > 0)
            --g_column;
        break;
    default:
        render(g_row, g_column, c);
        ++g_column;
        break;
    }

    if (g_column >= g_columns) {
        g_column = 0;
        ++g_row;
    }
    while (g_row >= g_rows)
        scroll();
}

// A minimal ANSI CSI reader. It acts on the two sequences a screen-clear needs
// - ESC[2J (erase display) and ESC[H (cursor home) - and silently swallows any
// other CSI sequence so an unhandled escape does not spray its bytes across the
// screen. Enough for `clear`; a fuller terminal can grow from here.
enum class Esc { Normal, Escape, Csi };
Esc  g_esc = Esc::Normal;
u32  g_csi_param = 0;

void clear_screen()
{
    if (g_graphical)
        framebuffer::clear(background_rgb());
    else {
        for (u16 i = 0; i < kVgaWidth * kVgaHeight; ++i)
            g_vga[i] = vga_cell(' ', g_attr);
    }
    g_row = 0;
    g_column = 0;
}

void display_put(char c)
{
    switch (g_esc) {
    case Esc::Normal:
        if (c == 0x1B) {              // ESC
            g_esc = Esc::Escape;
            return;
        }
        display_raw(c);
        return;

    case Esc::Escape:
        g_esc = (c == '[') ? Esc::Csi : Esc::Normal;
        g_csi_param = 0;
        return;

    case Esc::Csi:
        if (c >= '0' && c <= '9') {
            g_csi_param = g_csi_param * 10 + static_cast<u32>(c - '0');
            return;
        }
        // A letter ends the sequence.
        if (c == 'J' && g_csi_param == 2) {
            clear_screen();
        } else if (c == 'H') {
            g_row = 0;
            g_column = 0;
        }
        g_esc = Esc::Normal;
        return;
    }
}

// --- COM1 ------------------------------------------------------------------

constexpr u16 kCom1 = 0x3F8;
bool g_serial_ok = false;

bool serial_init()
{
    io::out8(kCom1 + 1, 0x00);      // interrupts off
    io::out8(kCom1 + 3, 0x80);      // DLAB on
    io::out8(kCom1 + 0, 0x01);      // divisor 1 => 115200 baud
    io::out8(kCom1 + 1, 0x00);
    io::out8(kCom1 + 3, 0x03);      // 8N1, DLAB off
    io::out8(kCom1 + 2, 0xC7);      // FIFO on, cleared, 14-byte threshold
    io::out8(kCom1 + 4, 0x1E);      // loopback for the self-test below

    io::out8(kCom1 + 0, 0xAE);
    if (io::in8(kCom1 + 0) != 0xAE)
        return false;

    io::out8(kCom1 + 4, 0x0F);      // DTR/RTS/OUT2, normal operation
    return true;
}

void serial_put(char c)
{
    if (!g_serial_ok)
        return;
    while ((io::in8(kCom1 + 5) & 0x20) == 0)
        ;
    io::out8(kCom1, static_cast<u8>(c));
}

// --- number formatting -----------------------------------------------------

// Emit one character with the console lock already held. Everything inside this
// file goes through here; the public entry points are what take the lock, once,
// at the outermost call - taking it again further in would deadlock, since a
// plain spinlock has no notion of already owning it.
void put_locked(char c)
{
    if (!g_display_suspended)
        display_put(c);
    if (c == '\n')
        serial_put('\r');
    serial_put(c);
}

void write_locked(const char* str)
{
    for (usize i = 0; str[i] != '\0'; ++i)
        put_locked(str[i]);
}

void put_unsigned(u64 value, u32 base, bool upper, u32 min_width, char pad)
{
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    char buf[24];
    u32 len = 0;
    do {
        buf[len++] = digits[value % base];
        value /= base;
    } while (value != 0);

    for (u32 i = len; i < min_width; ++i)
        put_locked(pad);
    while (len > 0)
        put_locked(buf[--len]);
}

void put_signed(i64 value, u32 min_width, char pad)
{
    if (value < 0) {
        put_locked('-');
        put_unsigned(~static_cast<u64>(value) + 1, 10, false,
                     min_width > 0 ? min_width - 1 : 0, pad);
        return;
    }
    put_unsigned(static_cast<u64>(value), 10, false, min_width, pad);
}

} // namespace

void init(const boot::Info& info)
{
    g_serial_ok = serial_init();

    g_graphical = framebuffer::init(info);
    if (g_graphical) {
        g_columns = framebuffer::columns();
        g_rows    = framebuffer::rows();
    } else {
        g_columns = kVgaWidth;
        g_rows    = kVgaHeight;
    }

    clear();
}

bool graphical() { return g_graphical; }
u32 columns() { return g_columns; }
u32 rows() { return g_rows; }

void clear()
{
    sync::IrqScopedLock guard(g_console_lock);
    if (g_graphical) {
        framebuffer::clear(background_rgb());
    } else {
        for (u32 i = 0; i < kVgaWidth * kVgaHeight; ++i)
            g_vga[i] = vga_cell(' ', g_attr);
    }
    g_row = 0;
    g_column = 0;
    update_cursor();
}

void suspend_display(bool suspended)
{
    sync::IrqScopedLock guard(g_console_lock);
    g_display_suspended = suspended;
}

void grant_display_to(u32 tgid)
{
    {
        sync::IrqScopedLock guard(g_console_lock);
        g_display_owner = tgid;
        g_display_suspended = true;
    }
}

void reclaim_display(u32 tgid)
{
    {
        sync::IrqScopedLock guard(g_console_lock);
        if (tgid == 0 || g_display_owner != tgid)
            return;
        g_display_owner = 0;
        g_display_suspended = false;
    }
    // Outside the lock: clear() takes it too, and this one does not nest.
    clear();
}

void set_color(Color fg, Color bg)
{
    sync::IrqScopedLock guard(g_console_lock);
    g_attr = static_cast<u8>(static_cast<u8>(fg) | static_cast<u8>(bg) << 4);
}

void put(char c)
{
    sync::IrqScopedLock guard(g_console_lock);
    put_locked(c);
}

void write(const char* str)
{
    sync::IrqScopedLock guard(g_console_lock);
    write_locked(str);
    update_cursor();
}

void printf(const char* fmt, ...)
{
    sync::IrqScopedLock guard(g_console_lock);
    va_list args;
    va_start(args, fmt);

    for (usize i = 0; fmt[i] != '\0'; ++i) {
        if (fmt[i] != '%') {
            put_locked(fmt[i]);
            continue;
        }

        ++i;
        char pad = ' ';
        if (fmt[i] == '0') {
            pad = '0';
            ++i;
        }

        u32 width = 0;
        while (fmt[i] >= '0' && fmt[i] <= '9')
            width = width * 10 + static_cast<u32>(fmt[i++] - '0');

        // The length modifier is not decoration: default-promoted ints occupy
        // a 64-bit vararg slot but only define its low half, so reading 64
        // bits for a plain %x would pick up whatever was in the register.
        bool wide = false;
        while (fmt[i] == 'l' || fmt[i] == 'h' || fmt[i] == 'z') {
            if (fmt[i] != 'h')
                wide = true;
            ++i;
        }

        switch (fmt[i]) {
        case 'd':
        case 'i':
            put_signed(wide ? va_arg(args, i64) : va_arg(args, int), width, pad);
            break;
        case 'u':
            put_unsigned(wide ? va_arg(args, u64) : va_arg(args, unsigned int),
                         10, false, width, pad);
            break;
        case 'x':
            put_unsigned(wide ? va_arg(args, u64) : va_arg(args, unsigned int),
                         16, false, width, pad);
            break;
        case 'X':
            put_unsigned(wide ? va_arg(args, u64) : va_arg(args, unsigned int),
                         16, true, width, pad);
            break;
        case 'p':
            write_locked("0x");
            put_unsigned(reinterpret_cast<u64>(va_arg(args, void*)), 16, false, 16, '0');
            break;
        case 'c':
            put_locked(static_cast<char>(va_arg(args, int)));
            break;
        case 's': {
            const char* s = va_arg(args, const char*);
            write_locked(s != nullptr ? s : "(null)");
            break;
        }
        case '%':
            put_locked('%');
            break;
        default:
            put_locked('%');
            put_locked(fmt[i]);
            break;
        }
    }

    va_end(args);
    update_cursor();
}

} // namespace console
