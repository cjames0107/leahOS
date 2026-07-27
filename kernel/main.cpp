#include <leah/acpi.hpp>
#include <leah/apic.hpp>
#include <leah/ata.hpp>
#include <leah/blockdev.hpp>
#include <leah/bootinfo.hpp>
#include <leah/console.hpp>
#include <leah/cpu.hpp>
#include <leah/elf.hpp>
#include <leah/syscall.hpp>
#include <leah/framebuffer.hpp>
#include <leah/ext.hpp>
#include <leah/fat32.hpp>
#include <leah/gdt.hpp>
#include <leah/heap.hpp>
#include <leah/hpet.hpp>
#include <leah/interrupts.hpp>
#include <leah/keyboard.hpp>
#include <leah/memory.hpp>
#include <leah/mouse.hpp>
#include <leah/e1000.hpp>
#include <leah/net.hpp>
#include <leah/panic.hpp>
#include <leah/pci.hpp>
#include <leah/pic.hpp>
#include <leah/pmm.hpp>
#include <leah/process.hpp>
#include <leah/scheduler.hpp>
#include <leah/timer.hpp>
#include <leah/types.hpp>
#include <leah/string.hpp>
#include <leah/vfs.hpp>
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

extern "C" u8 __kernel_start[];
extern "C" u8 __kernel_end[];

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

