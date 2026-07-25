#include <leah/cpu.hpp>
#include <leah/gdt.hpp>
#include <leah/heap.hpp>
#include <leah/panic.hpp>
#include <leah/scheduler.hpp>
#include <leah/string.hpp>

// Implemented in context.asm, user_entry.asm and syscall_entry.asm.
extern "C" void context_switch(u64* save_rsp, u64 load_rsp);
extern "C" void user_return();
extern "C" void set_syscall_stack(u64 rsp);

namespace scheduler {
namespace {

constexpr usize kMaxTasks   = 32;
constexpr usize kStackSize  = 16 * 1024;
constexpr u32   kQuantum    = 1;            // one 10 ms tick per slice

enum class State : u8 {
    Unused,
    Ready,
    Running,
    Blocked,        // in wait(), no zombie child yet
    Zombie,         // exited, waiting to be reaped
    Dead,           // finished kernel thread
};

struct Task {
    u64   kernel_rsp;        // saved scheduler context (points into kernel_stack)
    u64   kernel_stack;      // base, for freeing
    u64   kernel_stack_top;  // loaded into TSS.rsp0 while this task runs
    u32   pid;
    u32   parent_pid;
    State state;
    bool  is_user;
    vmm::AddressSpace space; // 0 for kernel threads (they use the kernel space)
    i32   exit_code;
    const char* name;
    Entry entry;             // kernel threads only
    void* arg;
};

Task g_tasks[kMaxTasks];
u32  g_task_count = 0;
u32  g_current = 0;                  // index into g_tasks
u32  g_next_pid = 1;

bool g_preemption = false;
u32  g_quantum = kQuantum;
volatile bool g_need_resched = false;

Task* current() { return &g_tasks[g_current]; }

Task* find(u32 pid)
{
    for (u32 i = 0; i < g_task_count; ++i) {
        if (g_tasks[i].pid == pid && g_tasks[i].state != State::Unused)
            return &g_tasks[i];
    }
    return nullptr;
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

[[noreturn]] void kernel_thread_trampoline()
{
    cpu::sti();
    Task* self = current();
    self->entry(self->arg);
    exit_current(0);
}

u32 pick_next()
{
    for (u32 step = 1; step <= g_task_count; ++step) {
        const u32 index = (g_current + step) % g_task_count;
        if (g_tasks[index].state == State::Ready)
            return index;
    }
    if (current()->state == State::Running || current()->state == State::Ready)
        return g_current;
    panic("scheduler: nothing runnable");
}

// Enter with interrupts disabled. Besides swapping kernel stacks, this loads
// the incoming task's page table and the ring-0 stack the CPU will use if that
// task takes an interrupt or a syscall while in ring 3.
void switch_to(u32 next_index)
{
    if (next_index == g_current)
        return;

    Task* prev = current();
    Task* next = &g_tasks[next_index];

    if (prev->state == State::Running)
        prev->state = State::Ready;
    next->state = State::Running;
    g_current = next_index;
    g_quantum = kQuantum;

    // The ring-0 stack for an interrupt from ring 3, and the stack SYSCALL
    // switches to, are both this task's own kernel stack - so a syscall or
    // interrupt handler that blocks keeps its state on a stack no other task
    // will reuse.
    gdt::set_kernel_stack(next->kernel_stack_top);
    set_syscall_stack(next->kernel_stack_top);
    vmm::switch_address_space(next->space != 0 ? next->space : vmm::kernel_space());

    context_switch(&prev->kernel_rsp, next->kernel_rsp);
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

    auto* stack = reinterpret_cast<u64*>(sp);
    *--stack = ret_target;      // context_switch's RET lands here
    *--stack = 0;               // r15
    *--stack = 0;               // r14
    *--stack = 0;               // r13
    *--stack = 0;               // r12
    *--stack = 0;               // rbp
    *--stack = 0;               // rbx
    task->kernel_rsp = reinterpret_cast<u64>(stack);
}

} // namespace

void init()
{
    memset(g_tasks, 0, sizeof(g_tasks));

    Task& main_task = g_tasks[0];
    main_task.pid    = g_next_pid++;
    main_task.state  = State::Running;
    main_task.name   = "main";
    main_task.space  = vmm::kernel_space();
    g_task_count = 1;
    g_current = 0;
}

u32 spawn(const char* name, Entry entry, void* arg)
{
    cpu::InterruptGuard guard;

    Task* task = alloc_slot();
    if (task == nullptr)
        return 0;

    auto* stack = static_cast<u8*>(kmalloc(kStackSize));
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
    task->name       = name;
    task->entry      = entry;
    task->arg        = arg;

    fabricate(task, reinterpret_cast<u64>(&kernel_thread_trampoline), nullptr, 0);
    return task->pid;
}

u32 spawn_user(const char* name, vmm::AddressSpace space,
               const TrapFrame& frame, u32 parent_pid)
{
    cpu::InterruptGuard guard;

    Task* task = alloc_slot();
    if (task == nullptr)
        return 0;

    auto* stack = static_cast<u8*>(kmalloc(kStackSize));
    if (stack == nullptr) {
        task->state = State::Unused;
        return 0;
    }

    task->kernel_stack     = reinterpret_cast<u64>(stack);
    task->kernel_stack_top = task->kernel_stack + kStackSize;
    task->pid        = g_next_pid++;
    task->parent_pid = parent_pid;
    task->state      = State::Ready;
    task->is_user    = true;
    task->space      = space;
    task->name       = name;

    // The first switch to this task returns into user_return, which restores
    // the TrapFrame and drops to ring 3.
    fabricate(task, reinterpret_cast<u64>(&user_return), &frame, sizeof(frame));
    return task->pid;
}

void start_preemption() { g_preemption = true; }

void yield()
{
    cpu::InterruptGuard guard;
    switch_to(pick_next());
}

u32 fork_current(const TrapFrame& parent_user)
{
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
    return child;
}

void exit_current(i32 code)
{
    cpu::cli();

    Task* self = current();
    self->exit_code = code;

    // Drop the user address space now; the kernel stack (in the shared heap)
    // stays until a parent reaps it. Switch off the space before freeing it.
    if (self->is_user && self->space != 0) {
        vmm::switch_address_space(vmm::kernel_space());
        vmm::destroy_address_space(self->space);
        self->space = 0;
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

i64 wait_child(i32* status)
{
    for (;;) {
        cpu::InterruptGuard guard;

        bool any_child = false;
        for (u32 i = 0; i < g_task_count; ++i) {
            Task& t = g_tasks[i];
            if (t.parent_pid != current()->pid || t.state == State::Unused)
                continue;
            any_child = true;

            if (t.state == State::Zombie) {
                const i64 pid = t.pid;
                if (status != nullptr)
                    *status = t.exit_code;
                kfree(reinterpret_cast<void*>(t.kernel_stack));
                t.state = State::Unused;
                return pid;
            }
        }

        if (!any_child)
            return -1;

        // A child is still running; block until one exits, then look again.
        current()->state = State::Blocked;
        switch_to(pick_next());
    }
}

u32 current_pid() { return current()->pid; }

vmm::AddressSpace current_task_space() { return current()->space; }

void current_task_set_space(vmm::AddressSpace space) { current()->space = space; }

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
    g_need_resched = false;
    switch_to(pick_next());
}

} // namespace scheduler
