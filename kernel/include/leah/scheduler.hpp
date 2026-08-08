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
/* One channel for "something a poller might care about has changed".
 *
 * poll waits on several descriptors at once, and block_on waits on one. Rather
 * than giving a task a list of channels - which means a wait queue per channel
 * and a task on several of them at once - every pipe that gains data, frees
 * space or loses its last writer wakes this one, and the poller re-checks.
 *
 * Spurious wakeups are the cost, and they are free: a poller's whole job is to
 * look again. */
constexpr u64 kPollChannel = 2;

// Sleep the calling task until wake(channel) is called. Must be entered with
// interrupts disabled (it is only called from syscalls, which mask them), so
// the check-then-block that precedes it cannot race a wake from an interrupt.
void block_on(u64 channel);

// Block on a channel, but not for longer than `ticks`. Either a wake or the
// timer releases it - the tick handler already clears wait_channel when it
// wakes a sleeper, so the two compose without either knowing about the other.
// `ticks` of 0 means no deadline, which is block_on again.
void block_on_until(u64 channel, u64 ticks);

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

// --- driver privileges ------------------------------------------------------
//
// What separates a driver from an ordinary program. Not a ring: on x86-64 the
// page tables have one privilege bit, so anything below ring 3 is supervisor
// and can read all of kernel memory - a ring number would name the privilege
// without enforcing it. These are enforced, one by the hardware's I/O bitmap
// and the rest by the page tables.

// Let the calling task use `count` ports from `base`. Returns 0, or -1 if it
// may not - only root may ask, and a task keeps only what it has asked for, so
// a sound driver that asked for the mixer cannot reach the disk.
i64 grant_io_ports(u16 base, u32 count);
u32  current_gid();
bool set_current_uid(u32 uid);
bool set_current_gid(u32 gid);

// Set both unconditionally, bypassing the rule that only root may change them.
// The one legitimate caller is the authentication path, which has just proved
// the caller knows the account's password - that check *is* the authorisation,
// and applying the ordinary rule on top would make a correct password fail.
// The identity of another process, for a server that has to decide what its
// caller may do. Not privileged: a uid is not a secret, and the alternative is
// servers believing whatever uid arrives in the message. The gid is in the
// high half.
u64 credentials_of(u32 pid);

// Set a process's identity outright, skipping the "only root may change
// credentials" rule that would refuse every successful login by a non-root
// user. Root only, which means authd: the password check that authorises this
// lives there now, along with the file it has to read to make it.
bool set_credentials_of(u32 pid, u32 uid, u32 gid);

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

// The same, to every process of a process group. Returns how many it reached,
// or -1 if that was none. This is what a terminal does with Ctrl-C: the unit a
// person means by "the thing I am running" is the group, because a pipeline is
// several processes and interrupting one of them is not interrupting it.
int  signal_send_group(u32 pgid, int signo);

// Suspend the calling task until somebody sends it SIGCONT, and tell its parent
// it has. Nothing is released - the address space, the files and the kernel
// stack all stay - because the task is expected to carry on afterwards.
void stop_current(int signo);

// --- process groups and sessions --------------------------------------------
//
// A process group is a job: everything in `a | b | c` is one group, so one
// Ctrl-C reaches all three and one wait covers all three. A session is a login,
// a set of groups sharing one terminal. Both are inherited across fork and kept
// across execve - a shell needs a child to be in the right group before the
// child has run a single instruction of its own.

u32  pgid_of(u32 pid);          // pid 0 means the caller
u32  sid_of(u32 pid);
bool set_pgid(u32 pid, u32 pgid);   // both 0-defaulting, as setpgid does
u32  set_sid();                     // 0 if the caller already leads a group

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

// End every thread of the calling process, then the caller. This is what a
// program returning from main means: a thread still blocked in a syscall would
// otherwise keep the process alive with nobody left to finish it, and nothing
// could ever reap it.
[[noreturn]] void exit_group(i32 code);

// What wait_child may return instead of a pid. Distinct values rather than one
// -1, because "you have no children" and "a signal arrived" want completely
// different things from the caller and it used to be unable to tell.
constexpr i64 kWaitNoChildren  = -1;
constexpr i64 kWaitInterrupted = -2;

