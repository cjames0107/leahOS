/* What is running.
 *
 * The kernel already had the snapshot - taskman has been drawing it in a window
 * for a while - and what was missing was the plain-text half, which is the one
 * you can pipe into grep.
 *
 * Threads are tasks here, so they show up as tasks. A process is a task whose
 * pid and tgid are the same; everything else is one of its threads, and `ps`
 * with no arguments hides them because a person asking what is running means
 * programs. -T says otherwise.
 */

#include <proc.h>
#include <cli.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MAX_TASKS 256

static const char* state_name(unsigned state)
{
    switch (state) {
    case PROC_READY:   return "ready";
    case PROC_RUNNING: return "run";
    case PROC_BLOCKED: return "wait";
    case PROC_STOPPED: return "stop";
    case PROC_ZOMBIE:  return "zombie";
    case PROC_DEAD:    return "dead";
    default:           return "?";
    }
}

int main(int argc, char** argv)
{
    cli_begin(argc, argv, "[-T threads] [-a all users] [-l jobs]", "Tale");
    struct proc_info tasks[MAX_TASKS];
    const int threads  = cli_flag("-T");     /* threads too, not just processes */
    const int wide     = cli_flag("-l");     /* the job and login columns */
    /* -e is what every other ps calls -a. */
    const int everyone = cli_flag("-a") || cli_flag("-e");

    const int n = proc_list(tasks, MAX_TASKS);
    if (n < 0) {
        cli_fail("cannot read the task list");
        return 1;
    }

    const unsigned me = (unsigned)getuid();

    if (wide)
        printf("  PID  PGID   SID   PPID UID STATE   TICKS  KIB NAME\n");
    else
        printf("  PID   PPID UID STATE   TICKS  KIB NAME\n");

    for (int i = 0; i < n; ++i) {
        const struct proc_info* t = &tasks[i];
        if (!threads && t->pid != t->tgid)
            continue;
        if (!everyone && t->uid != me)
            continue;

        if (wide)
            printf("%5u %5u %5u  %5u %3u %-6s %6lu %4lu %s\n",
                   t->pid, t->pgid, t->sid, t->parent, t->uid,
                   state_name(t->state), (unsigned long)t->ticks,
                   (unsigned long)(t->bytes / 1024), t->name);
        else
            printf("%5u  %5u %3u %-6s %6lu %4lu %s\n",
                   t->pid, t->parent, t->uid, state_name(t->state),
                   (unsigned long)t->ticks,
                   (unsigned long)(t->bytes / 1024), t->name);
    }
    return 0;
}
