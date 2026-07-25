#include <sys/syscall.h>
#include <unistd.h>

ssize_t write(int fd, const void* buffer, size_t count)
{
    return __syscall(SYS_write, fd, (long)buffer, (long)count, 0, 0);
}

ssize_t read(int fd, void* buffer, size_t count)
{
    return __syscall(SYS_read, fd, (long)buffer, (long)count, 0, 0);
}

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
    /* The kernel only reads the path for now; argv/envp are accepted for a
     * conventional signature but not yet passed to the new program. */
    return (int)__syscall(SYS_execve, (long)path, (long)argv, (long)envp, 0, 0);
}

pid_t wait(int* status)
{
    return (pid_t)__syscall(SYS_wait, 0, (long)status, 0, 0, 0);
}

void yield(void)
{
    __syscall(SYS_yield, 0, 0, 0, 0, 0);
}
