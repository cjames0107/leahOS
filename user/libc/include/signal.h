#ifndef _SIGNAL_H
#define _SIGNAL_H

/* Signal numbers, matching kernel/include/leah/signal.hpp (and Linux, so the
 * names mean what they usually mean). */
#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGKILL  9      /* never catchable */
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGTERM  15
#define SIGCHLD  17

typedef void (*sighandler_t)(int);

#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)

/* Set the disposition for `signo` and return the previous one. Dispositions are
 * process-wide, survive fork, and are reset to default by execve. */
sighandler_t signal(int signo, sighandler_t handler);

/* Send `signo` to the process (or thread) with this pid. Returns 0, or -1. */
int kill(int pid, int signo);

/* Send `signo` to the caller. */
int raise(int signo);

#endif /* _SIGNAL_H */