// Options, matching waitpid's.
constexpr u32 kWaitNoHang    = 1;   // return 0 rather than block
constexpr u32 kWaitUntraced  = 2;   // report children that stopped
constexpr u32 kWaitContinued = 4;   // report children that were continued

// Reap a finished child, or hear about one that stopped or was continued.
//
// `which` selects: a pid, -1 for any child, 0 for any in the caller's process
// group, or -pgid for any in that one. Writes the status word from
// <leah/signal.hpp> through `status`, and returns the child's pid, 0 when
// kWaitNoHang found nothing ready, or one of the two constants above.
i64 wait_child(i64 which, i32* status, u32 options);

// --- fork/exec support ------------------------------------------------------

// Duplicate the current task into a new process. The child gets a copy of the
// address space and resumes in ring 3 exactly where the parent's syscall
// returns, but with rax = 0. Returns the child pid to the parent, or 0 on
// failure. `parent_user` is the parent's user register state at the syscall.
u32 fork_current(const TrapFrame& parent_user);

u32 current_pid();

// For a fault report: which program was running, and whether it was in ring 3.
const char* current_name();

// Set by execve, which is the only thing that knows what the task has become.
// The task keeps its own copy - a borrowed pointer into the image's argument
// storage stops meaning anything the moment the image is replaced.
void set_current_name(const char* name);

// The kernel stack the running task is supposed to be using. For a panic to be
// able to say whether the stack pointer it faulted with is even its own.
void current_stack_bounds(u64* base, u64* top);

// Which task's kernel stack an address falls inside, or nullptr. A panic uses
// it to say whose stack a runaway pointer is actually standing on.
const char* stack_owner(u64 address, u32* pid_out);
bool current_is_user();
u32 alive_count();

// A snapshot of the task table, for a task manager to show. Copied out under
// the kernel lock rather than handed out by reference: the table is live, and a
// reader walking it while a task exits would see a slot change underneath it.
struct TaskInfo {
    u32 pid;
    u32 tgid;
    u32 parent;
    u32 uid;
    u32 state;          // matches State, with 0 meaning an unused slot
    u32 is_user;
    u32 pgid;           // its job, and
    u32 sid;            // its login - what ps prints beside the two above
    u64 ticks;          // scheduler slices this task has been given
    u64 bytes;          // resident user memory
    char name[32];
};

// Fills `out` with up to `max` entries and returns how many. Includes every
// live task, threads as well as processes - a thread is a task here.
u32 snapshot(TaskInfo* out, u32 max);

// How many tasks wanted to run, averaged over one, five and fifteen minutes,
// in hundredths - so 150 is a load of 1.5. Sampled every five seconds and
// decayed towards each sample, which is what makes it an average rather than
// an instantaneous count that flickers.
void load_average(u64 out[3]);

// Per-processor slice counts, for a monitor that wants to show more than one
// bar. `busy` counts slices given to real work and `idle` those given to that
// CPU's idle task; the difference between them is the only notion of "load"
// this system has, and it is a share rather than a duty cycle.
struct CpuStat {
    u64 busy;
    u64 idle;
};
u32 cpu_stats(CpuStat* out, u32 max);

// The running task's address space, and a setter execve uses to swap in a
// freshly loaded image's space (the caller frees the old one).
vmm::AddressSpace current_task_space();
void current_task_set_space(vmm::AddressSpace space);

// The running task's open-file table.
files::Table& current_files();

// The running task's sbrk program break.
u64  current_brk();
void set_current_brk(u64 brk);

/* Give the running task a freshly initialised floating-point unit, in its
 * saved state *and* in the registers themselves. execve wants both: the task
 * is not switched away before it enters the new image, so clearing only the
 * saved copy would leave the previous program's registers live. */
void reset_current_fpu();

// The running task's next free mmap address.
u64  current_mmap_next();
void set_current_mmap_next(u64 next);

// --- called from interrupt context -----------------------------------------

void on_tick();
void on_irq_return();

} // namespace scheduler
