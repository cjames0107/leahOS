#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>

/* The trampoline a signal handler returns through.
 *
 * The kernel cannot put this code on the user stack - the stack is mapped
 * non-executable - so libc supplies it and registers its address with the first
 * signal() call. When the handler returns, it RETs here with the stack pointer
 * sitting exactly on the saved context, which is what sigreturn expects. It must
 * therefore not touch the stack at all, which is why it is written out in
 * assembly rather than as a C function with a prologue. */
__asm__(
    ".globl __sigrestorer\n"
    "__sigrestorer:\n"
    "    movq $31, %rax\n"          /* SYS_sigreturn */
    "    syscall\n"
);
extern void __sigrestorer(void);

sighandler_t signal(int signo, sighandler_t handler)
{
    long previous = __syscall(SYS_signal, signo, (long)handler,
                              (long)__sigrestorer, 0, 0);
    return (sighandler_t)previous;
}

int kill(int pid, int signo)
{
    return (int)__syscall(SYS_kill, pid, signo, 0, 0, 0);
}

int raise(int signo)
{
    return kill(getpid(), signo);
}

/* Short, lowercase and without the SIG, because the one caller is a shell
 * printing "terminated" or "stopped" beside a job and the prefix would be
 * noise there. Unknown numbers come back as "signal", which is true. */
const char* signal_name(int signo)
{
    switch (signo) {
    case SIGHUP:  return "hangup";
    case SIGINT:  return "interrupt";
    case SIGQUIT: return "quit";
    case SIGKILL: return "killed";
    case SIGUSR1: return "user1";
    case SIGSEGV: return "segfault";
    case SIGUSR2: return "user2";
    case SIGPIPE: return "broken pipe";
    case SIGTERM: return "terminated";
    case SIGCHLD: return "child";
    case SIGCONT: return "continued";
    case SIGSTOP: return "stopped";
    case SIGTSTP: return "suspended";
    case SIGTTIN: return "wants input";
    case SIGTTOU: return "wants output";
    default:      return "signal";
    }
}
