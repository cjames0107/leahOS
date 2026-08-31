#include <leah/console.hpp>
#include <leah/image.hpp>
#include <leah/object.hpp>
#include <leah/cpu.hpp>
#include <leah/heap.hpp>
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
    // The environment, in the same storage. It is copied here for the same
    // reason argv is: both live in the caller's address space, and building
    // the new one switches away from it.
    int   envc;
    usize env_offset[kMaxArgs];
    char  storage[kArgStorage];
    usize used;
    /* The auxiliary vector, as type/value pairs. Zero entries is a program
     * started without one, which is what the four boot images get. */
    int   auxc;
    u64   aux[32];
};

// Copy an argv vector out of the currently active (caller's) address space.
// Copy one NULL-terminated vector into `out`'s storage, appending to whatever
// is already there. `count` and `offsets` are the caller's to keep.
void copy_vector(char** user_vector, Args& out, int& count, usize* offsets)
{
    count = 0;
    if (user_vector == nullptr)
        return;

    for (int i = 0; i < static_cast<int>(kMaxArgs); ++i) {
        const char* arg = user_vector[i];
        if (arg == nullptr)
            break;
        if (out.used >= kArgStorage - 1)
            break;                  // the storage is full; the rest is dropped
        offsets[count] = out.used;
        usize j = 0;
        while (arg[j] != '\0' && out.used < kArgStorage - 1)
            out.storage[out.used++] = arg[j++];
        out.storage[out.used++] = '\0';
        ++count;
    }
}

void copy_argv(char** user_argv, Args& out)
{
    out.used = 0;
    out.envc = 0;
    copy_vector(user_argv, out, out.argc, out.offset);
}

void copy_envp(char** user_envp, Args& out)
{
    copy_vector(user_envp, out, out.envc, out.env_offset);
}

