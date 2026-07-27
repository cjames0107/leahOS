/* su - become another user, after proving you are allowed to.
 *
 * The password is checked by the kernel, not here: it reads the shadow file
 * with its own privileges and switches this process's credentials only if the
 * password matches. That is why su needs no setuid bit and why no user process
 * ever holds a password hash.
 *
 * Root is not asked for a password. It can already become anyone by other
 * means, so demanding one would be theatre rather than security.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Read a line with the terminal not echoing it. */
static int read_password(char* out, int max)
{
    printf("Password: ");
    setecho(0);
    int n = (int)read(0, out, max - 1);
    setecho(1);
    printf("\n");                   /* the user's Enter was not echoed either */

    if (n <= 0)
        return -1;
    if (out[n - 1] == '\n')
        --n;
    out[n] = '\0';
    return n;
}

int main(int argc, char** argv)
{
    if (argc > 2) {
        printf("usage: su [user]\n");
        return 1;
    }
    const char* user = argc == 2 ? argv[1] : "root";

    char password[128] = {};
    const int need_password = getuid() != 0;
    if (need_password && read_password(password, sizeof(password)) < 0) {
        printf("su: could not read a password\n");
        return 1;
    }

    char home[128] = {};
    if (login(user, need_password ? password : 0, home) < 0) {
        /* Deliberately vague: saying which of the two was wrong tells an
         * attacker which usernames exist. */
        printf("su: authentication failed\n");
        return 1;
    }
    memset(password, 0, sizeof(password));

    if (home[0] != '\0' && chdir(home) < 0)
        printf("su: no home directory %s\n", home);

    char* sh[] = { "sh", 0 };
    execve("/BIN/SH.ELF", sh, 0);
    printf("su: could not start a shell\n");
    return 1;
}
