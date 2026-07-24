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
