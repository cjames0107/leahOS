#ifndef _SYS_SYSCALL_H
#define _SYS_SYSCALL_H

/* leahOS system call numbers. These are the kernel's own numbering, mirrored
 * from kernel/include/leah/syscall.hpp - the two must be changed together. */

#define SYS_exit     0
#define SYS_write    1
#define SYS_read     2
#define SYS_getpid   3
#define SYS_fork     4
#define SYS_execve   5
#define SYS_wait     6
#define SYS_yield    7
#define SYS_open     8
#define SYS_close    9
#define SYS_lseek    10
#define SYS_stat     11
#define SYS_getdents 12
#define SYS_chdir    13
#define SYS_getcwd   14
#define SYS_mkdir    15
#define SYS_unlink   16

/* The raw entry point. Arguments go in the SysV syscall registers
 * (rdi, rsi, rdx, r10, r8, r9) and the result comes back in rax. Unused
 * argument slots may be passed as 0. */
long __syscall(long number, long a1, long a2, long a3, long a4, long a5);

#endif /* _SYS_SYSCALL_H */
