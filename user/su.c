/* su - become another user, after proving you are allowed to.
 *
 * The password is checked by the kernel, not here: it reads the shadow file
 * with its own privileges and switches this process's credentials only if the
 * password matches. That is why su needs no setuid bit and why no user process
 * ever holds a password hash.
 *
 * Every switch needs the target account's password, root included. Root is the
 * hub the others go through - an ordinary user can only climb to root, so
 * reaching another ordinary user costs two passwords: root's, then theirs.
 */

#include <cli.h>
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
    cli_begin(argc, argv, "[user]", "");
    if (cli_argc() > 1)
        cli_usage();
    const char* user = cli_argc() == 1 ? cli_arg(0) : "root";

    /* Say why before asking for a password that cannot possibly work. */
    if (getuid() != 0 && strcmp(user, "root") != 0) {
        cli_fail("only root can become %s - su to root first", user);
        return 1;
    }

    char password[128] = {};
    if (read_password(password, sizeof(password)) < 0) {
        cli_fail("could not read a password");
        return 1;
    }

    char home[128] = {};
    if (login(user, password, home) < 0) {
        /* Deliberately vague: saying which of the two was wrong tells an
         * attacker which usernames exist. */
        cli_fail("authentication failed");
        return 1;
    }
    memset(password, 0, sizeof(password));

    if (home[0] != '\0' && chdir(home) < 0)
        cli_fail("no home directory %s", home);

    char* sh[] = { "sh", 0 };
    execve("/bin/sh", sh, 0);
    cli_fail("could not start a shell");
    return 1;
}
