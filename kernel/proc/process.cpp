#include <leah/console.hpp>
#include <leah/cpu.hpp>
#include <leah/elf.hpp>
#include <leah/heap.hpp>
#include <leah/vfs.hpp>
#include <leah/memory.hpp>
#include <leah/pmm.hpp>
#include <leah/process.hpp>
#include <leah/scheduler.hpp>
#include <leah/string.hpp>
#include <leah/syscall.hpp>
#include <leah/vmm.hpp>

namespace process {
namespace {

constexpr usize kMaxArgs      = 32;
constexpr usize kArgStorage   = 2048;

// A kernel-side copy of a program's arguments, taken from the caller's address
// space before we switch away from it.
struct Args {
    int   argc;
    usize offset[kMaxArgs];     // where each string starts in `storage`
    char  storage[kArgStorage];
};

// Copy an argv vector out of the currently active (caller's) address space.
void copy_argv(char** user_argv, Args& out)
{
    out.argc = 0;
    usize used = 0;
    if (user_argv == nullptr)
        return;

    for (int i = 0; i < static_cast<int>(kMaxArgs); ++i) {
        const char* arg = user_argv[i];
        if (arg == nullptr)
            break;
        out.offset[out.argc] = used;
        usize j = 0;
        while (arg[j] != '\0' && used < kArgStorage - 1) {
            out.storage[used++] = arg[j++];
        }
        out.storage[used++] = '\0';
        ++out.argc;
    }
}

// A single-argument vector, for a process created without a caller's argv.
void single_arg(const char* name, Args& out)
{
    out.argc = 1;
    out.offset[0] = 0;
    usize j = 0;
    while (name[j] != '\0' && j < kArgStorage - 1) {
        out.storage[j] = name[j];
        ++j;
    }
    out.storage[j] = '\0';
}

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

// Lay out the initial stack the way _start expects: argc, then the argv
// pointers, a NULL terminator, then the strings. Runs with the new space active
// so the writes land in it. Returns the user RSP _start begins on.
vaddr_t build_stack(const Args& args)
{
    u64 sp = kUserStackTop;

    // Copy the strings to the top of the stack and note their user addresses.
    vaddr_t arg_addr[kMaxArgs];
    for (int i = 0; i < args.argc; ++i) {
        const char* s = args.storage + args.offset[i];
        usize len = 0;
        while (s[len] != '\0')
            ++len;
        sp -= len + 1;
        memcpy(reinterpret_cast<void*>(sp), s, len + 1);
        arg_addr[i] = sp;
    }

    sp &= ~static_cast<u64>(15);         // keep the pointer array 16-aligned

    auto push = [&](u64 value) {
        sp -= 8;
        *reinterpret_cast<u64*>(sp) = value;
    };

    push(0);                             // argv[argc] = NULL
    for (int i = args.argc - 1; i >= 0; --i)
        push(arg_addr[i]);
    push(static_cast<u64>(args.argc));   // argc, at the lowest address

    return sp;
}

// Load an ELF, map a stack, and lay out args in a fresh space. Borrows CR3 for
// the duration and restores it. Returns the new space, with the entry point and
// initial stack pointer written out, or 0.
//
// The image is always bytes, never a path. It used to be able to take a path
// and read it, which meant the kernel reaching into the filesystem server - a
// blocking call, in the middle of building an address space, with all the
// trouble that implies: a task that blocks is rescheduled with its own space
// put back, silently undoing the switch below. Whoever wants to run a program
// reads it themselves now and hands over the bytes.
//
// `name` is only for the error message.
vmm::AddressSpace build_image(const char* name, const Args& args,
                              vaddr_t& entry_out, vaddr_t& stack_out,
                              const u8* image_bytes, usize image_size)
{
    const vmm::AddressSpace previous = vmm::current_space();

    const vmm::AddressSpace space = vmm::create_address_space();
    if (space == 0)
        return 0;

    vmm::switch_address_space(space);

    elf::Image image{};
    const elf::Error error = elf::load_memory(image_bytes, image_size, image);
    if (error != elf::Error::None || !map_user_stack()) {
        vmm::switch_address_space(previous);
        vmm::destroy_address_space(space);
        if (error != elf::Error::None)
            console::printf("  process: %s: %s\n", name, elf::error_name(error));
        return 0;
    }

    stack_out = build_stack(args);
    entry_out = image.entry;

    vmm::switch_address_space(previous);
    return space;
}

scheduler::TrapFrame entry_frame(vaddr_t entry, vaddr_t stack)
{
    scheduler::TrapFrame frame{};
    frame.rip    = entry;
    frame.cs     = syscall::kUserCode;
    frame.rflags = syscall::kUserFlags;
    frame.rsp    = stack;
    frame.ss     = syscall::kUserData;
    return frame;
}

} // namespace

u32 create_embedded(const char* name, const u8* image, usize size, u32 parent_pid)
{
    cpu::InterruptGuard guard;

    Args args;
    single_arg(name, args);

    vaddr_t entry = 0;
    vaddr_t stack = 0;
    const vmm::AddressSpace space =
        build_image(name, args, entry, stack, image, size);
    if (space == 0)
        return 0;

    const u32 pid =
        scheduler::spawn_user(name, space, entry_frame(entry, stack), parent_pid);
    if (pid == 0) {
        vmm::destroy_address_space(space);
        return 0;
    }
    return pid;
}

void exec(syscall::Frame& frame, const u8* image, usize size, char** argv)
{
    const vmm::AddressSpace old_space = scheduler::current_task_space();

    /* The image and argv are in the caller's space, which build_image switches
     * away from, so both are copied into the kernel first - its memory is
     * mapped in every address space and so is reachable from both sides of the
     * switch. The caller read the file; this is only the copy across the
     * boundary, which is the part that has to be here. */
    u8* owned = static_cast<u8*>(kmalloc(size));
    if (owned == nullptr) {
        frame.rax = static_cast<u64>(-1);
        return;
    }
    memcpy(owned, image, size);

    Args args;
    copy_argv(argv, args);
    if (args.argc == 0)
        single_arg("program", args);     // at least argv[0]

    vaddr_t entry = 0;
    vaddr_t stack = 0;
    const char* name = args.argc > 0 ? args.storage + args.offset[0] : "program";
    const vmm::AddressSpace space =
        build_image(name, args, entry, stack, owned, size);
    kfree(owned);
    if (space == 0) {
        frame.rax = static_cast<u64>(-1);
        return;
    }

    scheduler::current_task_set_space(space);
    scheduler::set_current_brk(memory::kUserBrkBase);   // fresh heap for the new image
    vmm::switch_address_space(space);
    vmm::destroy_address_space(old_space);

    // Rewrite the syscall frame so SYSRET enters the new program on its fresh
    // argv stack with a clean register file.
    frame.r15 = frame.r14 = frame.r13 = frame.r12 = frame.rbp = frame.rbx = 0;
    frame.r11 = frame.r10 = frame.r9 = frame.r8 = 0;
    frame.rdx = frame.rsi = frame.rdi = frame.rax = 0;
    frame.user_rip   = entry;
    frame.user_flags = syscall::kUserFlags;
    frame.user_rsp   = stack;
}

} // namespace process
