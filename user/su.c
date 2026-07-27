/* Switch to another uid and run a shell as that user.
 *
 * There are no passwords yet - there is no password file to check one against -
 * so this only enforces what the kernel enforces: root may become anyone, and
 * anyone else is refused. */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int parse_uint(const char* text, unsigned* out)
{
    unsigned value = 0;
    if (*text == '\0')
        return -1;
    for (; *text != '\0'; ++text) {
        if (*text < '0' || *text > '9')
            return -1;
        value = value * 10 + (unsigned)(*text - '0');
    }
    *out = value;
    return 0;
}

int main(int argc, char** argv)
{
    unsigned uid = 0;
    if (argc > 2) {
        printf("usage: su [uid]\n");
        return 1;
    }
    if (argc == 2 && parse_uint(argv[1], &uid) < 0) {
        printf("su: bad uid '%s'\n", argv[1]);
        return 1;
    }

    if (setuid(uid) < 0) {
        printf("su: permission denied\n");
        return 1;
    }

    printf("su: now uid %u\n", uid);
    char* sh[] = { "sh", 0 };
    execve("/BIN/SH.ELF", sh, 0);
    printf("su: could not start a shell\n");
    return 1;
}
