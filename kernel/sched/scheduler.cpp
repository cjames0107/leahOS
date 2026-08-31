#include <leah/console.hpp>
#include <leah/cpu.hpp>
#include <leah/gdt.hpp>
#include <leah/heap.hpp>
#include <leah/ipc.hpp>
#include <leah/memory.hpp>
#include <leah/panic.hpp>
#include <leah/object.hpp>
#include <leah/percpu.hpp>
#include <leah/file.hpp>
#include <leah/fpu.hpp>
#include <leah/scheduler.hpp>
#include <leah/shm.hpp>
#include <leah/smp.hpp>
#include <leah/signal.hpp>
#include <leah/spinlock.hpp>
#include <leah/timer.hpp>
#include <leah/string.hpp>

// Implemented in context.asm, user_entry.asm and syscall_entry.asm.
extern "C" void context_switch(u64* save_rsp, u64 load_rsp, u64 stamp);
extern "C" void user_return();
// The first-entry-to-ring-3 stub: releases the displaced task, then falls into
// user_return. See user_entry.asm.
extern "C" void first_user_entry();

namespace scheduler {
namespace {

/* Thirty-two was enough when the drivers were in the kernel. They are not:
 * every one of them is a process now, ps2d brings a thread as well, and each
 * processor has an idle task. A desktop session with a few windows open is
 * most of the way there before a program forks anything - and eight children
 * at once, which the suite does deliberately, went over on two CPUs. Running
 * out is not an error anyone sees; fork simply starts failing. */
/* Ninety-six was a number picked when the desktop was three programs. A task
 * is about a kilobyte and a half - most of it the FPU state and the signal
 * table - so this costs a third of a megabyte out of five hundred, and the
 * alternative is a machine that stops being able to start anything. */
constexpr usize kMaxTasks   = 256;
static_assert(kMaxTasks <= kMaxTaskInfo,
              "a snapshot must be allowed to ask for every task there can be");
// "this CPU has not switched away from anything yet"
constexpr u32 kNoPrevious   = 0xFFFFFFFFu;
constexpr u32   kMaxSignals = signals::kMaxSignals;
constexpr usize kStackSize  = 16 * 1024;

// A word at the lowest address of every kernel stack, checked on every switch.
//
// A kernel stack grows down towards this; if it is ever not what was written
// there, the stack has run past its own end and into whatever the heap put
// underneath. That failure has no signature of its own - it corrupts something
// else and shows up later as a jump to nonsense - so it is worth one compare
// per switch to catch it where it happens instead.
constexpr u64 kStackCanary = 0x5441434B47554152ull;   // "TACKGUAR"

void plant_canary(u64 stack_base)
{
    *reinterpret_cast<u64*>(stack_base) = kStackCanary;
}

bool canary_intact(u64 stack_base)
{
    return *reinterpret_cast<const u64*>(stack_base) == kStackCanary;
}
constexpr u32   kQuantum    = 1;            // one 10 ms tick per slice

enum class State : u8 {
    Unused,
    Ready,
    Running,
    Blocked,        // in wait(), no zombie child yet
    // Suspended by SIGSTOP or one of its relatives. Not Blocked: a blocked task
    // is waiting for something that will happen on its own, and a stopped one
    // is waiting for a decision somebody else has to make. Nothing wakes it but
    // SIGCONT - not the thing it was blocked on, not a timer - and it has to
    // survive being reported to its parent and asked about again.
    Stopped,
    Zombie,         // exited, waiting to be reaped
    Dead,           // finished kernel thread
};

struct Task {
    u64   kernel_rsp;        // saved scheduler context (points into kernel_stack)
    u64   kernel_stack;      // base, for freeing
    u64   kernel_stack_top;  // loaded into TSS.rsp0 while this task runs
    u32   pid;
    u32   parent_pid;
    // Thread group: every thread of a process shares one tgid, which is the pid
    // of the group leader. A single-threaded process is its own leader. Threads
    // share the leader's address space and open-file table.
    u32   tgid;
    // Process group and session. A process group is what a signal from the
    // keyboard goes to and what a shell calls a job: `a | b | c` is three
    // processes and one group, so Ctrl-C reaches all three and the shell waits
    // for all three. A session is a login - a set of groups sharing a terminal,
    // one of which is the foreground group at any moment.
    //
    // Both are inherited across fork and kept across execve, which is what lets
    // a shell put a child in a group before the child has run any of its own
    // code and be sure the child is in it either way.
    u32   pgid;
    u32   sid;
    // Set while a stopped task has a stop its parent has not been told about,
    // or a continue. Cleared by whoever reports it. Without this a parent that
    // waits twice hears about one stop twice.
    i32   report;
    State state;
    bool  is_user;
    vmm::AddressSpace space; // 0 for kernel threads (they use the kernel space)
    i32   exit_code;
    u64   wait_channel;      // when Blocked on block_on(); 0 otherwise
    u32   bkl_depth;         // kernel-lock depth this task was suspended holding
    u64   ticks;             // slices given to this task, for a resource monitor
    // True from the moment a CPU commits to running this task until the CPU it
    // left has finished saving its context. No other CPU may pick it up in
    // between: its kernel_rsp is still stale, and resuming from it would put
    // two processors on one kernel stack.
    volatile bool on_cpu;
    // The stamp on the switch frame this task's kernel_rsp points at. Set when
    // the frame is saved (or fabricated) and checked before it is resumed.
    u64 resume_stamp;
    // Tick at which a sleeping task becomes runnable again, or 0 if it is not
    // sleeping. Checked on every timer tick.
    u64   wake_tick;
    u64   brk;               // sbrk program break, for user tasks
    u64   mmap_next;         // next free address mmap hands out
    // Credentials, inherited across fork and execve. uid 0 is root and bypasses
    // permission checks; everything starts as root until someone calls setuid.
    u32   uid;
    u32   gid;
    // Signals. The pending set is per-task (a signal is delivered to the thread
    // it was sent to), but the dispositions are per-process, so handlers and the
    // restorer are read from and written to the group leader.
    u32   sig_pending;
    u64   sig_handler[kMaxSignals];
    u64   sig_restorer;      // libc's trampoline, which calls sigreturn
    /* Owned, not borrowed. This was a const char* pointing at whatever the
     * caller had - which for execve is the argument storage of an image that
     * is about to be replaced, and for fork is the parent's copy of that same
     * pointer. Once the storage went, the name read as whatever now occupied
     * it, and every crash report and every line of ps named the wrong
     * program. Thirty-two bytes per task is a cheap price for a report that
     * can be believed. */
    char name[32];
    Entry entry;             // kernel threads only
    void* arg;
    files::Table files;      // open files + cwd; inherited across fork
    /* Which I/O ports this task may touch, or null for the overwhelming
     * majority that may touch none. Allocated on the first grant and inherited
     * across fork - a driver that forks a worker meant the worker to be able to
     * do the work. */
    u8* io_bitmap;
    /* The x87/SSE registers, when this task is not the one holding them.
     * Only user tasks have any: the kernel is built -mno-sse, so a kernel
     * thread cannot put anything in them to lose. */
    fpu::State fpu_state;
};

Task g_tasks[kMaxTasks];

// The load average over one, five and fifteen minutes, in hundredths.
u64 g_load[3] = { 0, 0, 0 };
u32  g_task_count = 0;
// Which task each CPU is running, and which idle task belongs to it. Both were
// single globals before there was more than one processor; a CPU picking a task
// another CPU is already running, or two CPUs sharing one idle task, is exactly
// the corruption SMP invites.
constexpr usize kMaxCpuSlots = 32;
u32  g_current_by_cpu[kMaxCpuSlots]{};   // index into g_tasks, per CPU
// Counted where the decision is made, so a slice is attributed to the CPU that
// actually ran it rather than to whoever asked about it later.
u64  g_busy_by_cpu[kMaxCpuSlots]{};
u64  g_idle_by_cpu_ticks[kMaxCpuSlots]{};
u32  g_next_pid = 1;

constexpr u32 kNoIdle = 0xFFFFFFFF;
u32  g_idle_by_cpu[kMaxCpuSlots];    // the halt-when-idle task, one per CPU

bool g_preemption = false;
u32  g_quantum = kQuantum;
volatile bool g_need_resched = false;

u32& current_index() { return g_current_by_cpu[percpu::active()]; }
u32  idle_index()    { return g_idle_by_cpu[percpu::active()]; }

Task* current() { return &g_tasks[current_index()]; }

// Every entry into the scheduler has to be serialised, not only the ones that
// arrive through a syscall or an interrupt. A kernel thread calling yield() or
// exit_current(), or the boot path spawning a task, reaches this code with no
// lock held at all - and on more than one CPU that is a race over the task
// table. The lock is recursive per CPU, so taking it again inside a syscall
// that already holds it costs nothing.
struct KernelLock {
    KernelLock()  { sync::bkl::acquire(); }
    ~KernelLock() { sync::bkl::release(); }
};

Task* find(u32 pid)
{
    for (u32 i = 0; i < g_task_count; ++i) {
        if (g_tasks[i].pid == pid && g_tasks[i].state != State::Unused)
            return &g_tasks[i];
    }
    return nullptr;
}

// True for any CPU's idle task. Idle is only ever run as a last resort, and one
// CPU must never pick up another's.
bool is_idle_task(u32 index)
{
    for (usize i = 0; i < kMaxCpuSlots; ++i) {
        if (g_idle_by_cpu[i] == index)
            return true;
    }
    return false;
}

Task* alloc_slot()
{
    for (u32 i = 0; i < kMaxTasks; ++i) {
        if (g_tasks[i].state == State::Unused) {
            if (i >= g_task_count)
                g_task_count = i + 1;
            return &g_tasks[i];
        }
    }
    return nullptr;
}

// True if another live task still uses `space`. A thread group shares one
// address space, so the last one out is the only one that may free it - and a
// zombie has already released its own reference (space set to 0), so it does
// not keep the space alive.
bool space_still_used(vmm::AddressSpace space, const Task* except)
{
    if (space == 0)
        return false;
    for (u32 i = 0; i < g_task_count; ++i) {
        const Task* t = &g_tasks[i];
        if (t == except || t->state == State::Unused || t->state == State::Dead)
            continue;
        if (t->is_user && t->space == space)
            return true;
    }
    return false;
}

// The task holding a thread group's shared state: the group leader, or the task
// itself when the leader is gone or it is not part of a group.
Task* group_leader(Task* task)
{
    if (task->tgid != 0 && task->tgid != task->pid) {
        Task* leader = find(task->tgid);
        // A zombie leader still owns the group's open files. Excluding it here
        // sent every surviving thread to its own empty table copy instead: file
        // operations went nowhere, and the real table was never closed, so its
        // pipes were never released and the other end never saw an end-of-file.
        // The leader's slot stays reserved until the whole group is gone - see
        // wait_child - so this reference cannot dangle.
        if (leader != nullptr && leader->state != State::Unused)
            return leader;
    }
    return task;
}

// True if another thread of the same group is still alive.
bool group_still_alive(const Task* except)
{
    if (except->tgid == 0)
        return false;
    for (u32 i = 0; i < g_task_count; ++i) {
        const Task* t = &g_tasks[i];
        if (t == except || t->state == State::Unused ||
            t->state == State::Dead || t->state == State::Zombie)
            continue;
        if (t->tgid == except->tgid)
            return true;
    }
    return false;
}

/* A task's name is its own copy. The last component of the path is what a
 * person means by the program's name; anything longer is truncated rather than
 * refused, because a name is for reading and a report with a shortened name is
 * still a report. */
void set_name(Task& task, const char* name)
{
    usize n = 0;
    if (name == nullptr)
        name = "?";
    for (const char* p = name; *p != '\0'; ++p)
        if (*p == '/')
            name = p + 1;       // the last component, not the whole path
    while (name[n] != '\0' && n + 1 < sizeof(task.name)) {
        task.name[n] = name[n];
        ++n;
    }
    task.name[n] = '\0';
}

void finish_switch();

[[noreturn]] void kernel_thread_trampoline()
{
    // First code this task ever runs: it arrived by context_switch, so the CPU
    // it displaced still needs releasing.
    finish_switch();
    cpu::sti();
    Task* self = current();
    self->entry(self->arg);
    exit_current(0);
}

// Stamps only have to be unique, never reused, and never zero - a fresh frame
// must not accidentally match a stale one, and zero is what an uninitialised
// slot reads as.
u64 next_stamp()
{
    static u64 counter = 0;
    return __atomic_add_fetch(&counter, 1, __ATOMIC_RELAXED) | (1ull << 63);
}

u32 pick_next()
{
    // Any Ready task other than idle, round-robin from the current one.
    for (u32 step = 1; step <= g_task_count; ++step) {
        const u32 index = (current_index() + step) % g_task_count;
        // Ready means runnable and not already running elsewhere: a task another
        // CPU is executing is marked Running, so this skips it.
        if (!is_idle_task(index) && g_tasks[index].state == State::Ready &&
            !__atomic_load_n(&g_tasks[index].on_cpu, __ATOMIC_ACQUIRE))
            return index;
    }
    // Keep running the current task if it is still runnable.
    if (!is_idle_task(current_index()) &&
        (current()->state == State::Running || current()->state == State::Ready))
        return current_index();
    // Nothing else to do: fall back to this CPU's own idle task.
    if (idle_index() != kNoIdle)
        return idle_index();
    panic("scheduler: nothing runnable and no idle task");
}

// Release the task this CPU switched away from.
//
// Runs on the CPU that took over, after context_switch - which is the first
// moment the outgoing task's kernel_rsp is actually valid. Every path that a
// context switch can arrive at has to call this, including the two that never
// return through switch_to: a fresh kernel thread and a task entering ring 3
// for the first time.
void finish_switch()
{
    const u32 index = percpu::current().previous_task;
    percpu::current().previous_task = kNoPrevious;
    if (index == kNoPrevious || index >= kMaxTasks)
        return;

    Task& prev = g_tasks[index];
    // Only a task that was still running is now merely runnable; one that
    // blocked or exited on its way out already said so.
    if (prev.state == State::Running)
        prev.state = State::Ready;
    // Ordered last, and with a release, so a CPU that sees the flag clear also
    // sees the state and the saved stack pointer that go with it.
    __atomic_store_n(&prev.on_cpu, false, __ATOMIC_RELEASE);
}

} // namespace

// Reached from first_user_entry, the one context-switch target that is assembly
// all the way to IRETQ and so cannot call finish_switch itself.
extern "C" void scheduler_finish_switch() { finish_switch(); }

namespace {

// Enter with interrupts disabled. Besides swapping kernel stacks, this loads
// the incoming task's page table and the ring-0 stack the CPU will use if that
// task takes an interrupt or a syscall while in ring 3.
void switch_to(u32 next_index)
{
    if (next_index == current_index())
        return;

    Task* prev = current();
    Task* next = &g_tasks[next_index];

    // prev is deliberately *not* marked Ready here. Doing so published it as
    // runnable before context_switch had saved its stack pointer - and the
    // kernel lock is handed off a few lines below, so another CPU could pick it
    // up in that window and resume it from a stale kernel_rsp. The CPU that
    // takes over does it instead, in finish_switch, once the context really is
    // saved.
    next->state = State::Running;
    ++next->ticks;
    {
        const u32 cpu = percpu::active();
        if (cpu < kMaxCpuSlots) {
            if (is_idle_task(next_index))
                ++g_idle_by_cpu_ticks[cpu];
            else
                ++g_busy_by_cpu[cpu];
        }
    }
    /* Nobody else may already be running this task. pick_next refuses a task
     * with on_cpu set, but that is a check made before the lock is necessarily
     * still held all the way to here - and two processors executing on one
     * kernel stack is precisely the shape of the fault being chased: frames
     * interleaving, RSP ending up with a value that was never a stack. Assert
     * it at the moment of the switch, where the other CPU can still be named. */
    if (__atomic_exchange_n(&next->on_cpu, true, __ATOMIC_ACQ_REL)) {
        console::printf("\n  scheduler: cpu %u switching to %s (slot %u, pid %u) "
                        "which is already on another cpu\n",
                        percpu::active(),
                        next->name,
                        next_index, next->pid);
        panic("scheduler: two processors on one task");
    }
    percpu::current().previous_task = current_index();
    current_index() = next_index;
    g_quantum = kQuantum;

    // The ring-0 stack for an interrupt from ring 3, and the stack SYSCALL
    // switches to, are both this task's own kernel stack - so a syscall or
    // interrupt handler that blocks keeps its state on a stack no other task
    // will reuse.
    gdt::set_kernel_stack(percpu::active(), next->kernel_stack_top);
    /* The hardware checks this on every IN and OUT, so it has to describe the
     * task that is about to run rather than the one that just stopped. */
    gdt::set_io_bitmap(percpu::active(), next->io_bitmap);
    percpu::set_syscall_stack_for_this_cpu(next->kernel_stack_top);
    vmm::switch_address_space(next->space != 0 ? next->space : vmm::kernel_space());

    /* The floating-point registers travel with the task too, and for the same
     * reason. Saved eagerly rather than on a #NM fault: lazy switching means
     * tracking which CPU's registers belong to which task, and this kernel has
     * already paid once for machine-wide state that should have been per-CPU.
     *
     * Only user tasks are involved on either side. If a kernel thread runs in
     * between, the registers simply keep whatever the last user task left in
     * them until the next user task loads its own - which is correct, because
     * nothing in between can read them. */
    if (prev->is_user)
        fpu::save(prev->fpu_state);
    if (next->is_user)
        fpu::restore(next->fpu_state);

    // The kernel lock travels with the task, not the CPU: save what this one
    // was holding and give the incoming task back what it had. A task entered
    // for the first time has depth 0, so the lock is dropped here rather than
    // being carried into code that would never release it.
    prev->bkl_depth = sync::bkl::depth();
    sync::bkl::handoff(next->bkl_depth);

    /* The incoming task's saved return address, which context_switch is about
     * to RET to. It sits past the six callee-saved registers the switch pops.
     *
     * Checked because when it is wrong the failure is a jump to nonsense with
     * nothing left on the stack to say who did it: the register dump names
     * where it went and the frame that set it up is already gone. Catching it
     * here names the task instead. */
    if (prev->kernel_stack != 0 && !canary_intact(prev->kernel_stack)) {
        console::printf("\n  scheduler: %s (slot %u, pid %u) ran off the bottom "
                        "of its kernel stack\n",
                        prev->name,
                        current_index(), prev->pid);
        panic("scheduler: kernel stack overflow");
    }
    if (next->kernel_stack != 0 && !canary_intact(next->kernel_stack)) {
        console::printf("\n  scheduler: %s (slot %u, pid %u) has a smashed "
                        "kernel stack\n",
                        next->name,
                        next_index, next->pid);
        panic("scheduler: kernel stack overflow");
    }

    /* The six callee-saved registers sit between the stamp and the return
     * address, and there is nothing to check them against: a callee-saved
     * register may hold any integer at all, so "is this value sane" has no
     * answer. Both times this went wrong the restored r12 was a perfectly
     * plausible address - once a user stack, once this task's own kernel stack
     * - and the kernel then called through it. What is checkable is not the
     * values but whether the frame they came from is the one this task is
     * supposed to be resuming, which is what the stamp says. */
    if (next->kernel_rsp != 0) {
        const u64 stamp = *reinterpret_cast<const u64*>(next->kernel_rsp);
        if (stamp != next->resume_stamp) {
            console::printf("\n  scheduler: %s (slot %u, pid %u) would resume "
                            "from a frame stamped %llx, expected %llx, "
                            "rsp %p\n",
                            next->name,
                            next_index, next->pid,
                            static_cast<u64>(stamp),
                            static_cast<u64>(next->resume_stamp),
                            reinterpret_cast<void*>(next->kernel_rsp));
            panic("scheduler: a task's saved registers are not its own");
        }
    }

    {
        const u64 resume = *reinterpret_cast<const u64*>(next->kernel_rsp + 56);
        if (resume < 0xFFFFFFFF80000000ull) {
            console::printf("\n  scheduler: %s (slot %u, pid %u) would resume "
                            "at %p from rsp %p\n",
                            next->name,
                            next_index, next->pid,
                            reinterpret_cast<void*>(resume),
                            reinterpret_cast<void*>(next->kernel_rsp));
            panic("scheduler: a task's saved return address is not kernel code");
        }
    }

    /* Stamped before the switch, because context_switch is what writes it onto
     * the outgoing stack and the task has to agree with what lands there. */
    prev->resume_stamp = next_stamp();
    context_switch(&prev->kernel_rsp, next->kernel_rsp, prev->resume_stamp);

    // Reached as the *incoming* task, on whichever CPU resumed it.
    finish_switch();
}

// Lay a fabricated frame on a fresh kernel stack so the first context_switch to
// this task "returns" into `ret_target` with the six callee registers zeroed.
void fabricate(Task* task, u64 ret_target, const void* frame_bytes, usize frame_size)
{
    u64 sp = task->kernel_stack + kStackSize;

    if (frame_bytes != nullptr) {
        // A user TrapFrame is 16-aligned already; user_return consumes it and
        // does not need C-ABI stack alignment.
        sp -= frame_size;
        memcpy(reinterpret_cast<void*>(sp), frame_bytes, frame_size);
    } else {
        // No frame: an 8-byte pad so the trampoline is entered at
        // 16-byte-aligned-plus-8, the alignment a function may assume.
        sp -= 8;
    }

    /* Laid out the way context_switch pops it: rbx deepest, then rbp, r12,
     * r13, r14, r15, and the stamp last so it ends up at the saved rsp. The
     * labels here used to run the other way, which was harmless only because
     * every value is zero. */
    auto* stack = reinterpret_cast<u64*>(sp);
    *--stack = ret_target;      // context_switch's RET lands here
    *--stack = 0;               // rbx
    *--stack = 0;               // rbp
    *--stack = 0;               // r12
    *--stack = 0;               // r13
    *--stack = 0;               // r14
    *--stack = 0;               // r15
    task->resume_stamp = next_stamp();
    *--stack = task->resume_stamp;
    task->kernel_rsp = reinterpret_cast<u64>(stack);
}

} // namespace

void init()
{
    memset(g_tasks, 0, sizeof(g_tasks));

    Task& main_task = g_tasks[0];
    main_task.pid    = g_next_pid++;
    main_task.tgid   = main_task.pid;
    main_task.state  = State::Running;
    set_name(main_task, "main");
    main_task.space  = vmm::kernel_space();
    files::init_table(main_task.files);
    g_task_count = 1;
    for (usize i = 0; i < kMaxCpuSlots; ++i) {
        g_current_by_cpu[i] = 0;
        g_idle_by_cpu[i] = kNoIdle;
    }
}

u32 spawn(const char* name, Entry entry, void* arg)
{
    KernelLock lock;
    cpu::InterruptGuard guard;

    Task* task = alloc_slot();
    if (task == nullptr)
        return 0;

    auto* stack = static_cast<u8*>(kmalloc(kStackSize));
    if (stack != nullptr)
        plant_canary(reinterpret_cast<u64>(stack));
    if (stack == nullptr) {
        task->state = State::Unused;
        return 0;
    }

    task->kernel_stack     = reinterpret_cast<u64>(stack);
    task->kernel_stack_top = task->kernel_stack + kStackSize;
    task->pid        = g_next_pid++;
    task->parent_pid = current()->pid;
    task->state      = State::Ready;
    task->is_user    = false;
    task->space      = vmm::kernel_space();
    set_name(*task, name);
    task->bkl_depth  = 0;
    task->entry      = entry;
    task->arg        = arg;

    fabricate(task, reinterpret_cast<u64>(&kernel_thread_trampoline), nullptr, 0);
    return task->pid;
}

u32 spawn_user(const char* name, vmm::AddressSpace space,
               const TrapFrame& frame, u32 parent_pid)
{
    KernelLock lock;
    cpu::InterruptGuard guard;

    Task* task = alloc_slot();
    if (task == nullptr)
        return 0;

    auto* stack = static_cast<u8*>(kmalloc(kStackSize));
    if (stack != nullptr)
        plant_canary(reinterpret_cast<u64>(stack));
    if (stack == nullptr) {
        task->state = State::Unused;
        return 0;
    }

    task->kernel_stack     = reinterpret_cast<u64>(stack);
    task->kernel_stack_top = task->kernel_stack + kStackSize;
    task->pid        = g_next_pid++;
    task->tgid       = task->pid;      // a new process leads its own group
    // And its own process group and session, until a shell says otherwise.
    // fork overwrites both from the parent immediately below; this is what the
    // three programs the build hands the kernel start with.
    task->pgid       = task->pid;
    task->sid        = task->pid;
    task->report     = -1;
    task->parent_pid = parent_pid;
    task->state      = State::Ready;
    task->is_user    = true;
    task->space      = space;
    set_name(*task, name);
    task->brk        = memory::kUserBrkBase;
    task->mmap_next  = memory::kUserMmapBase;
    task->sig_pending  = 0;
    task->sig_restorer = 0;
    task->bkl_depth    = 0;
    memset(task->sig_handler, 0, sizeof(task->sig_handler));
    /* A clean unit rather than a zeroed buffer - see fpu::init_state for why
     * the difference matters. */
    fpu::init_state(task->fpu_state);
    task->uid = 0;
    task->gid = 0;
    files::init_table(task->files);

    // The first switch to this task returns into user_return, which restores
    // the TrapFrame and drops to ring 3.
    fabricate(task, reinterpret_cast<u64>(&first_user_entry), &frame, sizeof(frame));
    return task->pid;
}

u32 spawn_thread(const TrapFrame& frame)
{
    KernelLock lock;
    cpu::InterruptGuard guard;

    Task* self = current();
    if (!self->is_user || self->space == 0)
        return 0;                       // threads only exist inside a process

    Task* task = alloc_slot();
    if (task == nullptr)
        return 0;

    auto* stack = static_cast<u8*>(kmalloc(kStackSize));
    if (stack != nullptr)
        plant_canary(reinterpret_cast<u64>(stack));
    if (stack == nullptr) {
        task->state = State::Unused;
        return 0;
    }

    task->kernel_stack     = reinterpret_cast<u64>(stack);
    task->kernel_stack_top = task->kernel_stack + kStackSize;
    task->pid        = g_next_pid++;
    // Same group, same address space: this is a thread, not a process. The
    // creator is its parent so wait() can be used to join it.
    task->tgid       = self->tgid;
    task->pgid       = self->pgid;
    task->sid        = self->sid;
    task->report     = -1;
    task->parent_pid = self->pid;
    task->state      = State::Ready;
    task->is_user    = true;
    task->space      = self->space;
    set_name(*task, self->name);
    task->brk        = self->brk;
    task->mmap_next  = self->mmap_next;
    task->uid        = self->uid;
    task->gid        = self->gid;
    /* And the I/O ports its creator was granted. A thread shares the address
     * space; sharing what it may touch on the bus is the same statement. A
     * driver that services two interrupt lines needs a thread on each, and
     * without this the second one faults on its first port read - which is
     * exactly what ps2d did, on the byte that says whether a byte is the
     * keyboard's or the mouse's.
     *
     * A copy rather than the pointer, because a thread can exit on its own and
     * free it. */
    task->io_bitmap = nullptr;
    if (self->io_bitmap != nullptr) {
        task->io_bitmap = static_cast<u8*>(kmalloc(gdt::io_bitmap_bytes()));
        if (task->io_bitmap != nullptr)
            memcpy(task->io_bitmap, self->io_bitmap, gdt::io_bitmap_bytes());
    }
    task->sig_pending  = 0;
    task->sig_restorer = 0;
    task->bkl_depth    = 0;
    memset(task->sig_handler, 0, sizeof(task->sig_handler));
    /* Threads share an address space but not registers, and these are
     * registers. A new thread starts with a clean unit, not the creator's. */
    fpu::init_state(task->fpu_state);
    // The fd table is the leader's; this copy is never consulted (current_files
    // redirects to the group leader), but zero it so close_all cannot double-free.
    files::init_table(task->files);

    fabricate(task, reinterpret_cast<u64>(&first_user_entry), &frame, sizeof(frame));
    return task->pid;
}

void start_preemption() { g_preemption = true; }

bool set_preemption(bool enabled)
{
    const bool previous = g_preemption;
    g_preemption = enabled;
    return previous;
}

namespace {

[[noreturn]] void idle_entry(void*)
{
    for (;;) {
        // The kernel lock must not be held across the halt. Idle is reached by
        // a context switch from a task that held it, so this CPU owns it on
        // arrival - and halting while owning it would freeze every other
        // processor out of the kernel entirely.
        sync::bkl::release_all();
        cpu::wait_for_interrupt();       // sti; hlt until something happens

        // Something woke us; go and look for work - holding the lock, which is
        // the part that used to be missing.
        //
        // This said reacquire(held), and reacquire does nothing when held is
        // zero. A CPU really can arrive at idle holding nothing: switch_to
        // hands the incoming task its own saved depth, and idle's is zero, so
        // the handoff *releases*. release_all then returned zero, reacquire
        // declined to take anything, and pick_next and switch_to ran with no
        // lock at all. Two idle processors would both see the same freshly
        // woken task with on_cpu still clear, and both switch to it - two CPUs
        // executing on one kernel stack, which is what every wild jump in this
        // bug turned out to be.
        cpu::cli();
        sync::bkl::acquire();
        switch_to(pick_next());
    }
}

} // namespace

void start_idle_for(u32 cpu_slot)
{
    if (cpu_slot >= kMaxCpuSlots)
        return;
    KernelLock lock;
    const u32 pid = spawn("idle", idle_entry, nullptr);
    for (u32 i = 0; i < g_task_count; ++i) {
        if (g_tasks[i].pid == pid) {
            g_idle_by_cpu[cpu_slot] = i;
            break;
        }
    }
}

void start_idle() { start_idle_for(0); }

[[noreturn]] void enter_scheduler_on_this_cpu()
{
    sync::bkl::acquire();
    cpu::cli();

    // Adopt this CPU's idle task before doing anything that could switch away.
    // Until now current_index() still named task 0 - the bootstrap processor's -
    // and the first context switch would have saved this CPU's state straight
    // over the BSP's, which is as bad as it sounds.
    //
    // The AP is running on its trampoline stack, so that stack becomes the idle
    // task's kernel stack from here on. Idle never exits, so the unused one
    // spawn() allocated is simply never reclaimed.
    const u32 idle = idle_index();
    current_index() = idle;
    g_tasks[idle].state = State::Running;

    // From here it is an ordinary scheduling CPU: run whatever is ready, and
    // fall back to idling when nothing is.
    for (;;) {
        switch_to(pick_next());

        sync::bkl::release_all();
        cpu::wait_for_interrupt();      // sti; hlt - lets other CPUs in
        // Unconditionally, not reacquire(held): see the idle loop above. A
        // depth of zero on the way in must not mean "schedule without the
        // lock", which is what reacquire's early return amounted to.
        cpu::cli();
        sync::bkl::acquire();
        cpu::cli();
    }
}

void sleep_ticks(u64 ticks)
{
    if (ticks == 0) {
        yield();
        return;
    }
    KernelLock lock;
    cpu::InterruptGuard guard;
    current()->wake_tick = timer::ticks() + ticks;
    current()->state = State::Blocked;
    switch_to(pick_next());
}

void yield()
{
    KernelLock lock;
    cpu::InterruptGuard guard;
    switch_to(pick_next());
}

void block_on(u64 channel)
{
    KernelLock lock;
    // Caller holds interrupts off, so no wake can slip in between the check that
    // led here and this block.
    current()->wait_channel = channel;
    current()->state = State::Blocked;
    switch_to(pick_next());
    // Reached again once woken and rescheduled.
}

void block_on_until(u64 channel, u64 ticks)
{
    KernelLock lock;
    Task* self = current();
    self->wait_channel = channel;
    if (ticks != 0)
        self->wake_tick = timer::ticks() + ticks;
    self->state = State::Blocked;
    switch_to(pick_next());
    // Woken by one or the other; which does not matter, because the caller is
    // going to look at the world again either way.
    self->wake_tick = 0;
    self->wait_channel = 0;
}

void wake(u64 channel)
{
    KernelLock lock;
    for (u32 i = 0; i < g_task_count; ++i) {
        if (g_tasks[i].state == State::Blocked && g_tasks[i].wait_channel == channel) {
            g_tasks[i].state = State::Ready;
            g_tasks[i].wait_channel = 0;
        }
    }
}

u32 wake_n(u64 channel, u32 limit)
{
    KernelLock lock;
    u32 woken = 0;
    for (u32 i = 0; i < g_task_count && woken < limit; ++i) {
        if (g_tasks[i].state == State::Blocked && g_tasks[i].wait_channel == channel) {
            g_tasks[i].state = State::Ready;
            g_tasks[i].wait_channel = 0;
            ++woken;
        }
    }
    return woken;
}

u32 fork_current(const TrapFrame& parent_user)
{
    KernelLock lock;
    cpu::InterruptGuard guard;

    Task* parent = current();

    const vmm::AddressSpace child_space = vmm::fork_address_space(parent->space);
    if (child_space == 0)
        return 0;

    // The child resumes exactly where the parent's fork syscall returns, but
    // fork returns 0 in the child.
    TrapFrame child_frame = parent_user;
    child_frame.rax = 0;

    const u32 child = spawn_user(parent->name, child_space, child_frame, parent->pid);
    if (child == 0) {
        vmm::destroy_address_space(child_space);
        return 0;
    }

    // A child inherits the parent's open files and working directory. The pipe
    // ends among them gain a reference: both processes now hold each one.
    Task* child_task = find(child);
    if (child_task != nullptr) {
        child_task->files = parent->files;
        child_task->brk   = parent->brk;
        child_task->mmap_next = parent->mmap_next;
        files::inherit(child_task->files);

        // The group and the session are the parent's. A shell puts a child in
        // its own group straight after forking, and does the same from the
        // child before it execs, because whichever runs first has to win - but
        // until one of them does, the child is where its parent was.
        child_task->pgid = parent->pgid;
        child_task->sid  = parent->sid;

        // Dispositions carry across fork (the image is the same, so its handler
        // addresses are still valid); pending signals do not.
        memcpy(child_task->sig_handler, parent->sig_handler,
               sizeof(child_task->sig_handler));
        child_task->sig_restorer = parent->sig_restorer;
        child_task->sig_pending  = 0;

        /* The child resumes inside the same expression the parent is in, so it
         * needs the parent's registers - these as much as the general ones.
         * The parent is the task making this call, so what is live in the unit
         * right now is what has to be copied; saving first is not optional. */
        fpu::save(parent->fpu_state);
        child_task->fpu_state = parent->fpu_state;

        /* Port permissions carry across, with a copy of their own: a driver
         * that forks a worker meant the worker to be able to do the work, and
         * sharing one bitmap would mean a later grant to either reached both. */
        child_task->io_bitmap = nullptr;
        if (parent->io_bitmap != nullptr) {
            child_task->io_bitmap =
                static_cast<u8*>(kmalloc(gdt::io_bitmap_bytes()));
            if (child_task->io_bitmap != nullptr)
                memcpy(child_task->io_bitmap, parent->io_bitmap,
                       gdt::io_bitmap_bytes());
        }
        // Zero, not one. The parent returns from this syscall and releases the
        // lock on its way out; the child does not - it is fabricated to enter
        // ring 3 through first_user_entry, which goes straight to IRETQ and
        // never reaches a release. Handing it the lock therefore leaked it: the
        // child carried it into user mode and no one ever gave it back.
        //
        // On one processor that was invisible, because the CPU holding it just
        // re-entered recursively and never noticed. On two it is fatal - the
        // other CPU spins for a lock nobody holds, and since every device
        // interrupt is routed to one processor, the machine loses its keyboard
        // and mouse the first time a process forks.
        child_task->bkl_depth = 0;
        child_task->uid = parent->uid;
        child_task->gid = parent->gid;
    }
    /* The child holds what the parent held. Copied rather than shared: closing
     * one in the child must not close the parent's, which is the difference
     * between inheriting authority and sharing it. */
    object::inherit(parent->tgid, child);

    return child;
}

[[noreturn]] void exit_group(i32 code)
{
    // SIGKILL rather than retiring them here. A thread blocked inside a syscall
    // is sitting on a kernel stack halfway through something, and marking it
    // dead abandons that stack mid-call - whatever it had allocated is leaked,
    // and whatever it was about to free gets freed twice. Killing it instead
    // lets it unwind through its own exit path, which is the only code that
    // knows what it was holding.
    {
        sync::bkl::acquire();
        cpu::InterruptGuard guard;
        Task* self = current();
        for (u32 i = 0; i < g_task_count; ++i) {
            Task& t = g_tasks[i];
            if (&t == self || t.tgid != self->tgid || !t.is_user)
                continue;
            if (t.state == State::Unused || t.state == State::Dead ||
                t.state == State::Zombie)
                continue;
            t.sig_pending |= 1u << signals::kSigKill;
            // A signal is a reason to run: one parked on a channel has to come
            // back so that the blocking call it is inside notices.
            if (t.state == State::Blocked) {
                t.state = State::Ready;
                t.wait_channel = 0;
            }
        }
        sync::bkl::release();
    }
    exit_current(code);
}

// Suspend this task where it stands, and tell its parent so.
//
// Beside exit_current because it is the same shape - the task stops running and
// somebody waiting on it has to hear about it - and deliberately not the same
// thing: nothing is released. The address space, the open files, the kernel
// stack and the pending IPC all stay exactly as they are, because the whole
// point is that SIGCONT can put it back to work with none of it noticing.
void stop_current(int signo)
{
    sync::bkl::acquire();
    cpu::cli();

    Task* self = current();
    self->report = signals::kStopped | signo;
    self->state  = State::Stopped;

    Task* parent = find(self->parent_pid);
    if (parent != nullptr && parent->state == State::Blocked)
        parent->state = State::Ready;

    // The lock passes to whatever runs next, as it does for an exiting task:
    // this one is not going to reach a release.
    switch_to(pick_next());
    // ...and this is where SIGCONT brings it back, with the lock its own again.
}

void exit_current(i32 code)
{
    // Acquired and never released here: the task is going away, and the lock
    // passes to whatever runs next through the handoff in switch_to.
    sync::bkl::acquire();
    cpu::cli();

    Task* self = current();
    self->exit_code = code;

    // Release open files - notably pipe ends, so the other side sees EOF. The
    // table is shared across a thread group, so only the last thread out closes
    // it; otherwise a thread exiting would pull the fds from under its siblings.
    if (!group_still_alive(self)) {
        files::close_all(group_leader(self)->files);
        // If this was the process holding the screen, the console takes it
        // back - however the process happened to die.
        console::reclaim_display(self->tgid);
    }

    // Drop the user address space now; the kernel stack (in the shared heap)
    // stays until a parent reaps it. Threads share one space, so it is freed
    // only once the last of them has let go of it. Switch off it before freeing.
    if (self->is_user && self->space != 0) {
        const vmm::AddressSpace space = self->space;
        self->space = 0;
        if (!space_still_used(space, self)) {
            vmm::switch_address_space(vmm::kernel_space());
            vmm::destroy_address_space(space);
        }
    }

    /* Anyone mid-conversation with this task has to be let go. A server that
     * dies owing an answer would otherwise leave its clients asleep on a reply
     * that is never coming - which in a system built on message passing is how
     * one crash becomes a frozen machine. */
    if (self->is_user && !group_still_alive(self)) {
        ipc::abandon(self->tgid);
        /* And the shared segments it made. libc opens one per process to talk
         * to vfsd, keyed by pid so no two processes share a transfer buffer -
         * so every process that touches a file makes one, and nothing was
         * giving them back. Braced, because unbraced this ran for every
         * exiting *thread* and would tear down a live process's segments. */
        shm::abandon(self->tgid);
        /* And every handle it held. A capability is authority, and authority
         * that outlives the thing holding it is exactly what this design
         * exists to prevent. */
        object::close_all(self->tgid);
    }


    self->state = self->is_user ? State::Zombie : State::Dead;

    // Wake a parent that is blocked in wait().
    Task* parent = find(self->parent_pid);
    if (parent != nullptr && parent->state == State::Blocked)
        parent->state = State::Ready;

    switch_to(pick_next());
    panic("scheduler: exited task was scheduled again");
    __builtin_unreachable();
}

// Which children this call is asking about. The four cases POSIX gives waitpid,
// and they are all useful here: a shell waits for one process when it is
// foregrounding a job, and for a whole group when the job is a pipeline.
bool wanted(const Task& t, i64 which, u32 me)
{
    if (which > 0)  return t.pid == static_cast<u32>(which);
    if (which == -1) return true;
    if (which == 0)  return t.pgid == pgid_of(me);
    return t.pgid == static_cast<u32>(-which);
}

i64 wait_child(i64 which, i32* status, u32 options)
{
    KernelLock lock;
    for (;;) {
        cpu::InterruptGuard guard;

        bool any_child = false;
        for (u32 i = 0; i < g_task_count; ++i) {
            Task& t = g_tasks[i];
            if (t.parent_pid != current()->pid || t.state == State::Unused)
                continue;
            if (!wanted(t, which, current()->pid))
                continue;
            any_child = true;

            // A stop or a continue is news about a child that is still there,
            // so it is reported without reaping anything - and only once,
            // which is what clearing the note is for. A caller that did not ask
            // to hear about these gets no mention of them at all, which is why
            // an ordinary wait() still blocks straight through a suspension.
            if (t.report != -1) {
                const bool is_stop = (t.report & signals::kStatusKind) ==
                                     signals::kStopped;
                if ((is_stop  && (options & kWaitUntraced)  != 0) ||
                    (!is_stop && (options & kWaitContinued) != 0)) {
                    const i64 pid = t.pid;
                    if (status != nullptr)
                        *status = t.report;
                    t.report = -1;
                    return pid;
                }
            }

            // The group *leader's* slot has to outlive its threads: it holds
            // the open files they all share, and reusing it now would pull
            // those out from under the survivors. An ordinary thread's slot has
            // no such tenant and is reaped straight away - holding those back
            // would make thread_join wait for a process that is still running.
            if (t.state == State::Zombie && t.tgid == t.pid &&
                group_still_alive(&t))
                continue;

            // A zombie is not finished with its kernel stack until the CPU it
            // was running on has actually switched off it.
            //
            // exit_current marks the task Zombie and then calls switch_to, and
            // between those two the processor is still executing on that stack
            // - pushing, popping and finally running context_switch itself.
            // Freeing it in that window hands the memory back to the heap while
            // a CPU is still using it, and the next thing to be allocated (a
            // kernel stack, an I/O bitmap, a file buffer) is written straight
            // over a live saved frame. What comes out the other side is an
            // IRETQ to whatever now occupies the slot the return address was
            // in, which is why the faulting address always looked like a
            // pointer into the kernel heap.
            //
            // on_cpu is cleared by the *next* task's finish_switch, which is
            // the first moment the stack is genuinely nobody's. Waiting for it
            // is a handful of microseconds at most.
            if (t.state == State::Zombie &&
                __atomic_load_n(&t.on_cpu, __ATOMIC_ACQUIRE))
                continue;

            if (t.state == State::Zombie) {
                const i64 pid = t.pid;
                if (status != nullptr)
                    *status = t.exit_code;
                kfree(reinterpret_cast<void*>(t.kernel_stack));
                if (t.io_bitmap != nullptr) {
                    kfree(t.io_bitmap);
                    t.io_bitmap = nullptr;
                }
                t.state = State::Unused;
                return pid;
            }
        }

        if (!any_child)
            return kWaitNoChildren;

        if ((options & kWaitNoHang) != 0)
            return 0;           // nothing ready, and the caller will not wait

        // A signal arriving is an answer too. Without this a shell waiting for
        // a job would sit through its own Ctrl-C: the delivery check runs on
        // the way out to user mode, and a wait that goes straight back to
        // sleep never gets there.
        if (signal_pending())
            return kWaitInterrupted;

        // A child is still running; block until one exits, stops or is
        // continued, then look again.
        current()->state = State::Blocked;
        switch_to(pick_next());
    }
}

// --- signals ----------------------------------------------------------------

bool signal_send(u32 pid, int signo)
{
    if (signo < 0 || signo >= static_cast<int>(kMaxSignals))
        return false;
    cpu::InterruptGuard guard;

    Task* target = find(pid);
    if (target == nullptr || !target->is_user ||
        target->state == State::Zombie || target->state == State::Dead)
        return false;

    // Signal 0 delivers nothing: every check above still runs, so it answers
    // "is this process still alive?" and nothing else. The window server needs
    // that - outside the kernel, nothing tells it when a client dies.
    if (signo == 0)
        return true;

    // Stopping and continuing cancel each other, and the loser is whichever
    // arrived first. Without this a program sent SIGSTOP and then SIGCONT would
    // come back and immediately stop again on the signal still queued behind
    // it, which reads from the outside as SIGCONT not working.
    if (signo == signals::kSigCont)
        target->sig_pending &= ~((1u << signals::kSigStop) |
                                 (1u << signals::kSigTstp) |
                                 (1u << signals::kSigTtin) |
                                 (1u << signals::kSigTtou));
    else if (signals::default_stops(signo))
        target->sig_pending &= ~(1u << signals::kSigCont);

    target->sig_pending |= 1u << signo;

    // SIGCONT acts before any disposition is consulted, and has to: a stopped
    // task is not running, so it cannot reach the delivery check that would
    // otherwise notice. Restarting it *is* the signal. A handler for it, if
    // there is one, then runs on the way back out like any other.
    if (signo == signals::kSigCont && target->state == State::Stopped) {
        target->state  = State::Ready;
        target->report = signals::kContinued;
        Task* parent = find(target->parent_pid);
        if (parent != nullptr && parent->state == State::Blocked)
            parent->state = State::Ready;
    }

    // SIGKILL has to reach a stopped task too, which by definition is not going
    // to run to collect it. Nothing may be unkillable by virtue of being
    // suspended - that would be a way to make a process permanent.
    if (signo == signals::kSigKill && target->state == State::Stopped)
        target->state = State::Ready;

    // A signal is a reason to run: a target parked in wait() or on a channel
    // has to come back so the delivery check on its way out to user mode runs.
    if (target->state == State::Blocked) {
        target->state = State::Ready;
        target->wait_channel = 0;
    }
    return true;
}

// Every process in a group. This is what a terminal does with Ctrl-C and what
// a shell does with `kill %1`: the unit a person means by "that job" is the
// group, not whichever process of it happens to be listed first.
//
// Threads are skipped - a signal to a group is meant once per process, and the
// leader is where a process's dispositions live anyway.
int signal_send_group(u32 pgid, int signo)
{
    if (signo < 0 || signo >= static_cast<int>(kMaxSignals))
        return -1;

    int sent = 0;
    // Collected first and delivered after, because signal_send can restart a
    // stopped task and reorder nothing else - but walking the table while
    // sending to it is the kind of thing that only breaks once the table moves.
    u32 targets[kMaxTasks];
    u32 count = 0;
    {
        cpu::InterruptGuard guard;
        for (u32 i = 0; i < g_task_count && count < kMaxTasks; ++i) {
            const Task& t = g_tasks[i];
            if (t.state == State::Unused || t.state == State::Dead ||
                t.state == State::Zombie || !t.is_user)
                continue;
            if (t.pgid != pgid || t.tgid != t.pid)
                continue;
            targets[count++] = t.pid;
        }
    }
    for (u32 i = 0; i < count; ++i)
        if (signal_send(targets[i], signo))
            ++sent;
    return sent > 0 ? sent : -1;
}

// --- process groups and sessions ---------------------------------------------

u32 pgid_of(u32 pid)
{
    cpu::InterruptGuard guard;
    const Task* t = find(pid == 0 ? current()->pid : pid);
    return t != nullptr ? t->pgid : 0;
}

u32 sid_of(u32 pid)
{
    cpu::InterruptGuard guard;
    const Task* t = find(pid == 0 ? current()->pid : pid);
    return t != nullptr ? t->sid : 0;
}

// Move a process into a group. pid 0 means the caller, pgid 0 means "a group of
// its own, named after it".
//
// A shell calls this twice for every child - once in the parent and once in the
// child - because it cannot know which of the two runs first and the child must
// be in its group before it is sent anything. Doing it twice is the standard
// answer and is why this is idempotent.
bool set_pgid(u32 pid, u32 pgid)
{
    cpu::InterruptGuard guard;
    Task* self = current();
    Task* t = find(pid == 0 ? self->pid : pid);
    if (t == nullptr || !t->is_user)
        return false;
    // Only the process itself or its parent, and only within one session: a
    // group is a subdivision of a session, and a process cannot be moved into
    // some other login's idea of a job.
    if (t->pid != self->pid && t->parent_pid != self->pid)
        return false;
    if (t->sid != self->sid)
        return false;
    if (pgid == 0)
        pgid = t->pid;
    // The group has to be in this session, which it is if it already exists
    // here or if the process is starting it.
    if (pgid != t->pid) {
        bool found = false;
        for (u32 i = 0; i < g_task_count && !found; ++i)
            found = g_tasks[i].state != State::Unused &&
                    g_tasks[i].pgid == pgid && g_tasks[i].sid == t->sid;
        if (!found)
            return false;
    }
    t->pgid = pgid;
    return true;
}

// Start a new session: a new group in a new session, both named after the
// caller, and no controlling terminal. A process that already leads a group
// cannot, because the new session would have to take the group with it.
u32 set_sid()
{
    cpu::InterruptGuard guard;
    Task* self = current();
    if (self->pgid == self->pid && self->sid == self->pid)
        return self->sid;       // already exactly this
    for (u32 i = 0; i < g_task_count; ++i)
        if (g_tasks[i].state != State::Unused && g_tasks[i].pgid == self->pid)
            return 0;           // leads a group already
    self->sid  = self->pid;
    self->pgid = self->pid;
    return self->sid;
}

int signal_take_pending()
{
    Task* self = current();
    if (self->sig_pending == 0)
        return 0;
    for (u32 signo = 1; signo < kMaxSignals; ++signo) {
        if (self->sig_pending & (1u << signo)) {
            self->sig_pending &= ~(1u << signo);
            return static_cast<int>(signo);
        }
    }
    return 0;
}

bool signal_pending() { return current()->sig_pending != 0; }

u64 signal_handler(int signo)
{
    if (signo <= 0 || signo >= static_cast<int>(kMaxSignals))
        return signals::kSigDefault;
    return group_leader(current())->sig_handler[signo];
}

void signal_set_handler(int signo, u64 handler)
{
    if (signo <= 0 || signo >= static_cast<int>(kMaxSignals))
        return;
    // These two must stay what they are, or a process could make itself
    // unkillable - or, just as bad, unstoppable.
    if (signals::uncatchable(signo))
        return;
    group_leader(current())->sig_handler[signo] = handler;
}

void signal_reset_all()
{
    Task* leader = group_leader(current());
    for (u32 i = 0; i < kMaxSignals; ++i)
        leader->sig_handler[i] = signals::kSigDefault;
    leader->sig_restorer = 0;
    leader->sig_pending = 0;
}

// What execve calls once it knows which program it is about to become. Until
// this existed a task kept whatever name it was forked with, so everything
// descended from init was called init - including, misleadingly, in a crash
// report.
void set_current_name(const char* name)
{
    cpu::InterruptGuard guard;
    set_name(*current(), name);
}

u64  signal_restorer()          { return group_leader(current())->sig_restorer; }
void signal_set_restorer(u64 r) { group_leader(current())->sig_restorer = r; }

void load_average(u64 out[3])
{
    cpu::InterruptGuard guard;
    for (int i = 0; i < 3; ++i)
        out[i] = g_load[i];
}

u32 current_uid() { return current()->uid; }
u32 current_gid() { return current()->gid; }

bool set_current_uid(u32 uid)
{
    Task* self = current();
    // Only root may become another user; anyone else is refused. There is no
    // saved-set-uid subtlety here because there is no setuid-on-exec yet.
    if (self->uid != 0 && uid != self->uid)
        return false;
    self->uid = uid;
    return true;
}

bool set_current_gid(u32 gid)
{
    Task* self = current();
    if (self->uid != 0 && gid != self->gid)
        return false;
    self->gid = gid;
    return true;
}

u64 credentials_of(u32 pid)
{
    KernelLock lock;
    Task* t = find(pid);
    // No such process reads as "not root, not in any group", which is the safe
    // way to be wrong. The two are packed into one word because a server
    // deciding whether a caller may touch a file needs both, and asking twice
    // would let them come from different moments.
    if (t == nullptr)
        return 0xFFFFFFFFFFFFFFFFull;
    return static_cast<u64>(t->gid) << 32 | t->uid;
}

bool set_credentials_of(u32 pid, u32 uid, u32 gid)
{
    // Root only - and in practice that means authd, which is the only thing
    // that calls it. The ordinary setuid rule ("only root may change
    // credentials") is exactly wrong for a login, because the whole point is
    // that a correct password authorises the change and the person logging in
    // is not root yet. Moving the check here keeps that: authd holds the
    // privilege, checks the password, and then asks for the change on behalf
    // of a caller who could never have asked for it themselves.
    if (current()->uid != 0)
        return false;

    KernelLock lock;
    Task* target = find(pid);
    if (target == nullptr)
        return false;

    // Every thread of the process, not just the one that asked. Credentials
    // that differ between threads of one program are a bug with no upside.
    const u32 group = target->tgid;
    for (u32 i = 0; i < g_task_count; ++i) {
        if (g_tasks[i].state == State::Unused || g_tasks[i].tgid != group)
            continue;
        g_tasks[i].uid = uid;
        g_tasks[i].gid = gid;
    }
    return true;
}

u32 current_tid()  { return current()->pid; }
u32 current_tgid() { return current()->tgid; }

i64 grant_io_ports(u16 base, u32 count)
{
    KernelLock lock;
    Task* self = current();
    /* Root only, for now: there is no way yet to say "this program is the
     * sound driver" other than who started it. When drivers are launched by
     * something that knows what they are, the grant should come from there. */
    if (!self->is_user || self->uid != 0 || count == 0)
        return -1;
    const u32 last = static_cast<u32>(base) + count - 1;
    if (last > 0xFFFF)
        return -1;

    if (self->io_bitmap == nullptr) {
        self->io_bitmap = static_cast<u8*>(kmalloc(gdt::io_bitmap_bytes()));
        if (self->io_bitmap == nullptr)
            return -1;
        /* Everything denied to start with; a grant clears bits rather than
         * setting them, so a task can only ever gain what it names. */
        memset(self->io_bitmap, 0xFF, gdt::io_bitmap_bytes());
    }
    for (u32 port = base; port <= last; ++port)
        self->io_bitmap[port / 8] &= static_cast<u8>(~(1u << (port % 8)));

    /* This task is the one running, so the change has to reach the TSS now
     * rather than at the next switch. */
    gdt::set_io_bitmap(percpu::active(), self->io_bitmap);
    return 0;
}

u32 current_pid() { return current()->pid; }

vmm::AddressSpace current_task_space() { return current()->space; }

void current_task_set_space(vmm::AddressSpace space) { current()->space = space; }

// Threads share one open-file table, the group leader's: opening a file in one
// thread must be visible in its siblings.
files::Table& current_files() { return group_leader(current())->files; }

// The heap and the mmap arena live in the address space, which a thread group
// shares - so both cursors belong to the group leader. Keeping them per-task
// would hand two threads the same addresses.
u64  current_brk()            { return group_leader(current())->brk; }
void set_current_brk(u64 brk) { group_leader(current())->brk = brk; }

void reset_current_fpu()
{
    Task* task = current();
    fpu::init_state(task->fpu_state);
    fpu::restore(task->fpu_state);
}

u64  current_mmap_next()             { return group_leader(current())->mmap_next; }
void set_current_mmap_next(u64 next) { group_leader(current())->mmap_next = next; }

u32 snapshot(TaskInfo* out, u32 max)
{
    KernelLock lock;
    cpu::InterruptGuard guard;
    u32 n = 0;
    for (u32 i = 0; i < g_task_count && n < max; ++i) {
        const Task& t = g_tasks[i];
        if (t.state == State::Unused)
            continue;
        TaskInfo& o = out[n++];
        o.pid = t.pid;
        o.tgid = t.tgid;
        o.parent = t.parent_pid;
        o.uid = t.uid;
        o.state = static_cast<u32>(t.state);
        o.is_user = t.is_user ? 1u : 0u;
        o.pgid = t.pgid;
        o.sid  = t.sid;
        o.ticks = t.ticks;
        // What the process has asked for beyond its image: the break and the
        // mmap arena are the two things it grows, and together they are the
        // number a resource monitor is actually being asked for.
        o.bytes = t.is_user && t.space != 0
                ? (t.mmap_next > memory::kUserMmapBase
                       ? t.mmap_next - memory::kUserMmapBase : 0)
                : 0;
        u32 k = 0;
        while (t.name[k] != '\0' && k < 31) { o.name[k] = t.name[k]; ++k; }
        o.name[k] = '\0';
    }
    return n;
}

u32 cpu_stats(CpuStat* out, u32 max)
{
    const u32 n = static_cast<u32>(smp::cpu_count());
    const u32 give = n < max ? n : max;
    for (u32 i = 0; i < give; ++i) {
        out[i].busy = g_busy_by_cpu[i];
        out[i].idle = g_idle_by_cpu_ticks[i];
    }
    return give;
}

u32 alive_count()
{
    u32 n = 0;
    for (u32 i = 0; i < g_task_count; ++i) {
        const State s = g_tasks[i].state;
        if (s == State::Ready || s == State::Running || s == State::Blocked)
            ++n;
    }
    return n;
}

void on_tick()
{
    // Wake anything whose sleep has run out. Sleeping is what lets a polling
    // process - a window server, say - poll at a sane rate instead of spinning:
    // a task that never blocks holds the kernel lock over and over and can stop
    // another processor from getting into the kernel at all.
    /* The load average: how many tasks wanted to run, smoothed.
     *
     * Sampled every five seconds rather than every tick, and decayed towards
     * the new sample rather than replaced by it - which is what makes it an
     * average over a window instead of an instantaneous count that flickers.
     * Kept in hundredths, because the kernel has no floating point and a load
     * of "1" and a load of "1.75" are worth telling apart.
     *
     * The three windows are the traditional one, five and fifteen minutes, and
     * the decay constants are the ones every UNIX uses: exp(-5/60),
     * exp(-5/300), exp(-5/900), scaled by 2048.
     */
    if ((timer::ticks() % (5 * timer::kFrequencyHz)) == 0) {
        u32 runnable = 0;
        for (u32 i = 0; i < g_task_count; ++i) {
            const Task& t = g_tasks[i];
            if (is_idle_task(i))
                continue;
            if (t.state == State::Running || t.state == State::Ready)
                ++runnable;
        }
        constexpr u64 kDecay[3] = { 1884, 2014, 2037 };   // of 2048
        const u64 sample = static_cast<u64>(runnable) * 100;
        for (int w = 0; w < 3; ++w)
            g_load[w] = (g_load[w] * kDecay[w] +
                         sample * (2048 - kDecay[w])) / 2048;
    }

    const u64 now = timer::ticks();
    for (u32 i = 0; i < g_task_count; ++i) {
        Task& task = g_tasks[i];
        if (task.state == State::Blocked && task.wake_tick != 0 &&
            now >= task.wake_tick) {
            task.wake_tick = 0;
            task.wait_channel = 0;
            task.state = State::Ready;
        }
    }

    if (!g_preemption)
        return;
    if (g_quantum > 0)
        --g_quantum;
    if (g_quantum == 0)
        g_need_resched = true;
}

void on_irq_return()
{
    if (!g_preemption || !g_need_resched)
        return;

    // Not while the interrupted code was itself inside the kernel.
    //
    // The kernel lock is handed to whichever task is switched to, so preempting
    // a task that holds it does not keep the other processors out - it lets
    // them straight in, on top of work that is halfway done. Every shared
    // structure in the kernel is exposed by that: a heap block being split, an
    // ATA channel mid-transfer, a shared-memory slot chosen but not yet filled.
    // Each of those was a real crash, and each looked like a different bug.
    //
    // A big kernel lock only means anything if holding it is a promise to
    // finish, so involuntary preemption has to wait. g_need_resched stays set
    // and the next tick after the syscall returns acts on it, which costs a
    // task at most one extra tick of its turn. Yielding on purpose is still
    // allowed and still switches: code that calls yield() has chosen a moment
    // where its own invariants hold.
    //
    // Depth is 1 when the interrupted task was in user mode - that one acquire
    // is this handler's own. Anything above that was already in the kernel.
    if (sync::bkl::depth() > 1)
        return;

    g_need_resched = false;
    switch_to(pick_next());
}


void current_stack_bounds(u64* base, u64* top)
{
    const Task* task = current();
    if (base != nullptr) *base = task != nullptr ? task->kernel_stack : 0;
    if (top != nullptr)  *top  = task != nullptr ? task->kernel_stack_top : 0;
}

const char* stack_owner(u64 address, u32* pid_out)
{
    for (u32 i = 0; i < g_task_count; ++i) {
        const Task& t = g_tasks[i];
        if (t.state == State::Unused || t.kernel_stack == 0)
            continue;
        if (address >= t.kernel_stack && address < t.kernel_stack_top) {
            if (pid_out != nullptr) *pid_out = t.pid;
            return t.name;
        }
    }
    return nullptr;
}

const char* current_name()
{
    const Task* task = current();
    return task != nullptr ? task->name : "?";
}

bool current_is_user()
{
    const Task* task = current();
    return task != nullptr && task->is_user;
}

} // namespace scheduler