void print_memory(const boot::Info& info)
{
    console::set_color(console::Color::White);
    console::printf("\n  physical memory  %llu MiB usable, top of RAM at %p\n",
                    pmm::usable_bytes() / kMiB,
                    reinterpret_cast<void*>(pmm::highest_usable()));
    console::set_color(console::Color::LightGray);
    console::printf("    %llu MiB free, %llu MiB reserved, across %u E820 regions\n",
                    pmm::free_bytes() / kMiB, pmm::used_bytes() / kMiB, info.e820_count);
    console::printf("    kernel at %p virtual, %p physical, %llu KiB\n",
                    static_cast<void*>(__kernel_start),
                    reinterpret_cast<void*>(memory::kernel_virt_to_phys(
                        reinterpret_cast<u64>(__kernel_start))),
                    (reinterpret_cast<u64>(__kernel_end) -
                     reinterpret_cast<u64>(__kernel_start)) / 1024);
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

void print_disks()
{
    console::set_color(console::Color::White);
    console::printf("\n  ATA  %llu drives\n", static_cast<u64>(ata::drive_count()));
    console::set_color(console::Color::LightGray);

    for (usize i = 0; i < ata::drive_count(); ++i) {
        const ata::Drive& d = ata::drive_at(i);
        console::printf("    hd%llu  %llu MiB  %s  %s\n",
                        static_cast<u64>(i),
                        ata::capacity_bytes(i) / kMiB,
                        d.lba48 ? "LBA48" : "LBA28",
                        d.model);
    }
}

// The strongest check available right now: the kernel was loaded from LBA 64 by
// the bootloader, so reading those same sectors back through an entirely
// different code path must reproduce the image already in memory. It validates
// the ATA driver and the unreal-mode loader against each other.
void self_test_disk()
{
    if (ata::drive_count() == 0)
        panic("ata: no drives found, but we booted off one");

    auto* sector = static_cast<u8*>(kmalloc(ata::kSectorSize));
    if (sector == nullptr)
        panic("ata: out of memory for a sector buffer");

    if (!ata::read(0, 0, 1, sector))
        panic("ata: cannot read LBA 0");
    if (sector[510] != 0x55 || sector[511] != 0xAA)
        panic("ata: LBA 0 is not the boot sector we wrote");
    kfree(sector);

    constexpr u32 kSectors = 32;      // 16 KiB of kernel
    auto* image = static_cast<u8*>(kmalloc(kSectors * ata::kSectorSize));
    if (image == nullptr)
        panic("ata: out of memory for the verification buffer");

    if (!ata::read(0, 64, kSectors, image))
        panic("ata: cannot read the kernel image back");
    if (memcmp(image, __kernel_start, kSectors * ata::kSectorSize) != 0)
        panic("ata: kernel read from disk does not match the loaded image");

    kfree(image);

    // Writes get their own check: they are the operation that can quietly
    // corrupt the disk, and the cache-flush step is easy to get wrong. Use a
    // scratch LBA well past the kernel's slot in the image.
    constexpr u64 kScratchLba = 20000;
    auto* out = static_cast<u8*>(kmalloc(ata::kSectorSize));
    auto* back = static_cast<u8*>(kmalloc(ata::kSectorSize));
    if (out == nullptr || back == nullptr)
        panic("ata: out of memory for the write test");

    for (usize i = 0; i < ata::kSectorSize; ++i)
        out[i] = static_cast<u8>(i * 7 + 3);

    if (!ata::write(0, kScratchLba, 1, out))
        panic("ata: write failed");

    memset(back, 0, ata::kSectorSize);
    if (!ata::read(0, kScratchLba, 1, back))
        panic("ata: cannot read back what we just wrote");
    if (memcmp(out, back, ata::kSectorSize) != 0)
        panic("ata: read-back does not match the data written");

    kfree(back);
    kfree(out);
}

block::Device* g_disk = nullptr;
block::Device* g_partition = nullptr;

void mount_root()
{
    // The root filesystem is an ext4 volume on disk 1 (the whole disk, no
    // partition table). Disk 0 remains the bootable/kernel disk; its FAT32
    // partition is the fallback if the ext disk is absent.
    if (ata::drive_count() > 1) {
        auto* ext_disk = new block::AtaDevice(1);
        fs::Ext* ext = fs::Ext::probe(ext_disk);
        if (ext != nullptr) {
            g_disk = ext_disk;
            vfs::mount(ext);
            console::set_color(console::Color::White);
            console::printf("\n  root  ext4 on hd1, label \"%s\"\n", ext->volume_label());
            console::set_color(console::Color::LightGray);
            return;
        }
        delete ext_disk;
    }

    g_disk = new block::AtaDevice(0);

    block::PartitionInfo parts[4];
    const usize found = block::scan_partitions(*g_disk, parts, 4);

    console::set_color(console::Color::White);
    console::printf("\n  partitions  %llu found\n", static_cast<u64>(found));
    console::set_color(console::Color::LightGray);

    for (usize i = 0; i < found; ++i) {
        console::printf("    %llu  type 0x%02x  %s  LBA %llu, %llu MiB\n",
                        static_cast<u64>(i), parts[i].type,
                        block::partition_type_name(parts[i].type),
                        parts[i].start_lba,
                        parts[i].sectors * block::kSectorSize / kMiB);
    }

    for (usize i = 0; i < found; ++i) {
        if (parts[i].type != 0x0B && parts[i].type != 0x0C)
            continue;

        g_partition = new block::Partition(g_disk, parts[i].start_lba, parts[i].sectors);
        fs::Fat32* filesystem = fs::Fat32::probe(g_partition);
        if (filesystem == nullptr) {
            delete g_partition;
            g_partition = nullptr;
            continue;
        }
        vfs::mount(filesystem);
        return;
    }
}

void print_tree(const char* path, int depth)
{
    vfs::Entry entries[32];
    usize count = 0;
    if (!vfs::list(path, entries, 32, count))
        return;

    for (usize i = 0; i < count; ++i) {
        for (int d = 0; d < depth; ++d)
            console::write("  ");

        if (entries[i].type == vfs::Type::Directory) {
            console::set_color(console::Color::LightCyan);
            console::printf("    %s/\n", entries[i].name);
            console::set_color(console::Color::LightGray);

            char child[vfs::kMaxPath];
            usize n = 0;
            for (const char* p = path; *p != '\0' && n + 1 < sizeof(child); ++p)
                child[n++] = *p;
            if (n > 0 && child[n - 1] != '/')
                child[n++] = '/';
            for (const char* p = entries[i].name; *p != '\0' && n + 1 < sizeof(child); ++p)
                child[n++] = *p;
            child[n] = '\0';

            print_tree(child, depth + 1);
        } else {
            console::printf("    %s  (%llu bytes)\n", entries[i].name, entries[i].size);
        }
    }
}

// Reads the three cases that catch a subtly broken block map: a file smaller
// than one block, a file spanning several blocks (which exercises the extent or
// indirect map), and a name reached through a subdirectory. The fixtures come
// from tools/mkext.sh.
void self_test_fs()
{
    if (vfs::mounted() == nullptr)
        panic("vfs: no filesystem mounted");

    u64 size = 0;
    char* hello = vfs::read_entire_file("/HELLO.TXT", &size);
    if (hello == nullptr || size != 17)
        panic("ext: /HELLO.TXT did not read back at its stated size");
    if (memcmp(hello, "Hello from ext4.\n", 17) != 0)
        panic("ext: /HELLO.TXT contents are wrong");
    kfree(hello);

    // Larger than one 4 KiB block, so this only passes if the block map walks
    // past the first block correctly.
    vfs::Stat readme{};
    if (!vfs::stat("/README.MD", readme) || readme.size < 4096)
        panic("ext: /README.MD is missing or unexpectedly small");

    char* text = vfs::read_entire_file("/README.MD", &size);
    if (text == nullptr || size != readme.size)
        panic("ext: short read on a multi-block file");
    if (memcmp(text, "# leahOS", 8) != 0)
        panic("ext: multi-block read returned the wrong bytes");
    // The tail matters more than the head: a broken map still gets block 0 right.
    if (memcmp(text + size - 8, "block. \n", 8) != 0)
        panic("ext: the end of a multi-block file is wrong");
    kfree(text);

    // A path through a subdirectory.
    char* notes = vfs::read_entire_file("/docs/notes.txt", &size);
    if (notes == nullptr || size != 30)
        panic("ext: /docs/notes.txt did not resolve");
    kfree(notes);

    if (vfs::stat("/nope.txt", readme))
        panic("ext: stat succeeded on a file that does not exist");
}

// Writing is where a filesystem gets to corrupt itself, so this covers the
// cases that actually allocate on ext: a fresh inode, a file that spans several
// blocks (its extent has to grow), a subdirectory with its "." and ".." set up,
// and deletion returning inodes and blocks to the bitmaps. tools/fsck-ext.sh
// then has e2fsck confirm the volume is still consistent afterwards.
void self_test_fs_write()
{
    // Be idempotent: fsck-ext.sh runs on a persistent disk, so a previous boot
    // may have left these behind and create() of an existing name must fail.
    vfs::Stat leftover{};
    if (vfs::stat("/OUT/nested.txt", leftover))
        vfs::remove("/OUT/nested.txt");
    if (vfs::stat("/OUT", leftover))
        vfs::remove("/OUT");

    static const char kSmall[] = "written by leahOS\n";
    if (!vfs::write_entire_file("/wrote.txt", kSmall, sizeof(kSmall) - 1))
        panic("ext: could not create /wrote.txt");

    u64 size = 0;
    char* back = vfs::read_entire_file("/wrote.txt", &size);
    if (back == nullptr || size != sizeof(kSmall) - 1 ||
        memcmp(back, kSmall, size) != 0)
        panic("ext: /wrote.txt did not read back as written");
    kfree(back);

    // Several 4 KiB blocks, so block allocation and extent growth both run.
    constexpr usize kBigSize = 9000;
    auto* big = static_cast<u8*>(kmalloc(kBigSize));
    if (big == nullptr)
        panic("ext: out of memory for the write test");
    for (usize i = 0; i < kBigSize; ++i)
        big[i] = static_cast<u8>(i * 31 + 7);

    if (!vfs::write_entire_file("/BIG.BIN", big, kBigSize))
        panic("ext: could not write a multi-block file");

    auto* big_back = static_cast<u8*>(kmalloc(kBigSize));
    if (big_back == nullptr)
        panic("ext: out of memory verifying the write test");
    if (vfs::read("/BIG.BIN", 0, big_back, kBigSize) != static_cast<isize>(kBigSize))
        panic("ext: short read on a file we just wrote");
    if (memcmp(big, big_back, kBigSize) != 0)
        panic("ext: multi-block write did not round trip");
    kfree(big_back);
    kfree(big);

    // Directory creation, including its "." and ".." entries, and a file in it.
    if (!vfs::create("/OUT", vfs::Type::Directory))
        panic("ext: could not create a directory");
    if (!vfs::write_entire_file("/OUT/nested.txt", kSmall, sizeof(kSmall) - 1))
        panic("ext: could not write inside a created directory");

    char* nested = vfs::read_entire_file("/OUT/nested.txt", &size);
    if (nested == nullptr || size != sizeof(kSmall) - 1)
        panic("ext: file in a created directory did not read back");
    kfree(nested);

    // Deletion, and the inode/blocks coming back to the bitmaps.
    if (!vfs::write_entire_file("/temp.txt", kSmall, sizeof(kSmall) - 1))
        panic("ext: could not create the file to be deleted");
    if (!vfs::remove("/temp.txt"))
        panic("ext: remove failed");
    vfs::Stat gone{};
    if (vfs::stat("/temp.txt", gone))
        panic("ext: a removed file is still there");

    // A non-empty directory must refuse to disappear and orphan its contents.
    if (vfs::remove("/OUT"))
        panic("ext: removed a directory that still had a file in it");

    // Rename is a re-link, no data copy: move the nested file up a level.
    if (!vfs::rename("/OUT/nested.txt", "/moved.txt"))
        panic("ext: rename failed");
    if (vfs::stat("/OUT/nested.txt", gone))
        panic("ext: rename left the source behind");
    char* moved = vfs::read_entire_file("/moved.txt", &size);
    if (moved == nullptr || size != sizeof(kSmall) - 1 ||
        memcmp(moved, kSmall, size) != 0)
        panic("ext: renamed file did not read back");
    kfree(moved);

    // Clean up so a persistent re-run starts fresh and e2fsck sees no leftovers
    // beyond a consistent volume.
    vfs::remove("/moved.txt");
    vfs::remove("/OUT");
    vfs::remove("/wrote.txt");
    vfs::remove("/BIG.BIN");
}

void print_filesystem()
{
    auto* filesystem = vfs::mounted();
    if (filesystem == nullptr) {
        console::write("\n  no filesystem mounted\n");
        return;
    }

    console::set_color(console::Color::White);
    console::printf("\n  %s  volume \"%s\"\n",
                    filesystem->type_name(), filesystem->volume_label());
    console::set_color(console::Color::LightGray);
    print_tree("/", 0);
}

// Loads a real ELF off the filesystem and calls it. Still ring 0 and still the
// Resolves the gateway's MAC over ARP - proving the NIC transmits (the request)
// and receives (the reply), and that the ARP layer works, all against QEMU's
// virtual network.
void self_test_network()
{
    u8 gateway_mac[net::kMacLength];
    if (net::arp_resolve(net::kGatewayIp, gateway_mac)) {
        console::set_color(console::Color::DarkGray);
        console::write("  network: ARP resolved the gateway\n");
        net::arp_print_cache();
        console::set_color(console::Color::LightGray);
    } else {
        console::set_color(console::Color::LightRed);
        console::write("  network: gateway did not answer ARP\n");
        console::set_color(console::Color::LightGray);
    }
}

// Runs the init process and waits for it. init drives the fork/exec/wait demo;
// this is also the moment the kernel first hands the CPU to a scheduled user
// process rather than running one synchronously.
void run_userland()
{
    console::set_color(console::Color::White);
    console::write("\n  starting /BIN/INIT.ELF\n\n");
    console::set_color(console::Color::LightGray);

    // With real processes that block on I/O, the scheduler needs something to
    // run when everyone is asleep.
    scheduler::start_idle();

    const u32 pid = process::create("init", "/BIN/INIT.ELF", scheduler::current_pid());
    if (pid == 0)
        panic("could not create the init process");

    i32 status = 0;
    const i64 reaped = scheduler::wait_child(&status);

    console::set_color(console::Color::DarkGray);
    console::printf("\n  kernel: init (pid %lld) exited, status 0x%x\n", reaped, status);
    console::set_color(console::Color::LightGray);
}

// --- preemptive scheduler self-test ----------------------------------------
//
// Several workers spin in tight loops that never yield. The only thing that can
// move the CPU between them is the timer preempting one mid-loop, so if every
// worker makes progress - and the interleave trace shows the CPU bouncing
// between them - preemption is real. main spins too, without yielding, so its
// own survival to the end is proof the scheduler switches back as well.

constexpr usize kWorkers = 3;
volatile u64  g_work_counter[kWorkers];
volatile bool g_work_stop = false;

volatile u8  g_interleave[256];
volatile u32 g_interleave_len = 0;

void worker_thread(void* arg)
{
    const u64 id = reinterpret_cast<u64>(arg);
    while (!g_work_stop) {
        g_work_counter[id] = g_work_counter[id] + 1;

        // Record only when execution has moved to a different worker since the
        // last entry, so the log is a trace of preemption switches rather than
        // of loop iterations. Single core, so no two workers race here.
        const u32 len = g_interleave_len;
        if ((len == 0 || g_interleave[len - 1] != id) && len < 255) {
            g_interleave[len] = static_cast<u8>(id);
            g_interleave_len = len + 1;
        }
    }
    scheduler::exit_current(0);
}

void self_test_scheduler()
{
    scheduler::init();

    for (usize i = 0; i < kWorkers; ++i)
        g_work_counter[i] = 0;
    g_work_stop = false;
    g_interleave_len = 0;

    for (u64 i = 0; i < kWorkers; ++i) {
        if (scheduler::spawn("worker", worker_thread, reinterpret_cast<void*>(i)) == 0)
            panic("scheduler: could not spawn a worker");
    }

    scheduler::start_preemption();

    // Spin without yielding until the interleave trace shows several switches
    // among the workers. If preemption were broken this would spin forever, so
    // it is bounded; the bound is generous because TCG advances guest time
    // slowly under a full load of spinning threads.
    bool ok = false;
    for (u64 i = 0; i < 20'000'000'000ull; ++i) {
        if (g_interleave_len >= 12) {
            ok = true;
            break;
        }
        asm volatile("pause");
    }

    g_work_stop = true;
    while (scheduler::alive_count() > 1)
        scheduler::yield();

    if (!ok)
        panic("scheduler: workers did not interleave - preemption is not working");

    u32 transitions = 0;
    for (u32 i = 1; i < g_interleave_len; ++i) {
        if (g_interleave[i] != g_interleave[i - 1])
            ++transitions;
    }
    for (usize i = 0; i < kWorkers; ++i) {
        if (g_work_counter[i] == 0)
            panic("scheduler: a worker thread never got the CPU");
    }

    console::set_color(console::Color::DarkGray);
    console::printf("\n  scheduler: %llu threads time-sliced, %u preemptive switches\n",
                    static_cast<u64>(kWorkers) + 1, transitions);
    console::printf("    interleave ");
    for (u32 i = 0; i < g_interleave_len && i < 28; ++i)
        console::printf("%u", g_interleave[i]);
    console::write("...\n");
    for (usize i = 0; i < kWorkers; ++i)
        console::printf("    worker %llu ran %llu iterations\n",
                        static_cast<u64>(i), g_work_counter[i]);
    console::set_color(console::Color::LightGray);
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

void run_global_constructors();

extern "C" void kernel_main(const boot::Info* boot_info)
{
    // The boot info and E820 map live in low physical memory the bootloader
    // left behind. vmm::init() will unmap the low half, so copy what is needed
    // later into the kernel image now, while the stage-2 identity map still
    // makes it reachable.
    static boot::Info info_copy;
    info_copy = *boot_info;
    const boot::Info* info = &info_copy;

    console::init(*info);

    // Before anything else that could depend on a global: constructors here
    // run with no heap, so they must not allocate.
    run_global_constructors();

    print_banner();

    // Descriptor tables first: everything below can fault, and a fault before
    // the IDT exists is a triple fault with no diagnostic.
    gdt::init();
    step("GDT + TSS installed, IST1 armed for #DF");

    interrupts::init();
    step("IDT installed, 256 vectors");

    // Memory next, because every driver past this point wants to allocate.
    pmm::init(*info);
    step("physical frame allocator online");

    vmm::init();
    pmm::use_direct_map();          // rebase the frame bitmap onto the direct map
    framebuffer::remap_as_device(); // and the console onto it, before we print
    step("page tables rebuilt, kernel in the higher half, low half is user's");

    heap::init();
    pmm::init_refcounts();
    self_test_heap();
    step("kernel heap online, self-test passed");

    pic::init();
    pic::mask_all();
    step("PIC remapped to vectors 32-47");

    timer::init();
    step("PIT running at 100 Hz on IRQ 0");

    // ACPI names the interrupt controllers and the HPET. With those in hand the
    // legacy pair can be retired: the I/O APIC routes device interrupts and the
    // local APIC timer raises the scheduling tick, one per CPU rather than one
    // shared chip - which is what makes SMP possible at all.
    if (acpi::init()) {
        console::printf("  [ ok ] ACPI: %llu tables, %llu CPU(s), %llu I/O APIC(s)\n",
                        static_cast<u64>(acpi::table_count()),
                        static_cast<u64>(acpi::cpu_count()),
                        static_cast<u64>(acpi::io_apic_count()));

        if (hpet::init()) {
            console::printf("  [ ok ] HPET at %llu MHz, %u fs per tick\n",
                            hpet::frequency_hz() / 1000000, hpet::period_fs());
        }

        if (apic::init()) {
            console::printf("  [ ok ] local APIC id %u up, PIC masked\n",
                            apic::local_id());
            // The keyboard has to be re-routed: masking the PIC took its line
            // away, and the I/O APIC starts with everything masked.
            // Every device line has to be re-opened: masking the PIC took them
            // all away, and the I/O APIC starts with everything masked.
            constexpr u8 kIrqMouse = 12;
            const bool kbd = apic::route_irq(
                interrupts::kIrqKeyboard,
                interrupts::kIrqBase + interrupts::kIrqKeyboard);
            const bool mouse = apic::route_irq(kIrqMouse,
                                               interrupts::kIrqBase + kIrqMouse);
            if (kbd)
                console::printf("  [ ok ] I/O APIC routing IRQ 1 (keyboard)%s\n",
                                mouse ? ", IRQ 12 (mouse)" : "");

            // Same vector the PIT used, so the existing tick handler is reached
            // unchanged - only the source of the tick has moved.
            if (apic::start_timer(interrupts::kIrqBase + interrupts::kIrqTimer,
                                  timer::kFrequencyHz)) {
                console::printf("  [ ok ] local APIC timer at %llu Hz "
                                "(core clock %llu MHz), PIT retired\n",
                                static_cast<u64>(timer::kFrequencyHz),
                                apic::timer_frequency() / 1000000);
            }
        }
    } else {
        step("no ACPI tables found - staying on the PIC and PIT");
    }

    keyboard::init();
    step("PS/2 keyboard on IRQ 1");

    mouse::init();
    step("PS/2 mouse on IRQ 12");

    pci::enumerate();
    step("PCI bus enumerated");

    ata::init();
    self_test_disk();
    step("ATA drives identified, read-back verified against the loaded kernel");

    cpu::sti();
    step("interrupts enabled");

    if (console::graphical()) {
        console::printf("  [ ok ] framebuffer %ux%u, %llux%llu text, BIOS ROM font\n",
                        framebuffer::width(), framebuffer::height(),
                        static_cast<u64>(console::columns()),
                        static_cast<u64>(console::rows()));
    }

    mount_root();
    self_test_fs();
    step("ext4 root mounted, read path verified");

    self_test_fs_write();
    step("ext4 write path verified (create, extent grow, mkdir, remove, rename)");

    syscall::init();
    step("SYSCALL/SYSRET enabled, ring 3 ready");

    if (net::init()) {
        const u8* m = net::mac();
        console::printf("  [ ok ] e1000 up, MAC %02x:%02x:%02x:%02x:%02x:%02x, "
                        "IP 10.0.2.15\n", m[0], m[1], m[2], m[3], m[4], m[5]);
        self_test_network();
    } else {
        step("no e1000 NIC found - networking disabled");
    }

    self_test_scheduler();
    step("preemptive scheduler: kernel threads time-sliced by the PIT");

    run_userland();
    step("userland: init forked, exec'd and waited on a child");

    print_memory(*info);
    print_pci();
    print_disks();
    print_filesystem();

    echo_loop();
}
