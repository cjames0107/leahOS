#include <proc.h>
#include <sys/syscall.h>

int proc_list(struct proc_info* out, int max)
{
    return (int)__syscall(SYS_proclist, (long)out, (long)max, 0, 0, 0);
}

int mem_info(struct mem_info* out)
{
    return (int)__syscall(SYS_meminfo, (long)out, 0, 0, 0, 0);
}

int cpu_info(struct cpu_stat* out, int max)
{
    return (int)__syscall(SYS_cpuinfo, (long)out, (long)max, 0, 0, 0);
}
