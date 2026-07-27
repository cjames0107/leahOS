#include <leah/interrupts.hpp>
#include <leah/io.hpp>
#include <leah/pic.hpp>
#include <leah/scheduler.hpp>
#include <leah/hpet.hpp>
#include <leah/usb_hid.hpp>
#include <leah/timer.hpp>

namespace timer {
namespace {

constexpr u16 kChannel0 = 0x40;
constexpr u16 kCommand  = 0x43;

// 1.193182 MHz, which is the NTSC colourburst frequency divided by three. The
// PC inherited it because in 1981 that crystal was the cheap one.
constexpr u32 kBaseFrequency = 1193182;

// Written by the IRQ handler, read by everything else. volatile keeps the
// compiler from hoisting the load out of a spin loop; it is not a substitute
// for an atomic once there is more than one CPU.
volatile u64 g_ticks = 0;
u32 g_frequency = kFrequencyHz;

void on_tick(interrupts::Frame&)
{
    // Spelled out rather than ++: a compound operation on a volatile is
    // deprecated because it hides that this is a separate load and store.
    g_ticks = g_ticks + 1;

    // USB has no interrupt of its own here, so the tick is what drives it. It
    // has to happen on a timer rather than only when someone is reading: a task
    // blocked waiting for a key is asleep on a channel that nothing else would
    // ever wake, since the wake comes from the keypress this poll discovers.
    usb::hid::poll();

    // Charge the running thread's time slice. The switch itself is deferred to
    // on_irq_return, after the PIC is acknowledged.
    scheduler::on_tick();
}

} // namespace

void init(u32 frequency_hz)
{
    if (frequency_hz == 0)
        frequency_hz = kFrequencyHz;

    u32 divisor = kBaseFrequency / frequency_hz;
    if (divisor > 0xFFFF)
        divisor = 0xFFFF;       // slowest the hardware can go, ~18.2 Hz
    if (divisor == 0)
        divisor = 1;

    g_frequency = kBaseFrequency / divisor;
    g_ticks = 0;

    // 0x36: channel 0, access mode lo/hi, mode 3 (square wave), binary.
    io::out8(kCommand, 0x36);
    io::out8(kChannel0, static_cast<u8>(divisor & 0xFF));
    io::out8(kChannel0, static_cast<u8>(divisor >> 8));

    interrupts::register_irq(interrupts::kIrqTimer, on_tick);
    pic::unmask(interrupts::kIrqTimer);
}

u64 ticks()
{
    return g_ticks;
}

u64 uptime_ms()
{
    // The HPET counts at tens of MHz; the tick counter only resolves to a
    // scheduling quantum. Prefer the real clock when the machine has one.
    if (hpet::available())
        return hpet::uptime_us() / 1000;
    return g_ticks * 1000 / g_frequency;
}

void sleep_ms(u64 milliseconds)
{
    const u64 target = g_ticks + milliseconds * g_frequency / 1000;
    while (g_ticks < target)
        asm volatile("hlt");
}

} // namespace timer
