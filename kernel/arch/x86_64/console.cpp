#include <leah/console.hpp>
#include <leah/io.hpp>
#include <leah/string.hpp>

#include <stdarg.h>

namespace console {
namespace {

// --- VGA text mode ---------------------------------------------------------

constexpr u16   kVgaWidth  = 80;
constexpr u16   kVgaHeight = 25;
constexpr u64   kVgaBuffer = 0xB8000;
constexpr u16   kCrtcIndex = 0x3D4;
constexpr u16   kCrtcData  = 0x3D5;

volatile u16* const g_vga = reinterpret_cast<volatile u16*>(kVgaBuffer);

u16 g_row    = 0;
u16 g_column = 0;
u8  g_attr   = static_cast<u8>(Color::LightGray);

constexpr u16 vga_cell(char c, u8 attr)
{
    return static_cast<u16>(static_cast<u8>(c)) | static_cast<u16>(attr) << 8;
}

void update_cursor()
{
    const u16 pos = g_row * kVgaWidth + g_column;
    io::out8(kCrtcIndex, 0x0F);
    io::out8(kCrtcData, static_cast<u8>(pos & 0xFF));
    io::out8(kCrtcIndex, 0x0E);
    io::out8(kCrtcData, static_cast<u8>(pos >> 8));
}

void scroll()
{
    // The buffer is MMIO but plain volatile u16 stores are fine here; go
    // through a loop rather than memmove to keep the volatile qualifier.
    for (u16 row = 1; row < kVgaHeight; ++row) {
        for (u16 col = 0; col < kVgaWidth; ++col)
            g_vga[(row - 1) * kVgaWidth + col] = g_vga[row * kVgaWidth + col];
    }
    for (u16 col = 0; col < kVgaWidth; ++col)
        g_vga[(kVgaHeight - 1) * kVgaWidth + col] = vga_cell(' ', g_attr);

    g_row = kVgaHeight - 1;
}

void vga_put(char c)
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
        g_column = static_cast<u16>((g_column + 8) & ~7u);
        break;
    case '\b':
        if (g_column > 0)
            --g_column;
        break;
    default:
        g_vga[g_row * kVgaWidth + g_column] = vga_cell(c, g_attr);
        ++g_column;
        break;
    }

    if (g_column >= kVgaWidth) {
        g_column = 0;
        ++g_row;
    }
    while (g_row >= kVgaHeight)
        scroll();
}

// --- COM1 ------------------------------------------------------------------

constexpr u16 kCom1 = 0x3F8;

bool serial_init()
{
    io::out8(kCom1 + 1, 0x00);      // interrupts off
    io::out8(kCom1 + 3, 0x80);      // DLAB on
    io::out8(kCom1 + 0, 0x01);      // divisor 1 => 115200 baud
    io::out8(kCom1 + 1, 0x00);
    io::out8(kCom1 + 3, 0x03);      // 8N1, DLAB off
    io::out8(kCom1 + 2, 0xC7);      // FIFO on, cleared, 14-byte threshold
    io::out8(kCom1 + 4, 0x1E);      // loopback for the self-test below

    // Bounce a byte off the loopback to confirm something is actually there.
    io::out8(kCom1 + 0, 0xAE);
    if (io::in8(kCom1 + 0) != 0xAE)
        return false;

    io::out8(kCom1 + 4, 0x0F);      // DTR/RTS/OUT2, normal operation
    return true;
}

bool g_serial_ok = false;

void serial_put(char c)
{
    if (!g_serial_ok)
        return;
    while ((io::in8(kCom1 + 5) & 0x20) == 0)    // wait for THR empty
        ;
    io::out8(kCom1, static_cast<u8>(c));
}

// --- number formatting -----------------------------------------------------

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
        put(pad);
    while (len > 0)
        put(buf[--len]);
}

void put_signed(i64 value, u32 min_width, char pad)
{
    if (value < 0) {
        put('-');
        // Negating i64 min overflows, so widen through the unsigned domain.
        put_unsigned(~static_cast<u64>(value) + 1, 10, false,
                     min_width > 0 ? min_width - 1 : 0, pad);
        return;
    }
    put_unsigned(static_cast<u64>(value), 10, false, min_width, pad);
}

} // namespace

void init()
{
    g_serial_ok = serial_init();
    clear();
}

void clear()
{
    for (u16 i = 0; i < kVgaWidth * kVgaHeight; ++i)
        g_vga[i] = vga_cell(' ', g_attr);
    g_row = 0;
    g_column = 0;
    update_cursor();
}

void set_color(Color fg, Color bg)
{
    g_attr = static_cast<u8>(static_cast<u8>(fg) | static_cast<u8>(bg) << 4);
}

void put(char c)
{
    vga_put(c);
    if (c == '\n')
        serial_put('\r');
    serial_put(c);
}

void write(const char* str)
{
    for (usize i = 0; str[i] != '\0'; ++i)
        put(str[i]);
    update_cursor();
}

void printf(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    for (usize i = 0; fmt[i] != '\0'; ++i) {
        if (fmt[i] != '%') {
            put(fmt[i]);
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
            write("0x");
            put_unsigned(reinterpret_cast<u64>(va_arg(args, void*)), 16, false, 16, '0');
            break;
        case 'c':
            put(static_cast<char>(va_arg(args, int)));
            break;
        case 's': {
            const char* s = va_arg(args, const char*);
            write(s != nullptr ? s : "(null)");
            break;
        }
        case '%':
            put('%');
            break;
        default:
            put('%');
            put(fmt[i]);
            break;
        }
    }

    va_end(args);
    update_cursor();
}

} // namespace console
