#pragma once

#include <leah/file.hpp>
#include <leah/types.hpp>
#include <leah/vmm.hpp>

// A round-robin, preemptive scheduler over tasks. A task is either a kernel
// thread (runs a C function in the kernel's address space) or a user process
// (runs in ring 3 in its own address space). The scheduler owns the context
// switch, the ready queue, and turning a timer tick into a preemption; on a
// switch it also loads the incoming task's page table and kernel stack.

namespace scheduler {

using Entry = void (*)(void* arg);

// The full user register state, laid out to be restored by user_return in
// user_entry.asm and by an IRETQ. The 15 general registers come first (reverse
// push order), then the frame IRETQ itself consumes. Do not reorder without
// editing user_entry.asm in lockstep.
struct [[gnu::packed]] TrapFrame {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 rip, cs, rflags, rsp, ss;
};

static_assert(sizeof(TrapFrame) == 20 * 8);

void init();

// Create the idle task - a kernel thread that halts when nothing else is
// runnable. Call once, after the heap is up, before any task can block.
void start_idle();

// A kernel thread in the kernel's address space. Returns its pid, or 0.
u32 spawn(const char* name, Entry entry, void* arg);

// --- blocking ---------------------------------------------------------------
//
// A channel is an opaque tag a task sleeps on and is woken from. The keyboard
// has a fixed one; a pipe uses its own address so each has a distinct channel.
constexpr u64 kKeyboardChannel = 1;

// Sleep the calling task until wake(channel) is called. Must be entered with
// interrupts disabled (it is only called from syscalls, which mask them), so
// the check-then-block that precedes it cannot race a wake from an interrupt.
void block_on(u64 channel);

// Make every task sleeping on `channel` runnable. Safe to call from an IRQ.
void wake(u64 channel);

// A user process: its own address space, entered in ring 3 with the given
// register state the first time it is scheduled. Returns its pid, or 0.
u32 spawn_user(const char* name, vmm::AddressSpace space,
               const TrapFrame& frame, u32 parent_pid);

void start_preemption();
void yield();

// Turn time-slicing off and back on. A kernel routine that must poll a device to
// completion - draining the NIC while waiting for a network reply - uses this so
// a timer tick cannot schedule it away mid-wait, which would leave nothing
// servicing the device. Interrupts stay enabled; only the task switch is held
// off. Returns the previous state.
bool set_preemption(bool enabled);

// RAII form: disables preemption for the current scope, restoring what it found.
struct NoPreemption {
    bool previous;
    NoPreemption() : previous(set_preemption(false)) {}
    ~NoPreemption() { set_preemption(previous); }
};

// Ends the calling task with an exit code. Does not return. A user task's
// address space is torn down here; its record lingers as a zombie until the
// parent reaps it with wait().
[[noreturn]] void exit_current(i32 code);

// Reap a finished child. Returns the child's pid and writes its exit code
// through `status` (if non-null), or -1 when the caller has no children.
// Blocks until a child exits if one is still running.
i64 wait_child(i32* status);

// --- fork/exec support ------------------------------------------------------

// Duplicate the current task into a new process. The child gets a copy of the
// address space and resumes in ring 3 exactly where the parent's syscall
// returns, but with rax = 0. Returns the child pid to the parent, or 0 on
// failure. `parent_user` is the parent's user register state at the syscall.
u32 fork_current(const TrapFrame& parent_user);

u32 current_pid();
u32 alive_count();

// The running task's address space, and a setter execve uses to swap in a
// freshly loaded image's space (the caller frees the old one).
vmm::AddressSpace current_task_space();
void current_task_set_space(vmm::AddressSpace space);

// The running task's open-file table.
files::Table& current_files();

// The running task's sbrk program break.
u64  current_brk();
void set_current_brk(u64 brk);

// --- called from interrupt context -----------------------------------------

void on_tick();
void on_irq_return();

} // namespace scheduler
