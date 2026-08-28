/* env - show the environment, or run something with it changed.
 *
 *     env                     print it
 *     env NAME=value cmd ...  run cmd with NAME set
 *     env -i cmd ...          run cmd with nothing but what follows
 *     env -u NAME cmd ...     run cmd without NAME
 */

#include <errno.h>
#include <paths.h>
#include <cli.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char** argv)
{
    /* env's own options stop at the command, whose arguments are the
     * command's business and must not be read as env's - so the library is
     * told to parse nothing and env reads its two options itself. */
    cli_begin(argc, argv, "[-i] [-u NAME] [NAME=VALUE...] [command [arg...]]",
              0);

    int i = 1;

    for (; i < argc; ++i) {
        if (strcmp(argv[i], "-i") == 0) {
            clearenv();
        } else if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) {
            unsetenv(argv[++i]);
        } else {
            break;
        }
    }

    /* Assignments come before the command, which is what makes
     * `env PATH=/bin sh` read the way it does. */
    for (; i < argc; ++i) {
        char* eq = strchr(argv[i], '=');
        if (eq == 0)
            break;
        *eq = '\0';
        setenv(argv[i], eq + 1, 1);
        *eq = '=';                      /* put argv back as it was found */
    }

    if (i >= argc) {
        for (char** e = environ; e != 0 && *e != 0; ++e)
            printf("%s\n", *e);
        return 0;
    }

    char path[256];
    if (path_find_program(argv[i], path, sizeof(path)) != 0) {
        cli_fail("%s: command not found", argv[i]);
        return 127;
    }
    execve(path, &argv[i], environ);
    cli_fail("%s: %s", argv[i], strerror(errno));
    return 126;
}
