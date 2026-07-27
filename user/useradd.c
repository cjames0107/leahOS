/* useradd - create an account. Root only.
 *
 * The password is hashed inside the kernel, which also creates the home
 * directory and gives it to its new owner. Nothing here ever holds a digest.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int read_line(const char* prompt, char* out, int max, int echo)
{
    printf("%s", prompt);
    if (!echo)
        setecho(0);
    int n = (int)read(0, out, max - 1);
    if (!echo) {
        setecho(1);
        printf("\n");
    }
    if (n <= 0)
        return -1;
    if (out[n - 1] == '\n')
        --n;
    out[n] = '\0';
    return n;
}

static unsigned parse_uint(const char* text, unsigned fallback)
{
    if (*text < '0' || *text > '9')
        return fallback;
    unsigned value = 0;
    for (; *text >= '0' && *text <= '9'; ++text)
        value = value * 10 + (unsigned)(*text - '0');
    return value;
}

int main(int argc, char** argv)
{
    if (argc < 2 || argc > 3) {
        printf("usage: useradd <name> [uid]\n");
        return 1;
    }
    if (getuid() != 0) {
        printf("useradd: only root can create accounts\n");
        return 1;
    }

    const char* name = argv[1];
    /* 0 asks the kernel for the next free uid; an explicit one is honoured but
     * refused if it is already in use. */
    const unsigned uid = argc == 3 ? parse_uint(argv[2], 0) : 0;

    char password[128] = {};
    char again[128] = {};
    if (read_line("New password: ", password, sizeof(password), 0) < 0)
        return 1;
    if (read_line("Retype password: ", again, sizeof(again), 0) < 0)
        return 1;
    if (strcmp(password, again) != 0) {
        printf("useradd: passwords do not match\n");
        return 1;
    }
    if (password[0] == '\0') {
        printf("useradd: refusing to create an account with no password\n");
        return 1;
    }

    char home[128];
    snprintf(home, sizeof(home), "/home/%s", name);

    if (useradd(name, password, uid, uid, home) < 0) {
        printf("useradd: could not create '%s' (does it already exist?)\n", name);
        return 1;
    }
    memset(password, 0, sizeof(password));
    memset(again, 0, sizeof(again));

    /* The kernel may have chosen the uid, so read back what it actually is. */
    printf("created %s, home %s\n", name, home);
    return 0;
}
