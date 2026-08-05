#include <sys/syscall.h>
#include <errno.h>
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
    void* image;
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

    fd = open(full, O_RDONLY);
    if (fd < 0)
        return -1;
    image = malloc((size_t)st.st_size);
    if (image == 0) {
        close(fd);
        return -1;
    }
    got = read(fd, image, (unsigned long)st.st_size);
    close(fd);
    if (got != (long)st.st_size) {
        free(image);
        return -1;
    }

    /* A script names its interpreter on the first line. Handled here rather
     * than in the kernel because this is where a program is turned into
     * something runnable - the kernel is handed segments and does not know
     * what a file format is. */
    {
        const char* text = (const char*)image;
        if (got > 2 && text[0] == '#' && text[1] == '!') {
            static int depth;
            if (++depth > MAX_SHEBANG) {
                depth = 0;
                free(image);
                errno = ELOOP;
                return -1;
            }
            /* The rest of the line, split into the interpreter and at most one
             * argument - which is what every UNIX does, and why "#!/bin/sh -e"
             * works and "#!/bin/sh -e -u" does not. */
            char line[256];
            long n = 0;
            while (n < got - 2 && n < (long)sizeof(line) - 1 &&
                   text[2 + n] != '\n')
                { line[n] = text[2 + n]; ++n; }
            line[n] = '\0';
            free(image);

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
            return r;                   /* only ever reached on failure */
        }
    }

    /* Last, because it has to reflect the table as it will be handed over -
     * after the descriptor this function opened has been closed again. */
    __fd_save_for_exec();

    /* Work out what the segments are here. The kernel maps what it is told
     * and no longer knows what an ELF is - the only programs it ever loaded
     * itself are the three the build hands it, already parsed. */
    {
        struct seg { unsigned long vaddr, offset, filesz, memsz;
                     unsigned flags, pad; };
        struct { unsigned long entry; unsigned count, pad;
                 struct seg segs[16]; } req;
        struct seg* segs = req.segs;
        const unsigned char* e = (const unsigned char*)image;
        unsigned long phoff;
        unsigned short phentsize, phnum, i;
        unsigned count = 0;
        unsigned long entry;

        if (got < 64 || e[0] != 0x7F || e[1] != 'E' || e[2] != 'L' ||
            e[3] != 'F' || e[4] != 2 || e[5] != 1) {
            free(image);
            return -1;
        }
        memcpy(&entry, e + 24, 8);
        memcpy(&phoff, e + 32, 8);
        memcpy(&phentsize, e + 54, 2);
        memcpy(&phnum, e + 56, 2);

        for (i = 0; i < phnum && count < 16; ++i) {
            const unsigned char* ph = e + phoff + (unsigned long)i * phentsize;
            unsigned p_type, p_flags;
            unsigned long p_offset, p_vaddr, p_filesz, p_memsz;
            if ((unsigned long)(ph - e) + phentsize > (unsigned long)got)
                break;
            memcpy(&p_type, ph, 4);
            memcpy(&p_flags, ph + 4, 4);
            memcpy(&p_offset, ph + 8, 8);
            memcpy(&p_vaddr, ph + 16, 8);
            memcpy(&p_filesz, ph + 32, 8);
            memcpy(&p_memsz, ph + 40, 8);
            if (p_type != 1 || p_memsz == 0)
                continue;               /* only PT_LOAD carries anything */
            if (p_filesz > p_memsz || p_offset + p_filesz > (unsigned long)got) {
                free(image);
                return -1;
            }
            segs[count].vaddr  = p_vaddr;
            segs[count].offset = p_offset;
            segs[count].filesz = p_filesz;
            segs[count].memsz  = p_memsz;
            segs[count].flags  = p_flags;
            segs[count].pad    = 0;
            ++count;
        }
        if (count == 0) {
            free(image);
            return -1;
        }
        req.entry = entry;
        req.count = count;
        req.pad   = 0;
        /* The fifth argument is r8, which is where the kernel reads the
         * environment from. */
        result = (int)__syscall(SYS_execve, (long)image, got, (long)argv,
                                (long)&req, (long)envp);
    }

    /* Only reached when the exec failed: on success the kernel never returns
     * here, and this address space is already gone. */
    free(image);
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
