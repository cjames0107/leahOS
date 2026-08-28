/* sync - write out what the filesystem is holding back. */

#include <cli.h>
#include <unistd.h>

int main(int argc, char** argv)
{
    cli_begin(argc, argv, "", "");
    sync();
    return 0;
}
