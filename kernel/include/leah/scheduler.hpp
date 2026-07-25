#pragma once

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

// A kernel thread in the kernel's address space. Returns its pid, or 0.
u32 spawn(const char* name, Entry entry, void* arg);

// A user process: its own address space, entered in ring 3 with the given
// register state the first time it is scheduled. Returns its pid, or 0.
u32 spawn_user(const char* name, vmm::AddressSpace space,
               const TrapFrame& frame, u32 parent_pid);

void start_preemption();
void yield();

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

// --- called from interrupt context -----------------------------------------

void on_tick();
void on_irq_return();

} // namespace scheduler
