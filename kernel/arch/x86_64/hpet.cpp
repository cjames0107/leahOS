#include <leah/acpi.hpp>
#include <leah/hpet.hpp>
#include <leah/memory.hpp>
#include <leah/vmm.hpp>

namespace hpet {
namespace {

constexpr u32 kCapabilities = 0x000;    // period in the high half
constexpr u32 kConfiguration = 0x010;   // bit 0 enables counting
constexpr u32 kMainCounter  = 0x0F0;

constexpr u64 kEnableCounter = 1ull << 0;
constexpr u64 kFemtosecondsPerSecond = 1000000000000000ull;

volatile u8* g_regs = nullptr;
u32 g_period_fs = 0;
u64 g_start = 0;

u64 read(u32 offset)
{
    return *reinterpret_cast<volatile u64*>(g_regs + offset);
}

void write(u32 offset, u64 value)
{
    *reinterpret_cast<volatile u64*>(g_regs + offset) = value;
}

} // namespace

bool init()
{
    const u64 base = acpi::hpet_address();
    if (base == 0)
        return false;
    if (!vmm::map_mmio(memory::kHpetMmio, base, 0x1000))
        return false;
    g_regs = reinterpret_cast<volatile u8*>(memory::kHpetMmio);

    // The period lives in the top 32 bits of the capability register. A zero or
    // absurd value means this is not a working HPET.
    g_period_fs = static_cast<u32>(read(kCapabilities) >> 32);
    if (g_period_fs == 0 || g_period_fs > 100000000u) {
        g_regs = nullptr;
        return false;
    }

    write(kConfiguration, read(kConfiguration) | kEnableCounter);
    g_start = read(kMainCounter);
    return true;
}

bool available() { return g_regs != nullptr; }

u32 period_fs() { return g_period_fs; }

u64 frequency_hz()
{
    return g_period_fs == 0 ? 0 : kFemtosecondsPerSecond / g_period_fs;
}

u64 counter() { return g_regs == nullptr ? 0 : read(kMainCounter); }

u64 uptime_us()
{
    if (g_regs == nullptr)
        return 0;
    // ticks * period_fs gives femtoseconds; a billion of those is a microsecond.
    // Dividing the period first would lose everything to truncation, so scale
    // the tick count up and accept the (very distant) overflow.
    return (counter() - g_start) / (1000000000ull / g_period_fs);
}

void busy_wait_us(u64 microseconds)
{
    if (g_regs == nullptr)
        return;
    const u64 ticks = microseconds * (1000000000ull / g_period_fs);
    const u64 deadline = counter() + ticks;
    while (counter() < deadline)
        asm volatile("pause");
}

} // namespace hpet
