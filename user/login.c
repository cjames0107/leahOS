/* login - the gate in front of the shell.
 *
 * Runs as root from init and stays root: the shell it starts is a child that
 * drops to the authenticated user, so when that shell exits this loops back to
 * the prompt rather than leaving a root shell behind. That is what makes `exit`
 * a logout.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* Distinct from any status a shell is likely to return, so the parent can tell
 * "wrong password" from "the user typed exit". */
#define AUTH_FAILED 111

static int read_line(const char* prompt, char* out, int max, int echo)
{
    printf("%s", prompt);
    if (!echo)
        setecho(0);
    int n = (int)read(0, out, max - 1);
    if (!echo) {
        setecho(1);
        printf("\n");           /* the Enter was not echoed either */
    }
    if (n <= 0)
        return -1;
    if (out[n - 1] == '\n')
        --n;
    out[n] = '\0';
    return n;
}

int main(void)
{
    for (;;) {
        char user[64] = {};
        char password[128] = {};

        printf("\n");
        if (read_line("leahOS login: ", user, sizeof(user), 1) <= 0)
            continue;
        if (read_line("Password: ", password, sizeof(password), 0) < 0)
            continue;

        const int pid = fork();
        if (pid == 0) {
            char home[128] = {};
            if (login(user, password, home) < 0)
                exit(AUTH_FAILED);
            if (home[0] != '\0')
                chdir(home);
            char* sh[] = { "sh", 0 };
            execve("/BIN/SH.ELF", sh, 0);
            exit(127);
        }

        int status = 0;
        wait(&status);
        if (status == AUTH_FAILED) {
            /* Deliberately says nothing about which half was wrong. */
            printf("Login incorrect\n");
        } else {
            printf("logout\n");
        }
    }
    return 0;
}
