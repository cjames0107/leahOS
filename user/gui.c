/* gui - start the desktop again from a shell.
 *
 * login already brings the desktop up when you log in; this is for getting it
 * back afterwards. It starts a server if there is not one running, then the
 * demo clients, and waits for them.
 *
 * Starting the server needs the framebuffer, which is root's, so an ordinary
 * user cannot do it from a shell - the message below says so rather than just
 * failing.
 */

#include <bundle.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <window.h>

static void launch(const char* path, char** argv)
{
    const int pid = fork();
    if (pid < 0) {
        printf("gui: cannot fork for %s\n", path);
        return;
    }
    if (pid == 0) {
        execve(path, argv, 0);
        printf("gui: cannot exec %s\n", path);
        exit(127);
    }
}

int main(void)
{
    /* Bring a server up if there is not one already. Mapping the framebuffer is
     * root's to do, so an ordinary user cannot start the desktop from a shell -
     * login starts it for them instead. */
    if (!win_server_running()) {
        if (fork() == 0) {
            char* args[] = { "wserver", 0 };
            execve("/sbin/wserver", args, 0);
            exit(127);
        }
        for (int i = 0; i < 600 && !win_server_running(); ++i)
            msleep(10);
        if (!win_server_running()) {
            printf("gui: no window server, and only root can start one.\n");
            printf("     log in again - the desktop starts by itself.\n");
            return 1;
        }
    }

    printf("starting the desktop.\n");
    printf("  drag a title bar to move a window, click the box to close it.\n");
    printf("  in Paint: drag to draw, 1-4 pick a colour, c clears, q quits.\n");
    printf("  the console comes back when the last window closes.\n");

    char* paint1[] = { "paint", "80", "90", 0 };
    char* paint2[] = { "paint", "420", "150", 0 };
    char* clock[]  = { "clock", 0 };
    launch(app_path("Paint"), paint1);
    launch(app_path("Paint"), paint2);
    launch(app_path("Clock"), clock);

    wait(0);
    wait(0);
    wait(0);
    return 0;
}