// A single-argument vector, for a process created without a caller's argv.
void single_arg(const char* name, Args& out)
{
    out.argc = 1;
    out.envc = 0;
    out.offset[0] = 0;
    usize j = 0;
    while (name[j] != '\0' && j < kArgStorage - 1) {
        out.storage[j] = name[j];
        ++j;
    }
    out.storage[j] = '\0';
    out.used = j + 1;
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

// Lay out the initial stack the way _start expects:
//
//     argc, argv[0..argc-1], NULL, envp[0..envc-1], NULL, then the strings
//
// crt0 has always computed envp as argv + (argc + 1) * 8, which is where the
// second vector goes - so until there was one, it was pointing at the strings.
// Runs with the new space active so the writes land in it. Returns the user
// RSP _start begins on.
vaddr_t build_stack(const Args& args)
{
    u64 sp = kUserStackTop;

    auto copy_string = [&](const char* s) {
        usize len = 0;
        while (s[len] != '\0')
            ++len;
        sp -= len + 1;
        memcpy(reinterpret_cast<void*>(sp), s, len + 1);
        return sp;
    };

    // The strings first, at the top, so the pointers below can name them.
    vaddr_t arg_addr[kMaxArgs];
    vaddr_t env_addr[kMaxArgs];
    for (int i = 0; i < args.argc; ++i)
        arg_addr[i] = copy_string(args.storage + args.offset[i]);
    for (int i = 0; i < args.envc; ++i)
        env_addr[i] = copy_string(args.storage + args.env_offset[i]);

    sp &= ~static_cast<u64>(15);         // keep the pointer array 16-aligned

    auto push = [&](u64 value) {
        sp -= 8;
        *reinterpret_cast<u64*>(sp) = value;
    };

    // Pushed backwards, so that they read forwards from the low address.
    //
    // The auxiliary vector goes above the environment, which is where every
    // other system puts it and where every program that reads one looks. It
    // ends with an AT_NULL pair; a program that does not know about it simply
    // stops at the environment's own terminator and never sees this.
    push(0);                             // AT_NULL's value
    push(0);                             // AT_NULL
    for (int i = args.auxc - 2; i >= 0; i -= 2) {
        push(args.aux[i + 1]);           // value, at the higher address
        push(args.aux[i]);               // type
    }
    push(0);                             // envp[envc] = NULL
    for (int i = args.envc - 1; i >= 0; --i)
        push(env_addr[i]);
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
/* What the kernel maps: an address, some bytes, and how much of it is memory.
 * Not a format - the caller has already worked out the format, whether that was
 * libc reading a file or the build reading it years earlier. */
struct Segment {
    u64 vaddr;
    u64 offset;         // into the image, or into the blob when image is -1
    u64 filesz;
    u64 memsz;
    u32 flags;          // ELF program-header flags: 1 execute, 2 write, 4 read
    i32 image;          // a handle on the image this is cut from, or -1
};

constexpr u32 kSegExecute = 1;
constexpr u32 kSegWrite   = 2;

/* Map one program's segments into the address space that is currently active.
 *
 * This is all that is left of loading a program. Deciding *which* bytes go
 * where is parsing, and parsing is somebody else's job now - libc's for an
 * ordinary exec, the build's for the three programs the kernel carries. */
bool map_segments(const Segment* segments, u32 count, const u8* blob,
                  u64 blob_size)
{
    if (count == 0 || count > 16)
        return false;

    for (u32 i = 0; i < count; ++i) {
        const Segment& s = segments[i];
        if (s.memsz == 0)
            continue;
        if (s.filesz > s.memsz)
            return false;
        if (s.image < 0 && s.offset + s.filesz > blob_size)
            return false;
        if (s.vaddr >= memory::kKernelBase)
            return false;               // a user program, in the user half

        /* A segment rarely starts on a page boundary, and the bytes in front
         * of it inside the first page belong to it too - so round outward. */
        const vaddr_t start = s.vaddr & ~(vmm::kPageSize - 1);
        const vaddr_t end   = (s.vaddr + s.memsz + vmm::kPageSize - 1) &
                              ~(vmm::kPageSize - 1);
        /* Where the file's bytes stop. Past this the segment is .bss, which is
         * the same zeros in every process and is never shared. */
        const vaddr_t file_end = (s.vaddr + s.filesz + vmm::kPageSize - 1) &
                                 ~(vmm::kPageSize - 1);

        const bool writable = (s.flags & kSegWrite) != 0;

        /* A read-only segment cut from a held image is *mapped*, not copied:
         * the frames are the image's, every process that runs this program
         * points at the same ones, and libc exists once in memory instead of
         * once per process. That is the whole point of the image cache.
         *
         * It works because ELF guarantees p_vaddr and p_offset are congruent
         * modulo the page size - the same guarantee that lets every other
         * system mmap a segment straight out of a file. Checked rather than
         * assumed, because a program that broke it would otherwise be mapped
         * off by a few bytes and fault somewhere unrecognisable. */
        const bool congruent =
            (s.vaddr & (vmm::kPageSize - 1)) == (s.offset & (vmm::kPageSize - 1));
        /* The handle is resolved once, here, and every use below is of the
         * object it named. A segment claiming an image this process does not
         * hold is simply not backed by one - it falls through to the blob,
         * which is bounds-checked, rather than reaching anything. */
        void* held = s.image < 0 ? nullptr
                                 : object::look(scheduler::current_tgid(),
                                                static_cast<object::Handle>(s.image),
                                                object::Type::Image,
                                                object::kMap);
        if (s.image >= 0 && held == nullptr)
            return false;
        const bool shareable = !writable && held != nullptr && congruent;

        for (vaddr_t page = start; page < end; page += vmm::kPageSize) {
            u64 flags = vmm::User;
            if (writable)
                flags |= vmm::Write;
            if ((s.flags & kSegExecute) == 0)
                flags |= vmm::NoExecute;

            if (shareable && page < file_end) {
                const u64 at = s.offset - (s.vaddr - page);
                if (image::share_frame(held, at)) {
                    if (!vmm::map(page, image::frame_at(held, at), flags))
                        return false;
                    continue;
                }
                /* No reference to be had - fall through and copy, which is
                 * always correct and only slower. */
            }

            /* Past the file's bytes is .bss: reserved rather than allocated,
             * so a program with a megabyte of it that touches a page of it
             * pays for a page. libc alone has two hundred kilobytes. */
            if (page >= file_end) {
                if (!vmm::reserve(page, flags))
                    return false;
                continue;
            }

            const paddr_t frame = pmm::alloc();
            if (frame == 0)
                return false;
            /* Writable while loading, because the loader has to write into it.
             * A read-only segment is tightened once its contents are in. */
            if (!vmm::map(page, frame, flags | vmm::Write)) {
                pmm::free(frame);
                return false;
            }
            /* Cleared, which is also what makes the tail of the last page read
             * as the zeros it has to be. */
            memset(reinterpret_cast<void*>(page), 0, vmm::kPageSize);

            /* And the bytes themselves, clipped to this page. */
            const vaddr_t from = page > s.vaddr ? page : s.vaddr;
            const vaddr_t to   = page + vmm::kPageSize;
            const vaddr_t stop = s.vaddr + s.filesz < to ? s.vaddr + s.filesz : to;
            if (stop > from) {
                const u64 at = s.offset + (from - s.vaddr);
                if (held != nullptr) {
                    if (!image::read(held, at, reinterpret_cast<void*>(from),
                                     stop - from))
                        return false;
                } else {
                    memcpy(reinterpret_cast<void*>(from), blob + at, stop - from);
                }
            }
        }

        if (!writable) {
            u64 readonly = vmm::User;
            if ((s.flags & kSegExecute) == 0)
                readonly |= vmm::NoExecute;
            for (vaddr_t page = start; page < end; page += vmm::kPageSize) {
                const paddr_t frame = vmm::translate(page);
                if (frame != 0)
                    vmm::map(page, frame, readonly);
            }
        }
    }

    return true;
}

vmm::AddressSpace build_image(const char* name, const Args& args,
                              vaddr_t& entry_out, vaddr_t& stack_out,
                              u64 entry, const Segment* segments, u32 count,
                              const u8* blob, u64 blob_size)
{
    const vmm::AddressSpace previous = vmm::current_space();

    const vmm::AddressSpace space = vmm::create_address_space();
    if (space == 0)
        return 0;

    vmm::switch_address_space(space);

    if (!map_segments(segments, count, blob, blob_size) || !map_user_stack()) {
        vmm::switch_address_space(previous);
        vmm::destroy_address_space(space);
        console::printf("  process: %s: could not be mapped\n", name);
        return 0;
    }

    stack_out = build_stack(args);
    entry_out = entry;

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

    /* A boot image, not an ELF: the build already read the program headers and
     * wrote out what they meant. See tools/mkbootimage.py - the layout is a
     * magic word, an entry point, a count, that many segment descriptors, and
     * then the bytes they point at. */
    if (size < 24 || memcmp(image, "LEAHIMG1", 8) != 0) {
        console::printf("  process: %s: not a boot image\n", name);
        return 0;
    }
    const u64 entry_point = *reinterpret_cast<const u64*>(image + 8);
    const u32 count = *reinterpret_cast<const u32*>(image + 16);
    const auto* segments = reinterpret_cast<const Segment*>(image + 24);
    if (count == 0 || count > 16 || 24 + count * sizeof(Segment) > size) {
        console::printf("  process: %s: boot image is malformed\n", name);
        return 0;
    }

    Args args;
    single_arg(name, args);
    args.auxc = 0;

    /* The four programs the build hands the kernel are cut from the boot image
     * itself, not from a held one - there is no filesystem yet, which is the
     * whole reason they are here. Said explicitly rather than relying on the
     * tool to have written a -1 into a field it predates. */
    Segment owned[16];
    memcpy(owned, segments, count * sizeof(Segment));
    for (u32 i = 0; i < count; ++i)
        owned[i].image = -1;

    vaddr_t entry = 0;
    vaddr_t stack = 0;
    const vmm::AddressSpace space =
        build_image(name, args, entry, stack, entry_point, owned, count,
                    image, size);
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

void exec(syscall::Frame& frame, const u8* image, usize size, char** argv,
          char** envp, u64 entry_point, const void* user_segments, u32 count,
          const u64* aux, u32 auxc)
{
    const vmm::AddressSpace old_space = scheduler::current_task_space();

    /* The image and argv are in the caller's space, which build_image switches
     * away from, so both are copied into the kernel first - its memory is
     * mapped in every address space and so is reachable from both sides of the
     * switch. The caller read the file; this is only the copy across the
     * boundary, which is the part that has to be here. */
    if (count == 0 || count > 16) {
        frame.rax = static_cast<u64>(-1);
        return;
    }
    Segment segments[16];
    memcpy(segments, user_segments, count * sizeof(Segment));

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
    copy_envp(envp, args);
    args.auxc = 0;
    if (aux != nullptr && auxc > 0 && auxc <= 32) {
        memcpy(args.aux, aux, auxc * sizeof(u64));
        args.auxc = static_cast<int>(auxc);
    }

    vaddr_t entry = 0;
    vaddr_t stack = 0;
    const char* name = args.argc > 0 ? args.storage + args.offset[0] : "program";
    const vmm::AddressSpace space =
        build_image(name, args, entry, stack, entry_point, segments, count,
                    owned, size);
    if (space == 0) {
        kfree(owned);
        frame.rax = static_cast<u64>(-1);
        return;
    }

    /* It is this program now, and every report from here on should say so. */
    scheduler::set_current_name(name);

    scheduler::current_task_set_space(space);
    scheduler::set_current_brk(memory::kUserBrkBase);   // fresh heap for the new image
    vmm::switch_address_space(space);
    vmm::destroy_address_space(old_space);

    kfree(owned);

    // Rewrite the syscall frame so SYSRET enters the new program on its fresh
    // argv stack with a clean register file - the floating-point ones
    // included, which the frame does not carry.
    scheduler::reset_current_fpu();
    frame.r15 = frame.r14 = frame.r13 = frame.r12 = frame.rbp = frame.rbx = 0;
    frame.r11 = frame.r10 = frame.r9 = frame.r8 = 0;
    frame.rdx = frame.rsi = frame.rdi = frame.rax = 0;
    frame.user_rip   = entry;
    frame.user_flags = syscall::kUserFlags;
    frame.user_rsp   = stack;
}

} // namespace process
