#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

int open(const char* path, int flags)
{
    return (int)__syscall(SYS_open, (long)path, flags, 0, 0, 0);
}

int close(int fd)
{
    return (int)__syscall(SYS_close, fd, 0, 0, 0, 0);
}

long lseek(int fd, long offset, int whence)
{
    return __syscall(SYS_lseek, fd, offset, whence, 0, 0);
}

int stat(const char* path, struct stat* out)
{
    return (int)__syscall(SYS_stat, (long)path, (long)out, 0, 0, 0);
}

int getdents(const char* path, struct dirent* buffer, int max)
{
    return (int)__syscall(SYS_getdents, (long)path, (long)buffer, max, 0, 0);
}

int chdir(const char* path)
{
    return (int)__syscall(SYS_chdir, (long)path, 0, 0, 0, 0);
}

int getcwd(char* buffer, size_t size)
{
    return (int)__syscall(SYS_getcwd, (long)buffer, (long)size, 0, 0, 0);
}

int mkdir(const char* path)
{
    return (int)__syscall(SYS_mkdir, (long)path, 0, 0, 0, 0);
}

int unlink(const char* path)
{
    return (int)__syscall(SYS_unlink, (long)path, 0, 0, 0, 0);
}

int pipe(int fds[2])
{
    return (int)__syscall(SYS_pipe, (long)fds, 0, 0, 0, 0);
}

int dup2(int oldfd, int newfd)
{
    return (int)__syscall(SYS_dup2, oldfd, newfd, 0, 0, 0);
}

void* sbrk(long increment)
{
    return (void*)__syscall(SYS_sbrk, increment, 0, 0, 0, 0);
}

int rename(const char* oldpath, const char* newpath)
{
    return (int)__syscall(SYS_rename, (long)oldpath, (long)newpath, 0, 0, 0);
}
