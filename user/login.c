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
#include <shm.h>
#include <signal.h>
#include <window.h>

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

/* What an authenticated user gets: a desktop with a shell already on it.
 *
 * The desktop opens a terminal, so the shell is a window rather than something
 * waiting for the desktop to finish. Anything else - paint, another terminal -
 * is launched from there like any other program.
 *
 * The text console is still underneath, because there is one framebuffer and no
 * way to switch between virtual consoles. Closing every window ends the desktop
 * and hands the screen back, and the shell that starts on it is what `exit`
 * logs out of. That is also the path taken when there is no desktop at all.
 *
 * By the time this runs the server is already up: login waits for it, so there
 * is nothing to poll for here. */
static void session(void)
{
    if (win_server_running()) {
        printf("starting the desktop - the terminal window is your shell.\n");
        /* The desktop first, so it is at the back before anything else opens. */
        char* desk[]   = { "desktop", 0 };
        char* browse[] = { "browse", "40", "40", 0 };
        char* term[]   = { "term", "60", "300", 0 };
        char* setts[]  = { "settings", "430", "60", 0 };
        const char* which[] = { "/BIN/DESKTOP.ELF", "/BIN/BROWSE.ELF",
                                "/BIN/TERM.ELF", "/BIN/SETTINGS.ELF" };
        char** argv[] = { desk, browse, term, setts };
        int started = 0;
        for (int i = 0; i < 4; ++i) {
            const int pid = fork();
            if (pid == 0) {
                execve(which[i], argv[i], 0);
                exit(127);
            }
            if (pid > 0)
                ++started;
        }
        for (int i = 0; i < started; ++i)
            wait(0);
    }

    /* The desktop is gone and the console is back. */
    char* sh[] = { "sh", 0 };
    execve("/BIN/SH.ELF", sh, 0);
    printf("login: cannot start a shell\n");
}

/* Check the password without giving this process away.
 *
 * login() changes the credentials of whoever calls it, and this process has to
 * stay root - it starts the window server, which needs the framebuffer, and it
 * has to come back to the prompt afterwards. So the check happens in a child
 * that exists only to report the answer. */
static int password_accepted(const char* user, const char* password)
{
    const int pid = fork();
    if (pid < 0)
        return 0;
    if (pid == 0) {
        char home[128] = {};
        exit(login(user, password, home) < 0 ? AUTH_FAILED : 0);
    }
    int status = 0;
    wait(&status);
    return status != AUTH_FAILED;
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

        if (!password_accepted(user, password)) {
            /* Deliberately says nothing about which half was wrong. */
            printf("Login incorrect\n");
            continue;
        }

        /* Start the desktop before the session, not alongside it. Mapping the
         * framebuffer is root's to do, so it has to be started from here rather
         * than by the user's own processes - and starting it first means the
         * session never has to wonder whether it is ready yet. */
        const int server = fork();
        if (server == 0) {
            char* args[] = { "wserver", 0 };
            execve("/BIN/WSERVER.ELF", args, 0);
            exit(127);
        }
        if (server > 0) {
            /* Bounded: a server that cannot start must not hold up the login.
             * Falling through without one simply means a text session. */
            for (int i = 0; i < 600 && !win_server_running(); ++i)
                msleep(10);         /* up to six seconds, without spinning */
        }

        const int pid = fork();
        if (pid == 0) {
            char home[128] = {};
            if (login(user, password, home) < 0)
                exit(AUTH_FAILED);
            if (home[0] != '\0')
                chdir(home);
            session();
            exit(0);
        }
        (void)pid;

        wait(0);                /* the session */

        /* The server has no reason of its own to stop if the session ended
         * without ever opening a window, so say so rather than leaving it
         * running behind the next login prompt. */
        if (server > 0) {
            kill(server, SIGTERM);
            wait(0);
        }
        printf("logout\n");
    }
    return 0;
}
