#include <stdio.h>
#include <unistd.h>

int main(void)
{
    char buffer[128];
    if (getcwd(buffer, sizeof(buffer)) < 0) {
        printf("pwd: cannot read working directory\n");
        return 1;
    }
    printf("%s\n", buffer);
    return 0;
}
