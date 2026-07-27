/* passwd - change a password.
 *
 * Root may change anyone's without knowing the old one. Anyone else may change
 * only their own, and has to prove they know it - which the kernel checks, not
 * this program.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int read_line(const char* prompt, char* out, int max)
{
    printf("%s", prompt);
    setecho(0);
    int n = (int)read(0, out, max - 1);
    setecho(1);
    printf("\n");
    if (n <= 0)
        return -1;
    if (out[n - 1] == '\n')
        --n;
    out[n] = '\0';
    return n;
}

int main(int argc, char** argv)
{
    char self[32] = {};
    if (username(getuid(), self) != 0) {
        printf("passwd: no account for this uid\n");
        return 1;
    }
    const char* target = argc == 2 ? argv[1] : self;
    if (argc > 2) {
        printf("usage: passwd [user]\n");
        return 1;
    }

    char old_password[128] = {};
    const int need_old = getuid() != 0;
    if (need_old && read_line("Current password: ", old_password,
                              sizeof(old_password)) < 0)
        return 1;

    char new_password[128] = {};
    char again[128] = {};
    if (read_line("New password: ", new_password, sizeof(new_password)) < 0)
        return 1;
    if (read_line("Retype new password: ", again, sizeof(again)) < 0)
        return 1;

    if (strcmp(new_password, again) != 0) {
        printf("passwd: passwords do not match\n");
        return 1;
    }

    if (passwd(target, need_old ? old_password : 0, new_password) < 0) {
        printf("passwd: could not change the password for %s\n", target);
        return 1;
    }
    memset(old_password, 0, sizeof(old_password));
    memset(new_password, 0, sizeof(new_password));
    memset(again, 0, sizeof(again));

    printf("password changed for %s\n", target);
    return 0;
}
