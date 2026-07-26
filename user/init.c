/* leahOS init - the first user process.
 *
 * It runs a short scripted demo of the real commands - so a headless boot
 * proves the filesystem syscalls, argv passing and coreutils all work without
 * needing keyboard input - and then hands off to the interactive shell.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* Fork, exec /BIN/<PATH>, and wait. argv[0] is the command name as typed. */
static void run(const char* elf, char** argv)
{
    const pid_t pid = fork();
    if (pid == 0) {
        execve(elf, argv, 0);
        printf("init: exec %s failed\n", elf);
        exit(127);
    }
    int status = 0;
    wait(&status);
}

int main(void)
{
    printf("init: pid %d, running startup demo\n\n", getpid());

    char* echo_args[] = { "echo", "hello", "from", "leahOS", 0 };
    printf("$ echo hello from leahOS\n");
    run("/BIN/ECHO.ELF", echo_args);

    char* pwd_args[] = { "pwd", 0 };
    printf("$ pwd\n");
    run("/BIN/PWD.ELF", pwd_args);

    char* ls_args[] = { "ls", "/", 0 };
    printf("$ ls /\n");
    run("/BIN/LS.ELF", ls_args);

    char* cat_args[] = { "cat", "/HELLO.TXT", 0 };
    printf("$ cat /HELLO.TXT\n");
    run("/BIN/CAT.ELF", cat_args);

    // The write commands: make a directory, copy a file into it, list it, then
    // remove what we made - proving mkdir, cp, ls and rm end to end.
    char* mkdir_args[] = { "mkdir", "/PLAY", 0 };
    printf("$ mkdir /PLAY\n");
    run("/BIN/MKDIR.ELF", mkdir_args);

    char* cp_args[] = { "cp", "/HELLO.TXT", "/PLAY/COPY.TXT", 0 };
    printf("$ cp /HELLO.TXT /PLAY/COPY.TXT\n");
    run("/BIN/CP.ELF", cp_args);

    char* ls_play[] = { "ls", "/PLAY", 0 };
    printf("$ ls /PLAY\n");
    run("/BIN/LS.ELF", ls_play);

    char* cat_copy[] = { "cat", "/PLAY/COPY.TXT", 0 };
    printf("$ cat /PLAY/COPY.TXT\n");
    run("/BIN/CAT.ELF", cat_copy);

    char* rm_args[] = { "rm", "/PLAY/COPY.TXT", 0 };
    printf("$ rm /PLAY/COPY.TXT\n");
    run("/BIN/RM.ELF", rm_args);

    // Redirection: send echo's output to a file, then read it back.
    printf("$ echo redirected to a file > /REDIR.TXT\n");
    if (fork() == 0) {
        const int fd = open("/REDIR.TXT", O_WRONLY | O_CREAT | O_TRUNC);
        dup2(fd, 1);
        close(fd);
        char* a[] = { "echo", "redirected", "to", "a", "file", 0 };
        execve("/BIN/ECHO.ELF", a, 0);
        exit(127);
    }
    wait(0);
    char* cat_redir[] = { "cat", "/REDIR.TXT", 0 };
    printf("$ cat /REDIR.TXT\n");
    run("/BIN/CAT.ELF", cat_redir);

    // Rename (atomic, no data copy), then read the file back at its new name.
    char* mv_args[] = { "mv", "/REDIR.TXT", "/MOVED.TXT", 0 };
    printf("$ mv /REDIR.TXT /MOVED.TXT\n");
    run("/BIN/MV.ELF", mv_args);
    char* cat_moved[] = { "cat", "/MOVED.TXT", 0 };
    printf("$ cat /MOVED.TXT\n");
    run("/BIN/CAT.ELF", cat_moved);
    char* rm_moved[] = { "rm", "/MOVED.TXT", 0 };
    run("/BIN/RM.ELF", rm_moved);

    // A pipe: echo's stdout feeds cat's stdin.
    printf("$ echo piped through a pipe | cat\n");
    int pfd[2];
    pipe(pfd);
    if (fork() == 0) {
        dup2(pfd[1], 1);
        close(pfd[0]);
        close(pfd[1]);
        char* a[] = { "echo", "piped", "through", "a", "pipe", 0 };
        execve("/BIN/ECHO.ELF", a, 0);
        exit(127);
    }
    if (fork() == 0) {
        dup2(pfd[0], 0);
        close(pfd[0]);
        close(pfd[1]);
        char* a[] = { "cat", 0 };
        execve("/BIN/CAT.ELF", a, 0);
        exit(127);
    }
    close(pfd[0]);
    close(pfd[1]);
    wait(0);
    wait(0);

    // hello exercises the growing sbrk heap.
    char* hello_args[] = { "hello", 0 };
    printf("$ hello\n");
    run("/BIN/HELLO.ELF", hello_args);

    printf("\ninit: demo complete, launching shell\n");

    char* sh_args[] = { "sh", 0 };
    execve("/BIN/SH.ELF", sh_args, 0);

    printf("init: could not launch shell\n");
    return 1;
}
