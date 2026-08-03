/* login - the gate in front of the shell.
 *
 * Runs as root from init and stays root: the shell it starts is a child that
 * drops to the authenticated user, so when that shell exits this loops back to
 * the prompt rather than leaving a root shell behind. That is what makes `exit`
 * a logout.
 */

#include <bundle.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <shm.h>
#include <signal.h>
#include <display.h>
#include <screen.h>
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
        char* setts[]  = { "settings", "380", "60", 0 };
        const char* which[] = { "/BIN/DESKTOP.ELF", app_path("Files"),
                                app_path("Terminal"), app_path("Settings") };
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

/* --- the login screen ---------------------------------------------------------
 *
 * Drawn onto the framebuffer directly, because this runs before the window
 * server does - and since the kernel's console became serial-only there is
 * nothing on the screen at all until something puts it there.
 *
 * Keys come from input_poll rather than read(), because this wants them one at
 * a time: a password field has to echo something without echoing the password,
 * and Tab has to move between fields rather than being a character. read()
 * gives back a cooked line, which is the wrong shape for both.
 */

#define BG      0x00101418u
#define PANEL   0x001C232Au
#define FIELD   0x00131A20u
#define TEXT    0x00C8D0D4u
#define DIM     0x00707880u
#define ACCENT  0x0038B0A0u
#define BAD     0x00C05048u

#define FIELD_MAX 32

static int g_have_screen;

/* One field, drawn with its own frame so the focused one is obvious. */
static void draw_field(int x, int y, int w, const char* label, const char* value,
                       int masked, int focused)
{
    const int gh = screen_glyph_height();
    char shown[FIELD_MAX + 1];
    int i;

    screen_text(x, y, label, DIM, PANEL, 0);
    screen_fill(x, y + gh + 4, w, gh + 8, FIELD);
    screen_frame(x, y + gh + 4, w, gh + 8, focused ? ACCENT : DIM);

    for (i = 0; value[i] != '\0' && i < FIELD_MAX; ++i)
        shown[i] = masked ? '*' : value[i];
    shown[i] = '\0';
    screen_text(x + 6, y + gh + 8, shown, TEXT, FIELD, 0);

    /* A caret, so an empty focused field still says where typing goes. */
    if (focused)
        screen_fill(x + 6 + screen_text_width(shown), y + gh + 8,
                    2, gh, ACCENT);
}

static void draw_login(const char* user, const char* password, int focus,
                       const char* message)
{
    const int w = 380, h = 210;
    const int x = (int)screen_width() / 2 - w / 2;
    const int y = (int)screen_height() / 2 - h / 2;

    screen_fill(0, 0, (int)screen_width(), (int)screen_height(), BG);
    screen_fill(x, y, w, h, PANEL);
    screen_frame(x, y, w, h, DIM);

    screen_text_centred(x + w / 2, y + 18, "leahOS", TEXT, PANEL, 0);

    draw_field(x + 30, y + 56, w - 60, "username", user, 0, focus == 0);
    draw_field(x + 30, y + 118, w - 60, "password", password, 1, focus == 1);

    if (message != 0)
        screen_text_centred(x + w / 2, y + h - 26, message, BAD, PANEL, 0);
    else
        screen_text_centred(x + w / 2, y + h - 26,
                            "tab to move, enter to sign in", DIM, PANEL, 0);
}

