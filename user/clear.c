#include <cli.h>
#include <unistd.h>

int main(int argc, char** argv)
{
    cli_begin(argc, argv, "", "");
    /* ESC[2J erases the screen, ESC[H homes the cursor - the console
     * understands both. */
    write(1, "\x1b[2J\x1b[H", 7);
    return 0;
}
