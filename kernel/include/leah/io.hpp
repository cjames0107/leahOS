#pragma once

#include <leah/types.hpp>

// x86 port I/O. The io_wait() spin exists because some legacy devices need a
// moment between writes and the traditional trick is a dummy write to an
// unused port.

namespace io {

inline void out8(u16 port, u8 value)
{
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

inline u8 in8(u16 port)
{
    u8 value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

inline void out16(u16 port, u16 value)
{
    asm volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

inline u16 in16(u16 port)
{
    u16 value;
    asm volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

inline void out32(u16 port, u32 value)
{
    asm volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

inline u32 in32(u16 port)
{
    u32 value;
    asm volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

inline void wait()
{
    out8(0x80, 0);
}

} // namespace io