/* Returns 1 when the two fields have been filled in and submitted. */
static int prompt_on_screen(char* user, char* password, const char* message)
{
    int focus = 0;
    int lengths[2] = { 0, 0 };
    char* fields[2];

    fields[0] = user;
    fields[1] = password;
    user[0] = '\0';
    password[0] = '\0';
    draw_login(user, password, focus, message);

    for (;;) {
        struct input_state in;
        char c;

        if (input_poll(&in) != 0)
            return 0;
        if (in.key == 0) {
            msleep(15);
            continue;
        }
        c = (char)in.key;

        if (c == '\t') {
            focus = focus == 0 ? 1 : 0;
        } else if (c == '\n' || c == '\r') {
            if (focus == 0) {
                focus = 1;                  /* enter moves on, like tab */
            } else {
                return 1;                   /* and submits from the last field */
            }
        } else if (c == '\b') {
            if (lengths[focus] > 0)
                fields[focus][--lengths[focus]] = '\0';
        } else if (c >= ' ' && c < 127 && lengths[focus] < FIELD_MAX) {
            fields[focus][lengths[focus]++] = c;
            fields[focus][lengths[focus]] = '\0';
        } else {
            continue;                       /* arrows and the rest: not text */
        }
        draw_login(user, password, focus, message);
    }
}

/* Wait for one particular child, reaping anything else that turns up.
 *
 * login is what init became - execve keeps the process - so every server init
 * started is still a child of this process. An unqualified wait() therefore
 * returns whichever of them dies first, and on a machine with no xHCI
 * controller usbd exits during boot and leaves a zombie pending before login
 * has even drawn its prompt.
 *
 * That was two bugs. The session was declared over the moment any server
 * exited, so the desktop never appeared and the screen said "logout". And, far
 * worse, the password check below read that stranger's exit status as its own
 * verdict: a dead server that exited 0 or 1 is not AUTH_FAILED, so a *wrong
 * password was accepted*. Confirmed by typing one and being let in.
 *
 * Reaping the strangers as they go is the right thing to do with them anyway. */
static int wait_for(int want, int* status_out)
{
    for (;;) {
        int status = 0;
        const int got = wait(&status);
        if (got < 0)
            return -1;                  /* no children left at all */
        if (got == want) {
            if (status_out != 0)
                *status_out = status;
            return got;
        }
    }
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
    if (wait_for(pid, &status) < 0)
        return 0;                       /* it vanished; refuse rather than guess */
    return status != AUTH_FAILED;
}

int main(void)
{
    int incorrect = 0;

    /* If there is a framebuffer, this is where it starts being used - the boot
     * splash drew the last thing on it and login takes over from there. */
    g_have_screen = screen_open() == 0;

    for (;;) {
        char user[64] = {};
        char password[128] = {};

        if (g_have_screen) {
            if (!prompt_on_screen(user, password, incorrect ? "login incorrect" : 0))
                continue;
        } else {
            /* No framebuffer: the serial line is the only thing anyone can be
             * looking at, so ask there instead. */
            printf("\n");
            if (incorrect)
                printf("Login incorrect\n");
            if (read_line("leahOS login: ", user, sizeof(user), 1) <= 0)
                continue;
            if (read_line("Password: ", password, sizeof(password), 0) < 0)
                continue;
        }
        incorrect = 0;

        if (!password_accepted(user, password)) {
            /* Deliberately says nothing about which half was wrong. */
            incorrect = 1;
            if (!g_have_screen)
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
            /* Wait on the server rather than on a stopwatch. Six fixed
             * seconds was fine on a fast machine and wrong on a loaded one:
             * the server was still starting, the clock ran out, and the
             * session silently became a text shell. As long as the process is
             * alive it is still coming up, and a slow machine deserves the
             * same desktop a fast one gets. The bound that matters - a server
             * that died - is answered by kill(pid, 0), which runs every check
             * a real signal would and then delivers nothing. */
            for (int i = 0; i < 6000 && !win_server_running(); ++i) {
                if (kill(server, 0) != 0)
                    break;          /* gone; there will be no desktop */
                msleep(10);
            }
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

        wait_for(pid, 0);       /* this session, not whatever exits first */

        /* The server has no reason of its own to stop if the session ended
         * without ever opening a window, so say so rather than leaving it
         * running behind the next login prompt. */
        if (server > 0) {
            kill(server, SIGTERM);
            wait_for(server, 0);
        }
        printf("logout\n");
    }
    return 0;
}
