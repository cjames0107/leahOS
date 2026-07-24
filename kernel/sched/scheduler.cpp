#include <leah/cpu.hpp>
#include <leah/heap.hpp>
#include <leah/panic.hpp>
#include <leah/scheduler.hpp>
#include <leah/string.hpp>

// Implemented in context.asm.
extern "C" void context_switch(u64* save_rsp, u64 load_rsp);

namespace scheduler {
namespace {

constexpr usize kMaxThreads   = 16;
constexpr usize kStackSize    = 16 * 1024;
constexpr u32   kQuantumTicks = 1;          // 10 ms at the PIT's 100 Hz

enum class State : u8 {
    Unused,
    Ready,
    Running,
    Dead,
};

struct Thread {
    u64   kernel_rsp;       // parked stack pointer; the rest of the state is on it
    u64   stack_base;       // for a future reaper to free
    u32   id;
    State state;
    const char* name;
    Entry entry;
    void* arg;
};

Thread g_threads[kMaxThreads];
u32    g_thread_count = 0;
u32    g_current = 0;                 // index into g_threads

bool   g_preemption = false;
u32    g_quantum = kQuantumTicks;
volatile bool g_need_resched = false;

Thread* current() { return &g_threads[g_current]; }

// The first thing a brand-new thread executes. context_switch "returns" here
// off the fabricated stack. It runs the thread body and, if that ever returns,
// exits cleanly rather than falling off the end of the stack.
[[noreturn]] void trampoline()
{
    // The switch that got us here ran with interrupts off; a thread must run
    // with them on, or it could never be preempted.
    cpu::sti();

    Thread* self = current();
    self->entry(self->arg);

    exit_current();
}

u32 pick_next()
{
    // Round robin: the first Ready thread after the current one, wrapping.
    for (u32 step = 1; step <= g_thread_count; ++step) {
        const u32 index = (g_current + step) % g_thread_count;
        if (g_threads[index].state == State::Ready)
            return index;
    }
    // Nothing else is runnable. Stay put if we still can, otherwise there is
    // genuinely nothing to run.
    if (current()->state == State::Running || current()->state == State::Ready)
        return g_current;
    panic("scheduler: no runnable thread");
}

// Switch to another thread. Must be entered with interrupts disabled so the
// choice of next thread cannot change underneath the switch.
void switch_to(u32 next_index)
{
    if (next_index == g_current)
        return;

    Thread* prev = current();
    Thread* next = &g_threads[next_index];

    if (prev->state == State::Running)
        prev->state = State::Ready;
    next->state = State::Running;
    g_current = next_index;
    g_quantum = kQuantumTicks;

    context_switch(&prev->kernel_rsp, next->kernel_rsp);
    // Execution resumes here only when prev is scheduled again.
}

} // namespace

void init()
{
    memset(g_threads, 0, sizeof(g_threads));

    // Thread 0 is whatever called us. It has no fabricated stack because it is
    // already running on a real one; context_switch will fill in its kernel_rsp
    // the first time we switch away from it.
    Thread& main_thread = g_threads[0];
    main_thread.id    = 1;
    main_thread.state = State::Running;
    main_thread.name  = "main";
    g_thread_count = 1;
    g_current = 0;
}

u32 spawn(const char* name, Entry entry, void* arg)
{
    cpu::InterruptGuard guard;

    if (g_thread_count >= kMaxThreads)
        return 0;

    Thread& thread = g_threads[g_thread_count];

    auto* stack = static_cast<u8*>(kmalloc(kStackSize));
    if (stack == nullptr)
        return 0;

    thread.stack_base = reinterpret_cast<u64>(stack);
    thread.id    = g_thread_count + 1;
    thread.state = State::Ready;
    thread.name  = name;
    thread.entry = entry;
    thread.arg   = arg;

    // Fabricate a stack that context_switch can "return" from into trampoline.
    // The pad keeps the trampoline's entry RSP at 16-byte-aligned-plus-8, the
    // alignment the SysV ABI promises a function on entry - without it the
    // first call the thread makes would be misaligned.
    u64* sp = reinterpret_cast<u64*>(stack + kStackSize);
    *--sp = 0;                                        // alignment pad
    *--sp = reinterpret_cast<u64>(&trampoline);       // context_switch's RET target
    *--sp = 0;                                        // r15
    *--sp = 0;                                        // r14
    *--sp = 0;                                        // r13
    *--sp = 0;                                        // r12
    *--sp = 0;                                        // rbp
    *--sp = 0;                                        // rbx
    thread.kernel_rsp = reinterpret_cast<u64>(sp);

    ++g_thread_count;
    return thread.id;
}

void start_preemption()
{
    g_preemption = true;
}

void yield()
{
    cpu::InterruptGuard guard;
    switch_to(pick_next());
}

void exit_current()
{
    cpu::cli();
    current()->state = State::Dead;

    // Note: the stack is not freed here - a thread cannot free the stack it is
    // standing on. Reaping dead threads' stacks lands with the process work;
    // for now a handful of finished threads leak their 16 KiB, which is
    // immaterial against hundreds of megabytes.
    switch_to(pick_next());

    panic("scheduler: a dead thread was scheduled again");
    __builtin_unreachable();
}

u32 current_id() { return current()->id; }

u32 alive_count()
{
    u32 alive = 0;
    for (u32 i = 0; i < g_thread_count; ++i) {
        if (g_threads[i].state == State::Ready || g_threads[i].state == State::Running)
            ++alive;
    }
    return alive;
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

    // We are here with interrupts off (still inside the ISR) and the PIC
    // already acknowledged, so switching away is safe.
    switch_to(pick_next());
}

} // namespace scheduler
