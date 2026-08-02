#include <sys/syscall.h>
#include <fcntl.h>
#include <stdlib.h>
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
int execve(const char* path, char* const argv[], char* const envp[])
{
    char full[256];
    struct stat st;
    int fd;
    void* image;
    long got;
    int result;

    (void)envp;

    __fd_resolve(path, full);
    if (stat(full, &st) != 0 || st.st_type != S_IFREG || st.st_size == 0)
        return -1;

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

    /* Last, because it has to reflect the table as it will be handed over -
     * after the descriptor this function opened has been closed again. */
    __fd_save_for_exec();

    result = (int)__syscall(SYS_execve, (long)image, got, (long)argv, 0, 0);

    /* Only reached when the exec failed: on success the kernel never returns
     * here, and this address space is already gone. */
    free(image);
    return result;
}

pid_t wait(int* status)
{
    return (pid_t)__syscall(SYS_wait, 0, (long)status, 0, 0, 0);
}

void msleep(unsigned long ms) { __syscall(SYS_sleep, (long)ms, 0, 0, 0, 0); }

void yield(void)
{
    __syscall(SYS_yield, 0, 0, 0, 0, 0);
}
