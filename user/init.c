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
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* Start a server and wait for the port it is going to claim, so the next one
 * up does not race it. Waiting on the port rather than on a delay means a slow
 * machine waits longer and a fast one does not wait at all. */
static void start(const char* path, const char* name, unsigned port)
{
    if (fork() == 0) {
        char* argv[2];
        argv[0] = (char*)name;
        argv[1] = 0;
        execve(path, argv, 0);
        exit(127);
    }
    for (int i = 0; i < 500; ++i) {
        if (port_open(port) >= 0)
            return;
        msleep(10);
    }
    printf("init: %s did not come up\n", name);
}

int main(void)
{
    start("/BIN/E1000D.ELF", "e1000d", IPC_PORT_NIC);
    start("/BIN/NETD.ELF", "netd", IPC_PORT_NET);
    start("/BIN/AUDIOD.ELF", "audiod", IPC_PORT_AUDIO);
    start("/BIN/AUTHD.ELF", "authd", IPC_PORT_AUTH);
    start("/BIN/USBD.ELF", "usbd", IPC_PORT_USB);

    // login owns the console from here: it authenticates, starts a shell as
    // whoever logged in, and comes back to its prompt when that shell exits.
    char* login_args[] = { "login", 0 };
    execve("/BIN/LOGIN.ELF", login_args, 0);

    printf("init: could not launch login\n");
    return 1;
}
