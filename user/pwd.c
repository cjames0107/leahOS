#include <cli.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char** argv)
{
    cli_begin(argc, argv, "", "");
    char buffer[128];
    if (getcwd(buffer, sizeof(buffer)) < 0)
        return cli_fail("cannot read working directory");
    printf("%s\n", buffer);
    return 0;
}
