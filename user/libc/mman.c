#include <sys/mman.h>
#include <sys/syscall.h>

void* mmap(void* addr, size_t length, int prot, int flags, int fd, long offset)
{
    (void)fd;
    (void)offset;
    long r = __syscall(SYS_mmap, (long)addr, (long)length, prot, flags, 0);
    if (r == -1)
        return MAP_FAILED;
    return (void*)r;
}

int munmap(void* addr, size_t length)
{
    return (int)__syscall(SYS_munmap, (long)addr, (long)length, 0, 0, 0);
}
