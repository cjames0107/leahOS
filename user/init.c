/* leahOS init - the first user process.
 *
 * It launches the interactive shell and, should the shell ever exit, keeps the
 * system from falling off the end of userspace by reporting it and returning.
 */

#include <stdio.h>
#include <unistd.h>

int main(void)
{
    char* sh_args[] = { "sh", 0 };
    execve("/BIN/SH.ELF", sh_args, 0);

    printf("init: could not launch shell\n");
    return 1;
}
