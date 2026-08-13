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

int load_average(unsigned long out[3])
{
    return (int)__syscall(SYS_loadavg, (long)out, 0, 0, 0, 0);
}

unsigned long klog_read(unsigned long long* from, char* out, unsigned long max)
{
    const long n = __syscall(SYS_klog, (long)from, (long)out, (long)max, 0, 0);
    return n < 0 ? 0 : (unsigned long)n;
}
