#include <stdio.h>
#include <sys/stat.h>

/* Octal only: the symbolic form (u+rw) is a parser for little gain here. */
static int parse_octal(const char* text, unsigned* out)
{
    unsigned value = 0;
    if (*text == '\0')
        return -1;
    for (; *text != '\0'; ++text) {
        if (*text < '0' || *text > '7')
            return -1;
        value = value * 8 + (unsigned)(*text - '0');
    }
    if (value > 0777)
        return -1;
    *out = value;
    return 0;
}

int main(int argc, char** argv)
{
    if (argc != 3) {
        printf("usage: chmod <octal-mode> <file>\n");
        return 1;
    }
    unsigned mode;
    if (parse_octal(argv[1], &mode) < 0) {
        printf("chmod: bad mode '%s'\n", argv[1]);
        return 1;
    }
    if (chmod(argv[2], mode) < 0) {
        printf("chmod: cannot change '%s'\n", argv[2]);
        return 1;
    }
    return 0;
}
