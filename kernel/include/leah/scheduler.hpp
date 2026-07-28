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

// An idle task for a specific processor; each CPU needs its own, or two would
// try to run the same one.
void start_idle_for(u32 cpu_slot);

// An application processor's last call: join the scheduler and never return.
// Must be entered holding the kernel lock.
[[noreturn]] void enter_scheduler_on_this_cpu();

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

// Wake at most `limit` sleepers on `channel`; returns how many were woken. This
// is what futex needs: waking one waiter rather than a thundering herd.
u32 wake_n(u64 channel, u32 limit);

// A user process: its own address space, entered in ring 3 with the given
// register state the first time it is scheduled. Returns its pid, or 0.
u32 spawn_user(const char* name, vmm::AddressSpace space,
               const TrapFrame& frame, u32 parent_pid);

// A new thread inside the calling process: it shares the caller's address space
// and open-file table rather than copying them, and enters ring 3 on `frame`.
// The caller is its parent, so wait() joins it. Returns its tid, or 0.
u32 spawn_thread(const TrapFrame& frame);

// The running task's thread id (its pid) and thread-group id (the process id
// every thread of the process shares).
u32 current_tid();
u32 current_tgid();

// --- credentials ------------------------------------------------------------
//
// uid 0 is root and bypasses permission checks. Credentials are inherited
// across fork and execve; only root may change them.
u32  current_uid();
u32  current_gid();
bool set_current_uid(u32 uid);
bool set_current_gid(u32 gid);

// Set both unconditionally, bypassing the rule that only root may change them.
// The one legitimate caller is the authentication path, which has just proved
// the caller knows the account's password - that check *is* the authorisation,
// and applying the ordinary rule on top would make a correct password fail.
void set_credentials(u32 uid, u32 gid);

// --- signals ----------------------------------------------------------------
//
// Delivery happens on the way out of a syscall, which is the one place the
// kernel holds the full user register state and can rewrite it to enter a
// handler. Sending a signal therefore wakes a blocked target so it reaches that
// check; a purely compute-bound loop that never enters the kernel will not see
// a signal until it does.

// Mark `signo` pending on the task with this pid. False if there is no such
// user task.
bool signal_send(u32 pid, int signo);

// Remove and return the lowest pending signal, or 0 if none are pending.
int  signal_take_pending();
bool signal_pending();

// The process-wide disposition for `signo`: 0 default, 1 ignore, else a handler.
u64  signal_handler(int signo);
void signal_set_handler(int signo, u64 handler);

// libc's trampoline, which the handler returns through to call sigreturn.
u64  signal_restorer();
void signal_set_restorer(u64 restorer);

// Reset every disposition to default - what execve does, since the new image
// knows nothing of the old one's handlers.
void signal_reset_all();

void start_preemption();
void yield();

// Block until at least `ticks` timer ticks have passed. The point of it is that
// a task which only ever yields is still runnable, so it keeps re-entering the
// kernel and can starve another CPU out of the lock; a sleeping one is not
// runnable at all.
void sleep_ticks(u64 ticks);

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

// The running task's next free mmap address.
u64  current_mmap_next();
void set_current_mmap_next(u64 next);

// --- called from interrupt context -----------------------------------------

void on_tick();
void on_irq_return();

} // namespace scheduler
