#include <stdio.h>
#include <sys/stat.h>

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
    if (argc != 3) {
        printf("usage: chown <uid> <file>\n");
        return 1;
    }
    unsigned uid;
    if (parse_uint(argv[1], &uid) < 0) {
        printf("chown: bad uid '%s'\n", argv[1]);
        return 1;
    }
    /* -1 leaves the group alone. */
    if (chown(argv[2], uid, (unsigned)-1) < 0) {
        printf("chown: cannot change '%s' (are you root?)\n", argv[2]);
        return 1;
    }
    return 0;
}
