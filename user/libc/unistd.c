#include <sys/syscall.h>
#include <unistd.h>

/* read and write live in fs.c now, with the descriptor table they have to
 * consult: which of them is a file and which is the console is a fact about
 * that table, and the table is ours. */

pid_t getpid(void)
{
    return (pid_t)__syscall(SYS_getpid, 0, 0, 0, 0, 0);
}

pid_t fork(void)
{
    return (pid_t)__syscall(SYS_fork, 0, 0, 0, 0, 0);
}

int execve(const char* path, char* const argv[], char* const envp[])
{
    /* Hand the descriptor table and the working directory to whatever comes
     * next. They are ours, in memory that is about to be thrown away, and a
     * shell that redirected a child's output would otherwise be handing that
     * child nothing. */
    __fd_save_for_exec();

    /* Resolve here too. The kernel still opens the image by name, and it is no
     * longer told about the working directory, so a relative path would be
     * resolved against a directory nobody has updated since boot. */
    {
        static char full[256];
        __fd_resolve(path, full);
        path = full;
    }
    /* The kernel only reads the path for now; argv/envp are accepted for a
     * conventional signature but not yet passed to the new program. */
    return (int)__syscall(SYS_execve, (long)path, (long)argv, (long)envp, 0, 0);
}

pid_t wait(int* status)
{
    return (pid_t)__syscall(SYS_wait, 0, (long)status, 0, 0, 0);
}

void msleep(unsigned long ms) { __syscall(SYS_sleep, (long)ms, 0, 0, 0, 0); }

void yield(void)
{
    __syscall(SYS_yield, 0, 0, 0, 0, 0);
}
