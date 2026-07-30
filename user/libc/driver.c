#include <driver.h>
#include <sys/syscall.h>

int io_permit(unsigned base, unsigned count)
{
    return (int)__syscall(SYS_iopermit, (long)base, (long)count, 0, 0, 0);
}

volatile void* map_physical(uint64_t physical, unsigned long bytes)
{
    const long r = __syscall(SYS_mapphysical, (long)physical, (long)bytes, 0, 0, 0);
    return r < 0 ? 0 : (volatile void*)r;
}

void* dma_alloc(unsigned long bytes, uint64_t* physical_out)
{
    const long r = __syscall(SYS_dmaalloc, (long)bytes, (long)physical_out, 0, 0, 0);
    return r < 0 ? 0 : (void*)r;
}

int irq_listen(unsigned irq)
{
    return (int)__syscall(SYS_irqlisten, (long)irq, 0, 0, 0, 0);
}

long irq_wait(unsigned irq)
{
    return __syscall(SYS_irqwait, (long)irq, 0, 0, 0, 0);
}
