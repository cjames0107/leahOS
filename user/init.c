/* leahOS init - the first user process.
 *
 * It starts the servers that the rest of the system assumes are there, and
 * then hands the console to login.
 *
 * The servers come first because everything after them is a client. The
 * network card's driver owns the card; the stack owns the protocols and talks
 * to the driver; and every program that wants to send anything talks to the
 * stack. None of those three can see another's memory, and none of them is in
 * the kernel - which means something has to launch them in order, and that
 * something is here.
 */

#include <ipc.h>
#include <screen.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* --- the boot splash ---------------------------------------------------------
 *
 * The kernel's console goes to the serial port and nowhere else now, so a
 * machine booting with nobody watching a serial line shows a black screen from
 * power-on until the desktop appears. This is what fills that: a bar that
 * advances as each server claims its port, drawn by the process that is
 * starting them, because it is the one that knows.
 *
 * Drawn straight onto the framebuffer. There is no window server yet - that is
 * rather the point of a boot splash. */

#define SERVER_COUNT 6

#define BG      0x008894A8u
#define BAR_BG  0x00DDDDDDu
#define BAR_FG  0x00555555u
#define TEXT    0x00000000u
#define DIM     0x00333333u

static int g_have_screen;
static int g_done;

static void splash_open(void)
{
    g_have_screen = screen_open() == 0;
    if (!g_have_screen)
        return;                 /* no framebuffer; serial is still telling the story */
    screen_fill(0, 0, (int)screen_width(), (int)screen_height(), BG);
    screen_text_centred((int)screen_width() / 2,
                        (int)screen_height() / 2 - 60, "leahOS", TEXT, BG, 0);
}

/* The bar, and under it the name of whatever is being waited for. */
static void splash_progress(const char* what)
{
    const int cx = (int)screen_width() / 2;
    const int cy = (int)screen_height() / 2;
    const int w = 320, h = 12;
    const int x = cx - w / 2, y = cy - h / 2;
    int filled;

    if (!g_have_screen)
        return;

    screen_fill(x, y, w, h, BAR_BG);
    filled = g_done * w / SERVER_COUNT;
    if (filled > 0)
        screen_fill(x, y, filled, h, BAR_FG);
    /* Sunken, with a hard edge: a progress bar of this era is a well with
     * something filling it, not a floating capsule. */
    screen_bevel(x - 1, y - 1, w + 2, h + 2, 0);
    screen_frame(x - 2, y - 2, w + 4, h + 4, 0x00000000u);

    /* Cleared first, or a shorter name leaves the tail of a longer one. */
    screen_fill(x - 40, y + h + 12, w + 80, screen_glyph_height(), BG);
    if (what != 0)
        screen_text_centred(cx, y + h + 12, what, DIM, BG, 0);
}

/* Start a server and wait for the port it is going to claim, so the next one
 * up does not race it. Waiting on the port rather than on a delay means a slow
 * machine waits longer and a fast one does not wait at all. */
static void start(const char* path, const char* name, unsigned port)
{
    splash_progress(name);
    if (fork() == 0) {
        char* argv[2];
        argv[0] = (char*)name;
        argv[1] = 0;
        execve(path, argv, 0);
        exit(127);
    }
    for (int i = 0; i < 500; ++i) {
        if (port_open(port) >= 0) {
            ++g_done;
            splash_progress(name);
            return;
        }
        msleep(10);
    }
    printf("init: %s did not come up\n", name);
    ++g_done;                   /* it is not coming; do not stall the bar on it */
    splash_progress(name);
}

int main(void)
{
    splash_open();

    start("/sbin/e1000d", "e1000d", IPC_PORT_NIC);
    start("/sbin/netd", "netd", IPC_PORT_NET);
    start("/sbin/audiod", "audiod", IPC_PORT_AUDIO);
    start("/sbin/authd", "authd", IPC_PORT_AUTH);
    /* Input before login, or there is nothing to type the password with. Both
     * of these are keyboards; whichever the machine has is the one that
     * answers. */
    start("/sbin/ps2d", "ps2d", IPC_PORT_PS2);
    start("/sbin/usbd", "usbd", IPC_PORT_USB);

    splash_progress("starting login");

    // login owns the screen from here: it authenticates, starts a shell as
    // whoever logged in, and comes back to its prompt when that shell exits.
    char* login_args[] = { "login", 0 };
    execve("/sbin/login", login_args, 0);

    printf("init: could not launch login\n");
    return 1;
}
