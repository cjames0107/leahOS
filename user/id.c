#include <stdio.h>
#include <unistd.h>

int main(void)
{
    const unsigned uid = getuid();
    char name[32];
    if (username(uid, name) != 0)
        name[0] = 0;
    if (name[0] != 0)
        printf("uid=%u(%s) gid=%u\n", uid, name, getgid());
    else
        printf("uid=%u gid=%u\n", uid, getgid());
    return 0;
}
