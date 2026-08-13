#ifndef _PROC_H
#define _PROC_H

#include <stdint.h>

/* Looking at what the system is doing.
 *
 * A snapshot rather than a live view: the kernel copies the table out under its
 * own lock, because a reader walking a live table would see slots change as
 * tasks come and go. Threads appear as tasks, which is what they are here - a
 * thread shares its group's pid in `tgid` and has its own in `pid`. */

#define PROC_UNUSED  0
#define PROC_READY   1
#define PROC_RUNNING 2
#define PROC_BLOCKED 3
#define PROC_STOPPED 4      /* suspended, waiting for SIGCONT */
#define PROC_ZOMBIE  5
#define PROC_DEAD    6

struct proc_info {
    uint32_t pid;
    uint32_t tgid;
    uint32_t parent;
    uint32_t uid;
    uint32_t state;
    uint32_t is_user;
    uint32_t pgid;          /* its job */
    uint32_t sid;           /* its login */
    uint64_t ticks;         /* scheduler slices given to it */
    uint64_t bytes;         /* user memory it has asked for */
    char     name[32];
};

/* Returns how many were filled in, or -1. */
int proc_list(struct proc_info* out, int max);

struct mem_info {
    uint64_t usable;
    uint64_t used;
    uint64_t free;
};

int mem_info(struct mem_info* out);

/* Per-processor slice counts. Returns how many processors were reported. */
struct cpu_stat {
    uint64_t busy;
    uint64_t idle;
};
int cpu_info(struct cpu_stat* out, int max);

/* How many tasks wanted to run, over one, five and fifteen minutes, in
 * hundredths - so 150 means a load of 1.5. Integers because the kernel that
 * keeps them has no floating point; the caller can divide. */
int load_average(unsigned long out[3]);

/* The kernel's own messages, read back from the ring it keeps.
 *
 * `from` is a byte position since boot, kept by the caller: start at 0 for the
 * oldest still held, and pass back what this leaves to get only what is new.
 * A reader that falls further behind than the ring is long is fast-forwarded,
 * so this can never return bytes that have been overwritten. Returns how many
 * bytes were copied, which is 0 when nothing has been said since last time. */
unsigned long klog_read(unsigned long long* from, char* out,
                        unsigned long max);

#endif /* _PROC_H */
