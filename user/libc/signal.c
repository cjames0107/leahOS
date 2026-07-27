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
