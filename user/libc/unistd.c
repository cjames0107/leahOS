#include <sys/syscall.h>
#include <errno.h>
#include <loader.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* read and write live in fs.c now, with the descriptor table they have to
 * consult: which of them is a file and which is the console is a fact about
 * that table, and the table is ours. */

pid_t getpid(void)
{
    return (pid_t)__syscall(SYS_getpid, 0, 0, 0, 0, 0);
}

pid_t fork(void)
{
    /* Before the second process exists, not after: see __fd_before_fork. */
    __fd_before_fork();
    return (pid_t)__syscall(SYS_fork, 0, 0, 0, 0, 0);
}

/* Read the program, then hand the kernel its bytes.
 *
 * The kernel used to open the path itself, which made it a client of the
 * filesystem server in the middle of building an address space - a blocking
 * call at the worst possible moment, and the last place ring 0 reached into
 * ring 3 for something it needed. Whoever wants to run a program is already a
 * process with a filesystem; it can do its own reading.
 */
/* How deep a script may point at another script before this gives up. Every
 * UNIX has a limit here and they are all small; a chain longer than a handful
 * is a loop somebody wrote by accident. */
#define MAX_SHEBANG 4
#ifndef MAX_ARGS
#define MAX_ARGS 32
#endif

int execve(const char* path, char* const argv[], char* const envp[])
{
    char full[256];
    struct stat st;
    int fd;
    long got;
    int result;

    /* A null environment means "keep this one", not "have none". That is not
     * what POSIX says - there, a null envp is undefined and execv is the call
     * that inherits - but every caller in this system passed 0 meaning "I did
     * not think about it", and an empty environment is never what they wanted.
     * Pass an empty vector to really mean none. */
    if (envp == 0)
        envp = (char* const*)environ;

    __fd_resolve(path, full);
    if (stat(full, &st) != 0)
        return -1;                      /* stat set errno */
    if (st.st_type != S_IFREG) {
        errno = EACCES;
        return -1;
    }
    if (st.st_size == 0) {
        errno = ENOEXEC;
        return -1;
    }

    /* Only the first line, and only to see whether this is a script.
     *
     * The whole file used to be read here. It no longer is: the loader asks
     * the kernel whether it is already holding this program before reading
     * anything, and on a hit reads nothing at all. What has to be read here is
     * the two bytes that decide whether this is a program in the first place.
     */
    char head[256];
    fd = open(full, O_RDONLY);
    if (fd < 0)
        return -1;
    got = read(fd, head, sizeof(head) - 1);
    close(fd);
    if (got <= 0) {
        errno = ENOEXEC;
        return -1;
    }
    head[got] = '\0';

    /* A script names its interpreter on the first line. Handled here rather
     * than in the kernel because this is where a program is turned into
     * something runnable - the kernel is handed segments and does not know
     * what a file format is. */
    if (got > 2 && head[0] == '#' && head[1] == '!') {
        static int depth;
        if (++depth > MAX_SHEBANG) {
            depth = 0;
            errno = ELOOP;
            return -1;
        }
        /* The rest of the line, split into the interpreter and at most one
         * argument - which is what every UNIX does, and why "#!/bin/sh -e"
         * works and "#!/bin/sh -e -u" does not. */
        char line[256];
        long n = 0;
        while (n < got - 2 && n < (long)sizeof(line) - 1 && head[2 + n] != '\n')
            { line[n] = head[2 + n]; ++n; }
        line[n] = '\0';

        char* interp = line;
        while (*interp == ' ' || *interp == '\t')
            ++interp;
        char* extra = interp;
        while (*extra != '\0' && *extra != ' ' && *extra != '\t')
            ++extra;
        if (*extra != '\0') {
            *extra++ = '\0';
            while (*extra == ' ' || *extra == '\t')
                ++extra;
        }
        if (interp[0] == '\0') {
            depth = 0;
            errno = ENOEXEC;
            return -1;
        }

        /* argv becomes: interpreter, [its argument], the script, then
         * whatever the caller passed after argv[0]. The script's own path
         * goes in as it was named, so $0 reads as the script. */
        char* rebuilt[MAX_ARGS + 3];
        int at = 0;
        rebuilt[at++] = interp;
        if (extra[0] != '\0')
            rebuilt[at++] = extra;
        rebuilt[at++] = (char*)full;
        for (int i = 1; argv != 0 && argv[i] != 0 && at < MAX_ARGS + 2; ++i)
            rebuilt[at++] = argv[i];
        rebuilt[at] = 0;

        const int r = execve(interp, rebuilt, envp);
        depth = 0;
        return r;                       /* only ever reached on failure */
    }

    /* Work out what has to be mapped. For a static program that is its own
     * segments and nothing else; for a dynamic one it is those plus the
     * interpreter, plus every library it names, plus the table that tells the
     * interpreter where they all went. See user/libc/execload.c. */
    {
        static struct loader_request req;
        void* blob = 0;
        long blob_size = 0;

        if (__loader_prepare(full, &req, &blob, &blob_size) != 0)
            return -1;

        /* Last, because it has to reflect the table as it will be handed over
         * - after every descriptor the loader opened has been closed again. */
        __fd_save_for_exec();

        /* The fifth argument is r8, which is where the kernel reads the
         * environment from. A blob of zero bytes is still a pointer the
         * kernel will check, so an empty one is passed as a byte. */
        static char nothing;
        result = (int)__syscall(SYS_execve,
                                blob != 0 ? (long)blob : (long)&nothing,
                                blob_size > 0 ? blob_size : 1,
                                (long)argv, (long)&req, (long)envp);
    }

    /* Only reached when the exec failed: on success the kernel never returns
     * here, and this address space is already gone. */
    return result;
}

pid_t wait(int* status)
{
    return (pid_t)__syscall(SYS_wait, 0, (long)status, 0, 0, 0);
}

pid_t waitpid(pid_t which, int* status, int options)
{
    const long r = __syscall(SYS_waitpid, which, (long)status, options, 0, 0);
    /* The kernel answers with two different failures and they want opposite
     * things from the caller: no such child means stop asking, and a signal
     * arrived means ask again. One -1 could say neither. */
    if (r == -1) { errno = ECHILD; return -1; }
    if (r == -2) { errno = EINTR;  return -1; }
    return (pid_t)r;
}

int setpgid(pid_t pid, pid_t pgid)
{
    if (__syscall(SYS_setpgid, pid, pgid, 0, 0, 0) < 0) {
        errno = EPERM;
        return -1;
    }
    return 0;
}

pid_t getpgid(pid_t pid)
{
    const long r = __syscall(SYS_getpgid, pid, 0, 0, 0, 0);
    if (r < 0) { errno = ESRCH; return -1; }
    return (pid_t)r;
}

pid_t getpgrp(void) { return getpgid(0); }

pid_t setsid(void)
{
    const long r = __syscall(SYS_setsid, 0, 0, 0, 0, 0);
    if (r < 0) { errno = EPERM; return -1; }
    return (pid_t)r;
}

pid_t getsid(pid_t pid)
{
    const long r = __syscall(SYS_getsid, pid, 0, 0, 0, 0);
    if (r < 0) { errno = ESRCH; return -1; }
    return (pid_t)r;
}

void msleep(unsigned long ms) { __syscall(SYS_sleep, (long)ms, 0, 0, 0, 0); }

void yield(void)
{
    __syscall(SYS_yield, 0, 0, 0, 0, 0);
}
