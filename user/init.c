/* leahOS init - the first user process.
 *
 * For now it exists to exercise the process syscalls: it forks a child, the
 * child replaces itself with /BIN/HELLO.ELF via execve, and the parent waits
 * for it and reports the exit status. Once there is a filesystem interface and
 * a console driver for user space, this becomes a real init that launches a
 * shell.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void)
{
    printf("init: pid %d, starting\n", getpid());

    const pid_t pid = fork();
    if (pid < 0) {
        printf("init: fork failed\n");
        return 1;
    }

    if (pid == 0) {
        /* Child: prove fork returned 0 here and a different pid, then become a
         * different program entirely. */
        printf("  child: pid %d (fork returned 0), exec /BIN/HELLO.ELF\n", getpid());

        char* argv[] = { "/BIN/HELLO.ELF", 0 };
        execve("/BIN/HELLO.ELF", argv, 0);

        /* Only reached if exec fails - it replaces the image on success. */
        printf("  child: execve failed\n");
        exit(127);
    }

    /* Parent: fork returned the child's pid. Wait for it and read its status. */
    printf("init: forked child pid %d, waiting\n", pid);

    int status = 0;
    const pid_t reaped = wait(&status);
    printf("init: child %d exited with status 0x%x\n", reaped, status);

    return 0;
}
