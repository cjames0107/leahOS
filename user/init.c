/* leahOS init - the first user process.
 *
 * It launches the interactive shell and, should the shell ever exit, keeps the
 * system from falling off the end of userspace by reporting it and returning.
 */

#include <stdio.h>
#include <unistd.h>

int main(void)
{
    // login owns the console from here: it authenticates, starts a shell as
    // whoever logged in, and comes back to its prompt when that shell exits.
    char* login_args[] = { "login", 0 };
    execve("/BIN/LOGIN.ELF", login_args, 0);

    printf("init: could not launch login\n");
    return 1;
}
