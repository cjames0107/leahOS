/* leahOS init - the first user process.
 *
 * It runs a short scripted demo of the real commands - so a headless boot
 * proves the filesystem syscalls, argv passing and coreutils all work without
 * needing keyboard input - and then hands off to the interactive shell.
 */

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

    printf("\ninit: demo complete, launching shell\n");

    char* sh_args[] = { "sh", 0 };
    execve("/BIN/SH.ELF", sh_args, 0);

    printf("init: could not launch shell\n");
    return 1;
}
