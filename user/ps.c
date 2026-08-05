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
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MAX_TASKS 128

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
    struct proc_info tasks[MAX_TASKS];
    int threads = 0, everyone = 0, wide = 0;

    for (int i = 1; i < argc; ++i) {
        const char* flag = argv[i];
        if (flag[0] != '-') continue;
        for (int c = 1; flag[c] != '\0'; ++c) {
            switch (flag[c]) {
            case 'T': threads  = 1; break;   /* threads too, not just processes */
            case 'a': everyone = 1; break;   /* other people's as well as mine */
            case 'l': wide     = 1; break;   /* the job and login columns */
            case 'e': everyone = 1; break;   /* what every other ps calls it */
            default:
                printf("usage: ps [-T threads] [-a all users] [-l jobs]\n");
                return 1;
            }
        }
    }

    const int n = proc_list(tasks, MAX_TASKS);
    if (n < 0) {
        printf("ps: cannot read the task list\n");
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
