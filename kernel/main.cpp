#include <leah/bootinfo.hpp>
#include <leah/console.hpp>
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

void print_memory_map(const boot::MemoryMap& map)
{
    console::set_color(console::Color::White);
    console::printf("  E820 memory map (%u entries)\n", map.count);
    console::set_color(console::Color::LightGray);

    u64 usable = 0;
    u64 total  = 0;

    for (u32 i = 0; i < map.count; ++i) {
        const boot::MemoryRegion& r = map.regions[i];

        console::printf("    [%02u] %016llx - %016llx  %s\n",
                        i, r.base, r.base + r.length,
                        boot::region_type_name(r.type));

        total += r.length;
        if (r.type == boot::RegionType::Usable)
            usable += r.length;
    }

    console::write("\n");
    console::set_color(console::Color::LightGreen);
    console::printf("  usable: %llu MiB", usable / (1024 * 1024));
    console::set_color(console::Color::DarkGray);
    console::printf("   (of %llu MiB addressed)\n", total / (1024 * 1024));
    console::set_color(console::Color::LightGray);
}

} // namespace

extern "C" void kernel_main(const boot::MemoryMap* memory_map)
{
    console::init();

    print_banner();
    print_memory_map(*memory_map);

    console::set_color(console::Color::DarkGray);
    console::write("\n  long mode reached. nothing left to do yet - halting.\n");

    for (;;)
        asm volatile("cli; hlt");
}
