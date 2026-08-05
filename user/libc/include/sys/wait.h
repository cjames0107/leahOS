#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H

#include <unistd.h>

/* What came back out of wait.
 *
 * One word has to carry three different endings. "The exit code" was enough
 * while nothing could be suspended; a shell with job control has to tell a
 * program that finished from one that is sitting there waiting to be told to
 * carry on, and those are not the same news at all.
 *
 *   0x000 | code    ran to the end, or called exit
 *   0x100 | signo   killed by a signal
 *   0x200 | signo   stopped by a signal, and still there
 *   0x300           started again by SIGCONT
 *
 * The layout is this system's own rather than the bits Linux packs a wait
 * status into. Nothing here has to interoperate, and this way an ordinary exit
 * is *just its code* - which is what every caller in this system was already
 * comparing against, and why almost none of them had to change when the other
 * three endings arrived. Mirrored in kernel/include/leah/signal.hpp.
 */
#define WSTATUS_KIND(s)   ((s) & 0x300)
#define WSTATUS_DATA(s)   ((s) & 0x0FF)

#define WIFEXITED(s)      (WSTATUS_KIND(s) == 0x000)
#define WEXITSTATUS(s)    WSTATUS_DATA(s)
#define WIFSIGNALED(s)    (WSTATUS_KIND(s) == 0x100)
#define WTERMSIG(s)       WSTATUS_DATA(s)
#define WIFSTOPPED(s)     (WSTATUS_KIND(s) == 0x200)
#define WSTOPSIG(s)       WSTATUS_DATA(s)
#define WIFCONTINUED(s)   (WSTATUS_KIND(s) == 0x300)

/* What a shell prints as $?. There is no room in one byte for "and it was a
 * signal", so every shell has always used the same lie: 128 plus the number.
 * It is a lie a person can read, which is the only thing it is for. */
#define WSHELL_STATUS(s)  (WIFSIGNALED(s) ? 128 + WTERMSIG(s) : WEXITSTATUS(s))

/* Options. */
#define WNOHANG     1   /* return 0 rather than block */
#define WUNTRACED   2   /* report children that stopped */
#define WCONTINUED  4   /* report children that were continued */

/* Wait for one particular child, or one particular job.
 *
 * `which` is a pid, or -1 for any child, or 0 for any in the caller's process
 * group, or -pgid for any in that one. Returns the pid, 0 when WNOHANG found
 * nothing ready, or -1 - with errno ECHILD when there was no such child and
 * EINTR when a signal arrived first, which are worth telling apart: the first
 * means stop asking and the second means ask again.
 *
 * Without WUNTRACED a stopped child is not mentioned, so a plain wait() blocks
 * straight through a suspension exactly as it always did.
 */
pid_t waitpid(pid_t which, int* status, int options);

#endif /* _SYS_WAIT_H */
