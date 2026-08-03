#include <leah/console.hpp>
#include <leah/spinlock.hpp>
#include <leah/framebuffer.hpp>
#include <leah/io.hpp>
#include <leah/string.hpp>

#include <stdarg.h>

namespace console {
namespace {

// The kernel's own voice, and nothing else's.
//
// This used to render text into the framebuffer and into VGA text memory, keep
// a cursor, scroll, and understand enough ANSI to clear a screen. None of that
// is here now. The screen belongs to userland: wserver already mapped it and
// drew its own pixels, and the only thing still painting glyphs from ring 0 was
// the kernel talking to itself.
//
// So it talks over COM1 instead. That is what a microkernel's console is - seL4
// and L4 both keep a serial debug printf and no more - because a kernel that
// can draw is a kernel owning a device it has no business owning.
//
// The cost is real and worth stating plainly: boot messages and panics are
// visible over serial only, and on a machine with no serial port a panic says
// nothing at all.

sync::Spinlock g_console_lock;

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

// Everything inside this file goes through here; the public entry points take
// the lock, once, at the outermost call - taking it again further in would
// deadlock, since a plain spinlock has no notion of already owning it.
void put_locked(char c)
{
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
    /* Still brought up, because userland has to be able to map it - but the
     * kernel never draws into it again. */
    framebuffer::init(info);
}

/* Colour was a property of a screen this no longer draws to, and the cursor
 * was a position on it. Both are kept as calls that do nothing, so the fifty
 * places that set a colour before a banner do not each have to be edited to
 * say the same thing in silence. */
void set_color(Color, Color) {}
bool graphical() { return false; }
u32  columns() { return 80; }
u32  rows()    { return 25; }
void clear() {}
void suspend_display(bool) {}
void grant_display_to(u32) {}
void reclaim_display(u32) {}

void put(char c)
{
    sync::IrqScopedLock guard(g_console_lock);
    put_locked(c);
}

void write(const char* str)
{
    sync::IrqScopedLock guard(g_console_lock);
    write_locked(str);
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
}

} // namespace console
