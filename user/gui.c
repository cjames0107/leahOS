/* gui - start the desktop and put a couple of windows on it.
 *
 * The server itself comes up when the first window is created, so this is only
 * a launcher: it starts the demo clients and waits for them.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

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
    printf("starting the desktop.\n");
    printf("  drag a title bar to move a window, click the box to close it.\n");
    printf("  in Paint: drag to draw, 1-4 pick a colour, c clears, q quits.\n");
    printf("  the console comes back when the last window closes.\n");

    char* paint1[] = { "paint", "80", "90", 0 };
    char* paint2[] = { "paint", "420", "150", 0 };
    char* clock[]  = { "clock", 0 };
    launch("/BIN/PAINT.ELF", paint1);
    launch("/BIN/PAINT.ELF", paint2);
    launch("/BIN/CLOCK.ELF", clock);

    wait(0);
    wait(0);
    wait(0);
    return 0;
}
