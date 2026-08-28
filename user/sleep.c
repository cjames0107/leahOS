/* Wait, and do nothing else.
 *
 * The smallest useful program there is, and the one job control cannot be
 * demonstrated without: suspending and resuming a program needs a program that
 * is still there a second later.
 *
 * Fractions are accepted because the underlying call takes milliseconds and
 * throwing that away to match a historical integer-only sleep would be a
 * pointless loss.
 */

#include <cli.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char** argv)
{
    cli_begin(argc, argv, "SECONDS...", "");
    if (cli_argc() < 1)
        cli_usage();

    unsigned long total = 0;
    for (int i = 0; i < cli_argc(); ++i) {
        const char* start = cli_arg(i);
        const char* text = start;
        unsigned long ms = 0;
        int digits = 0;

        while (*text >= '0' && *text <= '9') {
            ms = ms * 10 + (unsigned long)(*text++ - '0');
            ++digits;
        }
        ms *= 1000;
        if (*text == '.' || *text == ',') {
            ++text;
            /* Three places and no more; a fourth would be below what the call
             * underneath can express anyway. */
            unsigned long scale = 100;
            while (*text >= '0' && *text <= '9' && scale > 0) {
                ms += (unsigned long)(*text++ - '0') * scale;
                scale /= 10;
                ++digits;
            }
            while (*text >= '0' && *text <= '9')
                ++text;
        }
        if (digits == 0 || *text != '\0') {
            cli_fail("%s: not a number of seconds", start);
            return 1;
        }
        total += ms;
    }

    msleep(total);
    return 0;
}
