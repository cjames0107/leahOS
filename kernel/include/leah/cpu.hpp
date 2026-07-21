#pragma once

#include <leah/types.hpp>

// Thin wrappers over the instructions and registers that have no C++ spelling.

namespace cpu {

inline void cli() { asm volatile("cli" ::: "memory"); }
inline void sti() { asm volatile("sti" ::: "memory"); }
inline void hlt() { asm volatile("hlt"); }

[[noreturn]] inline void halt_forever()
{
    for (;;)
        asm volatile("cli; hlt");
}

// Park the CPU until the next interrupt. The sti/hlt pair is deliberately
// adjacent: the CPU blocks interrupts for one instruction after sti, which
// closes the race where an interrupt arrives between enabling and halting and
// leaves us asleep with nothing left to wake us.
inline void wait_for_interrupt()
{
    asm volatile("sti; hlt");
}

inline bool interrupts_enabled()
{
    u64 flags;
    asm volatile("pushfq; popq %0" : "=r"(flags));
    return (flags & (1 << 9)) != 0;
}

inline u64 read_cr0() { u64 v; asm volatile("mov %%cr0, %0" : "=r"(v)); return v; }
inline u64 read_cr2() { u64 v; asm volatile("mov %%cr2, %0" : "=r"(v)); return v; }
inline u64 read_cr3() { u64 v; asm volatile("mov %%cr3, %0" : "=r"(v)); return v; }
inline u64 read_cr4() { u64 v; asm volatile("mov %%cr4, %0" : "=r"(v)); return v; }

inline u64 read_msr(u32 msr)
{
    u32 low, high;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return static_cast<u64>(low) | static_cast<u64>(high) << 32;
}

inline void write_msr(u32 msr, u64 value)
{
    asm volatile("wrmsr"
                 :
                 : "c"(msr),
                   "a"(static_cast<u32>(value)),
                   "d"(static_cast<u32>(value >> 32)));
}

// RAII guard for a critical section. Restores the previous interrupt state
// rather than blindly re-enabling, so nesting one inside another is safe.
class InterruptGuard {
public:
    InterruptGuard() : m_was_enabled(interrupts_enabled()) { cli(); }
    ~InterruptGuard() { if (m_was_enabled) sti(); }

    InterruptGuard(const InterruptGuard&) = delete;
    InterruptGuard& operator=(const InterruptGuard&) = delete;

private:
    bool m_was_enabled;
};

} // namespace cpu
