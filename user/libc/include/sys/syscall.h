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
#define SYS_pipe     17
#define SYS_dup2     18
#define SYS_sbrk     19
#define SYS_rename   20
#define SYS_netinfo  21
#define SYS_ping     22
#define SYS_arp      23
#define SYS_resolve  24
#define SYS_mmap     25
#define SYS_munmap   26
#define SYS_clone    27
#define SYS_gettid   28
#define SYS_kill     29
#define SYS_signal   30
#define SYS_sigreturn 31
#define SYS_getuid   32
#define SYS_setuid   33
#define SYS_getgid   34
#define SYS_setgid   35
#define SYS_chmod    36
#define SYS_chown    37
#define SYS_futex    38
#define SYS_connect  39
#define SYS_login    42
#define SYS_setecho  43
#define SYS_username 44
#define SYS_useradd  45
#define SYS_passwd   46
#define SYS_shmopen    47
#define SYS_shmmap     48
#define SYS_shmsize    49
#define SYS_fbinfo     50
#define SYS_fbmap      51
#define SYS_inputpoll  52
#define SYS_fbfont     53
#define SYS_sleep      54
#define SYS_shmdestroy 55
#define SYS_threadexit 56
#define SYS_proclist   57
#define SYS_meminfo    58
#define SYS_cpuinfo    59
#define SYS_audioplay   60
#define SYS_audiospace  61
#define SYS_audiovol    62
#define SYS_audiostop   63
#define SYS_audioflush  64
#define SYS_audioinfo   65
#define SYS_portcreate  66
#define SYS_portopen    67
#define SYS_portdestroy 68
#define SYS_ipccall     69
#define SYS_ipcrecv     70
#define SYS_ipcreply    71

/* shm_open's flags live in <shm.h> with the call they belong to. */

/* The raw entry point. Arguments go in the SysV syscall registers
 * (rdi, rsi, rdx, r10, r8, r9) and the result comes back in rax. Unused
 * argument slots may be passed as 0. */
long __syscall(long number, long a1, long a2, long a3, long a4, long a5);

#endif /* _SYS_SYSCALL_H */
