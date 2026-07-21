#include <leah/bootinfo.hpp>
#include <leah/console.hpp>
#include <leah/cpu.hpp>
#include <leah/gdt.hpp>
#include <leah/interrupts.hpp>
#include <leah/keyboard.hpp>
#include <leah/panic.hpp>
#include <leah/pic.hpp>
#include <leah/timer.hpp>
#include <leah/types.hpp>

namespace boot {

const char* region_type_name(RegionType type)
{
    switch (type) {
    case RegionType::Usable:          return "usable";
    case RegionType::Reserved:        return "reserved";
    case RegionType::AcpiReclaimable: return "ACPI reclaim";
    case RegionType::AcpiNvs:         return "ACPI NVS";
    case RegionType::Bad:             return "bad";
    }
    return "unknown";
}

} // namespace boot

namespace {

void print_banner()
{
    console::set_color(console::Color::LightCyan);
    console::write("\n  leahOS");
    console::set_color(console::Color::DarkGray);
    console::write("  x86_64  //  pre-alpha\n\n");
    console::set_color(console::Color::LightGray);
}

void step(const char* what)
{
    console::set_color(console::Color::LightGreen);
    console::write("  [ ok ] ");
    console::set_color(console::Color::LightGray);
    console::printf("%s\n", what);
}

void print_memory_map(const boot::MemoryMap& map)
{
    u64 usable = 0;
    for (u32 i = 0; i < map.count; ++i) {
        const boot::MemoryRegion& r = map.regions[i];
        if (r.type == boot::RegionType::Usable)
            usable += r.length;
    }

    console::set_color(console::Color::LightGreen);
    console::write("  [ ok ] ");
    console::set_color(console::Color::LightGray);
    console::printf("memory map: %u regions, %llu MiB usable\n",
                    map.count, usable / (1024 * 1024));
}

void echo_loop()
{
    console::set_color(console::Color::White);
    console::write("\n  type something (interrupts are live):\n\n  ");
    console::set_color(console::Color::LightGray);

    for (;;) {
        const char c = keyboard::read_blocking();
        if (c == '\n')
            console::write("\n  ");
        else
            console::put(c);
    }
}

} // namespace

extern "C" void kernel_main(const boot::MemoryMap* memory_map)
{
    console::init();
    print_banner();

    print_memory_map(*memory_map);

    // Order matters. The GDT has to be ours before the IDT references its code
    // selector, and the PIC has to be remapped off vectors 8-15 before the
    // first sti or a timer tick would arrive as a double fault.
    gdt::init();
    step("GDT + TSS installed, IST1 armed for #DF");

    interrupts::init();
    step("IDT installed, 256 vectors");

    pic::init();
    pic::mask_all();
    step("PIC remapped to vectors 32-47, all lines masked");

    timer::init();
    step("PIT running at 100 Hz on IRQ 0");

    keyboard::init();
    step("PS/2 keyboard on IRQ 1");

    cpu::sti();
    step("interrupts enabled");

    // Proof the timer is really firing: this only returns if IRQ 0 is
    // incrementing the tick count behind us.
    const u64 before = timer::ticks();
    timer::sleep_ms(500);
    console::set_color(console::Color::DarkGray);
    console::printf("\n  slept 500 ms across %llu timer ticks, uptime %llu ms\n",
                    timer::ticks() - before, timer::uptime_ms());
    console::set_color(console::Color::LightGray);

    // Drive the decoder directly: proves the scancode tables, the shift
    // handling and the ring buffer all work, independently of whether the
    // emulator can inject key events.
    {
        static const u8 kSelfTest[] = {
            0x26, 0x12, 0x1E, 0x23,     // l e a h
            0x2A, 0x18, 0x1F, 0xAA,     // shift down, o, s, shift up
        };
        for (u8 code : kSelfTest)
            keyboard::inject_scancode(code);

        console::set_color(console::Color::DarkGray);
        console::write("\n  keyboard decoder self-test: \"");
        console::set_color(console::Color::LightGreen);
        while (keyboard::has_input())
            console::put(keyboard::read());
        console::set_color(console::Color::DarkGray);
        console::write("\"\n");
        console::set_color(console::Color::LightGray);
    }
    echo_loop();
}
