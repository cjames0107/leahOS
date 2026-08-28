#include <cli.h>
#include <stdio.h>

int main(int argc, char** argv)
{
    /* Every argument is a word to print, including one that starts with a
     * dash, so nothing here is an option and the library is told so. */
    cli_begin(argc, argv, "[word...]", 0);

    for (int i = 1; i < argc; ++i) {
        printf("%s", argv[i]);
        if (i + 1 < argc)
            printf(" ");
    }
    printf("\n");
    return 0;
}
