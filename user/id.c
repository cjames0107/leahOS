#include <stdio.h>
#include <unistd.h>

int main(void)
{
    const unsigned uid = getuid();
    printf("uid=%u(%s) gid=%u\n", uid, uid == 0 ? "root" : "user", getgid());
    return 0;
}
