/* shutdown, reboot - stop the machine, or start it again.
 *
 * One program under two names, because they are one action with a flag: which
 * name it was run under picks the default, and -r or -h overrides it. That is
 * what every other UNIX does, and the reason is that "shutdown -r" and
 * "reboot" being different programs would be two things to keep in step.
 *
 * The filesystem is flushed by power_off and power_reboot themselves rather
 * than here - see <proc.h> - so a caller that forgets cannot lose the last few
 * writes.
 */

#include <cli.h>
#include <proc.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char** argv)
{
    cli_begin(argc, argv, "[-r] [-h]   (-r restarts, -h stops)", "rh");

    /* The name it was run under, unless a flag says otherwise. */
    const char* name = argc > 0 && argv[0] != 0 ? argv[0] : "shutdown";
    const char* leaf = name;
    for (const char* p = name; *p != '\0'; ++p)
        if (*p == '/')
            leaf = p + 1;
    int restart = strcmp(leaf, "reboot") == 0;
    if (cli_flag("-r")) restart = 1;
    if (cli_flag("-h")) restart = 0;

    if (getuid() != 0)
        cli_die("only root may stop the machine");

    printf("%s...\n", restart ? "restarting" : "shutting down");
    fflush(stdout);
    if (restart)
        power_reboot();
    else
        power_off();

    /* Neither returns when it works, so getting here is the answer. */
    cli_fail("the machine did not answer");
    return 1;
}
