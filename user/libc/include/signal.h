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
#define SIGPIPE  13     /* wrote to a pipe nobody is reading */
#define SIGTERM  15
#define SIGCHLD  17
/* Job control. Two to stop a program, one to start it again, and two more for a
 * background job reaching for a terminal that is not currently its. */
#define SIGCONT  18
#define SIGSTOP  19     /* never catchable, like SIGKILL */
#define SIGTSTP  20     /* what the keyboard sends, and catchable */
#define SIGTTIN  21
#define SIGTTOU  22

typedef void (*sighandler_t)(int);

#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)

/* Set the disposition for `signo` and return the previous one. Dispositions are
 * process-wide, survive fork, and are reset to default by execve. */
sighandler_t signal(int signo, sighandler_t handler);

/* Send `signo` to the process with this pid - or, if `pid` is negative, to
 * every process of the group named by its negation, which is what a terminal
 * does with Ctrl-C and a shell with `kill %1`. A pid of 0 means the caller's
 * own group. Signal 0 delivers nothing and answers "is it still there?".
 * Returns 0, or -1. */
int kill(int pid, int signo);

/* The name of a signal, without the SIG - "int", "term", "tstp". For a shell
 * saying why a job ended, which is the only thing that ever wants it. */
const char* signal_name(int signo);

/* Send `signo` to the caller. */
int raise(int signo);

#endif /* _SIGNAL_H */
