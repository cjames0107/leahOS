#include <cli.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char** argv)
{
    cli_begin(argc, argv, "", "");
    const unsigned uid = getuid();
    char name[32];
    if (username(uid, name) == 0)
        printf("%s\n", name);
    else
        printf("uid %u\n", uid);        /* an account with no entry */
    return 0;
}
