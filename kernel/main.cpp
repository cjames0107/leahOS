#include <leah/acpi.hpp>
#include <leah/apic.hpp>
#include <leah/bootinfo.hpp>
#include <leah/clock.hpp>
#include <leah/console.hpp>
#include <leah/cpu.hpp>
#include <leah/syscall.hpp>
#include <leah/framebuffer.hpp>
#include <leah/fpu.hpp>
#include <leah/gdt.hpp>
#include <leah/heap.hpp>
#include <leah/hpet.hpp>
#include <leah/interrupts.hpp>
#include <leah/keyboard.hpp>
#include <leah/memory.hpp>
#include <leah/mouse.hpp>
#include <leah/ipc.hpp>
#include <leah/panic.hpp>
#include <leah/percpu.hpp>
#include <leah/pic.hpp>
#include <leah/pmm.hpp>
#include <leah/process.hpp>
#include <leah/scheduler.hpp>
#include <leah/image.hpp>
#include <leah/object.hpp>
#include <leah/region.hpp>
#include <leah/tunable.hpp>
#include <leah/shm.hpp>
#include <leah/smp.hpp>
#include <leah/timer.hpp>
#include <leah/types.hpp>
#include <leah/string.hpp>
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

// The two programs the kernel carries, from servers.asm.
extern "C" const u8 g_server_ahcid[];
extern "C" const u8 g_server_ahcid_end[];
extern "C" const u8 g_server_blockd[];
extern "C" const u8 g_server_blockd_end[];
extern "C" const u8 g_server_vfsd[];
extern "C" const u8 g_server_vfsd_end[];
extern "C" const u8 g_server_init[];
extern "C" const u8 g_server_init_end[];

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

// The same shape as the ATA check: write a pattern well clear of anything that
// matters, read it back through the controller, and compare. A DMA path can
// Loads a real ELF off the filesystem and calls it. Still ring 0 and still the
// Resolves the gateway's MAC over ARP - proving the NIC transmits (the request)
// and receives (the reply), and that the ARP layer works, all against QEMU's
// virtual network.

// Runs the init process and waits for it. init drives the fork/exec/wait demo;
// this is also the moment the kernel first hands the CPU to a scheduled user
// process rather than running one synchronously.
// Start the disk driver and the filesystem, in that order, and wait for the
// filesystem to say it is open for business.
//
// Nothing can be loaded from disk until both are up, including init - so this
// is the one place in the system where the order is not a matter of taste. The
// wait is on the port rather than on a delay: a port appearing is the server
// saying it is ready, and a delay is a guess that is too long on a fast
// machine and too short on a slow one.
bool start_servers()
{
    const usize blockd_size =
        static_cast<usize>(g_server_blockd_end - g_server_blockd);
    const usize vfsd_size =
        static_cast<usize>(g_server_vfsd_end - g_server_vfsd);

    if (process::create_embedded("blockd", g_server_blockd, blockd_size,
                                 scheduler::current_pid()) == 0) {
        console::printf("  kernel: cannot start the disk driver\n");
        return false;
    }
    for (u32 i = 0; i < 20000 && ipc::port_open(ipc::kPortBlock) < 0; ++i)
        scheduler::yield();
    if (ipc::port_open(ipc::kPortBlock) < 0) {
        console::printf("  kernel: the disk driver never claimed its port\n");
        return false;
    }

    // The other controller, before the filesystem rather than after it: the
    // root volume may be on this one, and a driver started afterwards is a
    // driver that was not there when the mount happened. It is allowed to fail
    // - a machine with no AHCI controller is a machine that boots from the
    // other one - so nothing is waited for and nothing is checked.
    const usize ahcid_size =
        static_cast<usize>(g_server_ahcid_end - g_server_ahcid);
    process::create_embedded("ahcid", g_server_ahcid, ahcid_size,
                             scheduler::current_pid());
    for (u32 i = 0; i < 20000 && ipc::port_open(ipc::kPortBlock2) < 0; ++i)
        scheduler::yield();

    if (process::create_embedded("vfsd", g_server_vfsd, vfsd_size,
                                 scheduler::current_pid()) == 0) {
        console::printf("  kernel: cannot start the filesystem\n");
        return false;
    }
    for (u32 i = 0; i < 20000 && ipc::port_open(ipc::kPortVfs) < 0; ++i)
        scheduler::yield();
    if (ipc::port_open(ipc::kPortVfs) < 0) {
        console::printf("  kernel: the filesystem never mounted\n");
        return false;
    }
    return true;
}

void run_userland()
{
    if (!start_servers())
        panic("no filesystem: nothing can be loaded");

    /* The kernel used to verify the filesystem here, back when it had a
     * client to verify it with. It has none: reading a file is not something
     * the kernel does any more, and a test it cannot perform is a test that
     * belongs to whoever can. `tests` covers ext4 from ring 3, which is also
     * the only place it matters. */

    console::set_color(console::Color::White);
    console::write("\n  starting init\n\n");
    console::set_color(console::Color::LightGray);

    /* Carried in the kernel image like the two servers. Not because init is
     * needed before there is a filesystem - there is one by now - but because
     * exec takes bytes rather than a path, and somebody has to have read the
     * first program. Nothing in the kernel opens a file any more. */
    const usize init_size =
        static_cast<usize>(g_server_init_end - g_server_init);
    const u32 pid = process::create_embedded("init", g_server_init, init_size,
                                             scheduler::current_pid());
    if (pid == 0)
        panic("could not create the init process");

    i32 status = 0;
    const i64 reaped = scheduler::wait_child(-1, &status, 0);

    console::set_color(console::Color::DarkGray);
    console::printf("\n  kernel: init (pid %lld) exited, status 0x%x\n", reaped, status);
    console::set_color(console::Color::LightGray);
}

