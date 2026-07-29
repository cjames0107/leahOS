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
#define PROC_ZOMBIE  4
#define PROC_DEAD    5

struct proc_info {
    uint32_t pid;
    uint32_t tgid;
    uint32_t parent;
    uint32_t uid;
    uint32_t state;
    uint32_t is_user;
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

#endif /* _PROC_H */
