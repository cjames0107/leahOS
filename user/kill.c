/* Send a signal.
 *
 * Named after the one thing it usually does, as it has been everywhere since
 * the seventies. A negative pid is a process group - `kill -TERM -42` ends
 * every process of job 42 - which is the form that matters here, because a
 * pipeline is several processes and ending one of them is not ending it.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct { const char* name; int signo; } kNames[] = {
    { "HUP",  SIGHUP  }, { "INT",  SIGINT  }, { "QUIT", SIGQUIT },
    { "KILL", SIGKILL }, { "USR1", SIGUSR1 }, { "SEGV", SIGSEGV },
    { "USR2", SIGUSR2 }, { "PIPE", SIGPIPE }, { "TERM", SIGTERM },
    { "CHLD", SIGCHLD }, { "CONT", SIGCONT }, { "STOP", SIGSTOP },
    { "TSTP", SIGTSTP }, { "TTIN", SIGTTIN }, { "TTOU", SIGTTOU },
};

static int upper(int c) { return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c; }

/* "-9", "-TERM", "-term" and "-SIGTERM" all mean the same thing, because all
 * four are what people type. */
static int signal_of(const char* text)
{
    char name[16];
    unsigned n = 0;

    if (text[0] >= '0' && text[0] <= '9')
        return atoi_simple(text);

    while (text[n] != '\0' && n + 1 < sizeof(name)) {
        name[n] = (char)upper((unsigned char)text[n]);
        ++n;
    }
    name[n] = '\0';
    const char* bare = strncmp(name, "SIG", 3) == 0 ? name + 3 : name;

    for (unsigned i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i)
        if (strcmp(kNames[i].name, bare) == 0)
            return kNames[i].signo;
    return -1;
}

static void list_signals(void)
{
    for (unsigned i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i)
        printf("%2d %-5s %s\n", kNames[i].signo, kNames[i].name,
               signal_name(kNames[i].signo));
}

int main(int argc, char** argv)
{
    int signo = SIGTERM;
    int at = 1;

    if (argc > 1 && strcmp(argv[1], "-l") == 0) {
        list_signals();
        return 0;
    }

    /* A leading -SOMETHING is the signal, unless what follows the dash is a
     * number that turns out to be the only argument - `kill -42` on its own is
     * ambiguous everywhere, and everywhere resolves it as the signal. */
    if (argc > 2 && argv[1][0] == '-' && argv[1][1] != '\0') {
        signo = signal_of(argv[1] + 1);
        if (signo < 0) {
            fprintf(stderr, "kill: %s: no such signal\n", argv[1] + 1);
            return 1;
        }
        at = 2;
    }

    if (at >= argc) {
        printf("usage: kill [-SIGNAL] pid...   (a negative pid is a job)\n");
        printf("       kill -l\n");
        return 1;
    }

    int failed = 0;
    for (; at < argc; ++at) {
        const int pid = atoi_simple(argv[at]);
        if (kill(pid, signo) != 0) {
            fprintf(stderr, "kill: %s: no such process\n", argv[at]);
            failed = 1;
        }
    }
    return failed;
}