// --- preemptive scheduler self-test ----------------------------------------
//
// Several workers spin in tight loops that never yield. On one processor the
// only thing that can move between them is the timer preempting one mid-loop,
// so if every worker makes progress - and the interleave trace shows the CPU
// bouncing between them - preemption is real. With several processors the same
// test also demonstrates the workers running side by side. main spins too,
// without yielding, so its own survival to the end is proof the scheduler
// switches back as well.

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
        // last entry, so the log is a trace of switches rather than of loop
        // iterations. On one CPU that is exactly a record of preemption; with
        // several it also picks up workers genuinely running at the same time,
        // which is why the check below insists every worker made progress
        // rather than reading the trace alone.
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
    // The scheduler is initialised once, early, before any application
    // processor exists - re-initialising it here would wipe the task table out
    // from under the CPUs already running on it.
    const u32 baseline = scheduler::alive_count();

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
    // Wait for a long enough interleave *and* for every worker to have run.
    // On one CPU round-robin guaranteed the second once the first held; with
    // several CPUs a couple of workers can ping-pong enough to satisfy the
    // interleave while another has not been scheduled at all - and a worker
    // that never runs never sees the stop flag, so it never exits either.
    bool ok = false;
    for (u64 i = 0; i < 20'000'000'000ull; ++i) {
        bool every_worker_ran = true;
        for (usize w = 0; w < kWorkers; ++w) {
            if (g_work_counter[w] == 0)
                every_worker_ran = false;
        }
        if (g_interleave_len >= 12 && every_worker_ran) {
            ok = true;
            break;
        }
        asm volatile("pause");
    }

    g_work_stop = true;
    // Wait for the workers specifically, not for the machine to fall idle:
    // every CPU has its own idle task, so "one task left" is never true again.
    for (u64 i = 0; i < 3'000'000ull && scheduler::alive_count() > baseline; ++i)
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
    percpu::init(0, 0);
    step("GDT + TSS installed per CPU, IST1 armed for #DF");

    interrupts::init();
    step("IDT installed, 256 vectors");

    // Floating point, before anything runs that might use it. Only user code
    // does - the kernel is still built -mno-sse - but the control bits are
    // per-CPU and have to be set on each one, so this is paired with the
    // matching call in ap_main.
    if (fpu::init_this_cpu())
        step("x87/SSE enabled, FPU state saved per task");
    else
        step("no FXSAVE on this CPU: floating point stays off");

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

    /* The wall clock, read once from the CMOS and carried forward on the tick
     * the line above just started. Everything with a timestamp on it - every
     * file this system writes from here on - dates from this call. */
    clock::init();
    {
        const i64 t = clock::now_seconds();
        if (clock::from_hardware())
            console::printf("  [ ok ] wall clock: %lld seconds past 1970\n", t);
        else
            console::printf("  [ .. ] no believable CMOS clock; time starts at 1970\n");
    }

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

            // With the local APIC up, the other processors can be started.
            // The bootstrap processor needs its idle task before any other CPU
            // starts scheduling: the moment an AP picks up work, this one can
            // find itself with nothing to run, and no idle task is a panic.
            scheduler::init();
            scheduler::start_idle();

            const usize cpus = smp::init();
            // Only worth doing when the other CPUs can actually answer: one
            // halted with interrupts off never will, and every unmap would then
            // wait out the full shootdown timeout.
            vmm::enable_tlb_shootdown(
                smp::scheduling() ? static_cast<u32>(cpus) : 1);
            if (cpus > 1)
                console::printf("  [ ok ] SMP: %llu processors online, all "
                                "scheduling\n", static_cast<u64>(cpus));
            else if (acpi::cpu_count() > 1)
                step("SMP: application processors did not start");
        }
    } else {
        step("no ACPI tables found - staying on the PIC and PIT");
    }

    /* No input drivers here. The 8042 and everything it carries - scancodes,
     * modifiers, mouse packets - is ps2d's, in ring 3, next to usbd. All the
     * kernel keeps is the queue a blocked reader sleeps on, because waking
     * that reader is scheduling and nothing else in this is. */
    keyboard::init();
    mouse::init();

    /* No PCI enumeration either. Every driver scans config space for itself,
     * because a driver must not trust someone else's table about hardware it
     * is about to drive - and once they all did that, the kernel's copy was
     * only ever printed. It is `lspci` now. */

    /* No disk driver in here any more. The drive is blockd's, and two drivers
     * on one channel is one command arriving in the middle of another's
     * transfer - so the kernel does not even identify the drives, because it
     * would have to touch the same registers to do it. */

    /* No USB here either. The controller is usbd's; all the kernel keeps of a
     * keyboard is the queue a blocked reader sleeps on. */

    cpu::sti();
    step("interrupts enabled");

    if (console::graphical()) {
        console::printf("  [ ok ] framebuffer %ux%u, %llux%llu text, BIOS ROM font\n",
                        framebuffer::width(), framebuffer::height(),
                        static_cast<u64>(console::columns()),
                        static_cast<u64>(console::rows()));
    }

    shm::init();
    image::init();
    object::init();
    region::init();
    tunable::init();
    ipc::init();
    syscall::init();
    step("SYSCALL/SYSRET enabled, ring 3 ready");

    /* No sound driver in here either. The card is audiod's, and the kernel
     * has no business holding a mixer. */

    /* No network here any more. The card is driven by e1000d and the
     * protocols live in netd, both of them ordinary processes that init
     * starts; the kernel's part is the message passing they use to reach each
     * other, which the tests exercise directly. */

    self_test_scheduler();
    step("preemptive scheduler: kernel threads time-sliced by the PIT");

    run_userland();
    step("userland: init forked, exec'd and waited on a child");

    print_memory(*info);

    echo_loop();
}
