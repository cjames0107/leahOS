#pragma once

#include <leah/types.hpp>

// A round-robin, preemptive scheduler for kernel threads.
//
// Every thread runs in the shared kernel address space for now; separate
// per-process address spaces arrive with fork/exec. What this layer owns is the
// context switch, the ready queue, and turning a timer tick into a preemption.

namespace scheduler {

using Entry = void (*)(void* arg);

// Registers the currently executing code as the first thread ("main"), so there
// is always something to switch away from and back to.
void init();

// Creates a runnable kernel thread. Returns its id, or 0 on failure.
u32 spawn(const char* name, Entry entry, void* arg);

// Arms timer-driven preemption. Until this is called, threads only switch at an
// explicit yield().
void start_preemption();

// Give up the CPU voluntarily. Returns once this thread is scheduled again.
void yield();

// Ends the calling thread. Does not return.
[[noreturn]] void exit_current();

u32 current_id();
u32 alive_count();

// --- called from interrupt context -----------------------------------------

// From the timer IRQ: charges the running thread's quantum and, when it is
// spent, flags that a reschedule is due. Does not switch here - that waits for
// a safe point after the interrupt is acknowledged.
void on_tick();

// From the IRQ path, after the PIC has been acknowledged: performs the switch
// if one is pending. Acknowledging first is what makes switching away safe -
// otherwise the PIC would hold the line and starve every other thread.
void on_irq_return();

} // namespace scheduler
