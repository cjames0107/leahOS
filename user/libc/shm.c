#include <shm.h>
#include <sys/syscall.h>

int shm_open(unsigned key, unsigned long bytes, unsigned flags)
{
    return (int)__syscall(SYS_shmopen, (long)key, (long)bytes, (long)flags, 0, 0);
}

void* shm_map(int id)
{
    long address = __syscall(SYS_shmmap, id, 0, 0, 0, 0);
    if (address == -1)
        return 0;
    return (void*)address;
}

unsigned long shm_size(int id)
{
    return (unsigned long)__syscall(SYS_shmsize, id, 0, 0, 0, 0);
}

int shm_destroy(int id)
{
    return (int)__syscall(SYS_shmdestroy, id, 0, 0, 0, 0);
}
