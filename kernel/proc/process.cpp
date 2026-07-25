#include <leah/console.hpp>
#include <leah/cpu.hpp>
#include <leah/elf.hpp>
#include <leah/pmm.hpp>
#include <leah/process.hpp>
#include <leah/scheduler.hpp>
#include <leah/syscall.hpp>
#include <leah/vmm.hpp>

namespace process {
namespace {

// Maps a ring-3 stack into the currently active space.
bool map_user_stack()
{
    for (usize i = 0; i < kUserStackPages; ++i) {
        const vaddr_t page = kUserStackTop - (i + 1) * vmm::kPageSize;
        const paddr_t frame = pmm::alloc();
        if (frame == 0)
            return false;
        if (!vmm::map(page, frame, vmm::Write | vmm::User | vmm::NoExecute))
            return false;
    }
    return true;
}

// The register state a program starts on: entry point, fresh stack, ring-3
// selectors, interrupts enabled, everything else zero.
scheduler::TrapFrame entry_frame(vaddr_t entry)
{
    scheduler::TrapFrame frame{};
    frame.rip    = entry;
    frame.cs     = syscall::kUserCode;
    frame.rflags = syscall::kUserFlags;
    frame.rsp    = kUserStackTop;
    frame.ss     = syscall::kUserData;
    return frame;
}

// Load an ELF and a stack into a fresh space, borrowing CR3 for the duration.
// Returns the new space with the entry written to `entry_out`, or 0.
vmm::AddressSpace build_image(const char* path, vaddr_t& entry_out)
{
    const vmm::AddressSpace previous = vmm::current_space();

    const vmm::AddressSpace space = vmm::create_address_space();
    if (space == 0)
        return 0;

    vmm::switch_address_space(space);

    elf::Image image{};
    const elf::Error error = elf::load(path, image);
    if (error != elf::Error::None || !map_user_stack()) {
        vmm::switch_address_space(previous);
        vmm::destroy_address_space(space);
        if (error != elf::Error::None)
            console::printf("  process: %s: %s\n", path, elf::error_name(error));
        return 0;
    }

    vmm::switch_address_space(previous);
    entry_out = image.entry;
    return space;
}

} // namespace

u32 create(const char* name, const char* path, u32 parent_pid)
{
    // Interrupts off: build_image borrows CR3 to load into the new space, and a
    // preemption mid-load would restore the caller's CR3 underneath it.
    cpu::InterruptGuard guard;

    vaddr_t entry = 0;
    const vmm::AddressSpace space = build_image(path, entry);
    if (space == 0)
        return 0;

    const u32 pid = scheduler::spawn_user(name, space, entry_frame(entry), parent_pid);
    if (pid == 0) {
        vmm::destroy_address_space(space);
        return 0;
    }
    return pid;
}

void exec(syscall::Frame& frame, const char* path)
{
    // Called from a syscall, so interrupts are already masked and CR3 is the
    // calling process's space.
    const vmm::AddressSpace old_space = scheduler::current_task_space();

    // `path` points into the caller's address space, which build_image is about
    // to switch away from - so copy the string into the kernel (its stack is
    // shared across every space) before touching CR3.
    char kpath[256];
    usize n = 0;
    while (n < sizeof(kpath) - 1 && path[n] != '\0') {
        kpath[n] = path[n];
        ++n;
    }
    kpath[n] = '\0';

    vaddr_t entry = 0;
    const vmm::AddressSpace space = build_image(kpath, entry);
    if (space == 0) {
        frame.rax = static_cast<u64>(-1);       // exec failed; caller runs on
        return;
    }

    // Commit: the current task now lives in the new space. Activate it, then
    // reclaim the old one now that nothing points at it.
    scheduler::current_task_set_space(space);
    vmm::switch_address_space(space);
    vmm::destroy_address_space(old_space);

    // Rewrite the syscall's saved frame so SYSRET enters the new program with a
    // clean register file and its fresh stack, rather than returning to the
    // code that called exec.
    frame.r15 = frame.r14 = frame.r13 = frame.r12 = frame.rbp = frame.rbx = 0;
    frame.r11 = frame.r10 = frame.r9 = frame.r8 = 0;
    frame.rdx = frame.rsi = frame.rdi = frame.rax = 0;
    frame.user_rip   = entry;
    frame.user_flags = syscall::kUserFlags;
    frame.user_rsp   = kUserStackTop;
}

} // namespace process
