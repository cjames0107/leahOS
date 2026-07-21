#include <leah/bootinfo.hpp>
#include <leah/console.hpp>
#include <leah/cpu.hpp>
#include <leah/gdt.hpp>
#include <leah/heap.hpp>
#include <leah/interrupts.hpp>
#include <leah/keyboard.hpp>
#include <leah/mouse.hpp>
#include <leah/panic.hpp>
#include <leah/pci.hpp>
#include <leah/pic.hpp>
#include <leah/pmm.hpp>
#include <leah/timer.hpp>
#include <leah/types.hpp>
#include <leah/vmm.hpp>

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

constexpr u64 kMiB = 1024 * 1024;

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

void print_memory(const boot::MemoryMap& map)
{
    console::set_color(console::Color::White);
    console::printf("\n  physical memory  %llu MiB usable, top of RAM at %p\n",
                    pmm::usable_bytes() / kMiB,
                    reinterpret_cast<void*>(pmm::highest_usable()));
    console::set_color(console::Color::LightGray);
    console::printf("    %llu MiB free, %llu MiB reserved, across %u E820 regions\n",
                    pmm::free_bytes() / kMiB, pmm::used_bytes() / kMiB, map.count);
    console::printf("    page tables at %p, heap %llu KiB\n",
                    reinterpret_cast<void*>(vmm::kernel_page_table()),
                    static_cast<u64>(heap::heap_size()) / 1024);
}

void print_pci()
{
    console::set_color(console::Color::White);
    console::printf("\n  PCI  %llu devices\n", static_cast<u64>(pci::device_count()));
    console::set_color(console::Color::LightGray);

    for (usize i = 0; i < pci::device_count(); ++i) {
        const pci::Device& d = pci::device_at(i);
        console::printf("    %02x:%02x.%u  %04x:%04x  %s\n",
                        d.bus, d.slot, d.function, d.vendor_id, d.device_id,
                        pci::class_name(d.class_code, d.subclass));
    }
}

// Exercises split, reuse and coalescing rather than just proving that a single
// allocation returns non-null.
void self_test_heap()
{
    const usize before = heap::used_bytes();

    void* a = kmalloc(64);
    void* b = kmalloc(4096);
    void* c = kmalloc(17);
    if (a == nullptr || b == nullptr || c == nullptr)
        panic("heap: allocation returned null");

    // Writing the whole span catches a size field that lied about capacity.
    for (usize i = 0; i < 4096; ++i)
        static_cast<u8*>(b)[i] = static_cast<u8>(i);
    for (usize i = 0; i < 4096; ++i) {
        if (static_cast<u8*>(b)[i] != static_cast<u8>(i))
            panic("heap: allocation does not survive being written to");
    }

    kfree(b);
    void* d = kmalloc(4096);      // should land back in the block just freed
    if (d != b)
        panic("heap: freed block was not reused");

    kfree(d);
    kfree(c);
    kfree(a);

    if (heap::used_bytes() != before)
        panic("heap: bytes leaked across the self-test");

    void* big = kmalloc(256 * 1024);   // forces the heap to grow
    if (big == nullptr)
        panic("heap: cannot satisfy an allocation larger than the initial region");
    kfree(big);
}

void echo_loop()
{
    console::set_color(console::Color::White);
    console::write("\n  type something:\n\n  ");
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

    // Descriptor tables first: everything below can fault, and a fault before
    // the IDT exists is a triple fault with no diagnostic.
    gdt::init();
    step("GDT + TSS installed, IST1 armed for #DF");

    interrupts::init();
    step("IDT installed, 256 vectors");

    // Memory next, because every driver past this point wants to allocate.
    pmm::init(*memory_map);
    step("physical frame allocator online");

    vmm::init();
    step("page tables rebuilt, 4 GiB identity mapped, NX enabled");

    heap::init();
    self_test_heap();
    step("kernel heap online, self-test passed");

    pic::init();
    pic::mask_all();
    step("PIC remapped to vectors 32-47");

    timer::init();
    step("PIT running at 100 Hz on IRQ 0");

    keyboard::init();
    step("PS/2 keyboard on IRQ 1");

    mouse::init();
    step("PS/2 mouse on IRQ 12");

    pci::enumerate();
    step("PCI bus enumerated");

    cpu::sti();
    step("interrupts enabled");

    print_memory(*memory_map);
    print_pci();

    echo_loop();
}
