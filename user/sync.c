/* sync - write out what the filesystem is holding back. */

#include <unistd.h>

int main(void)
{
    sync();
    return 0;
}
