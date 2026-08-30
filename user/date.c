/* date - say what time it is.
 *
 * The clock is read from the CMOS once at boot and carried on the kernel's
 * tick, so this is as accurate as the machine's battery clock was and drifts
 * from there. There is nothing to set it from: no network time, and no way to
 * write the CMOS back.
 */

#include <cli.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char** argv)
{
    cli_begin(argc, argv,
              "[-u] [-R] [+format]\n"
              "  -u  UTC rather than local    -R  a fuller form\n"
              "  %Y %m %d %H %M %S %a %A %b %B %e %j %p %I %F %T %s",
              "uR");
    const int utc = cli_flag("-u");
    const char* format = cli_flag("-R") ? "%a, %d %b %Y %H:%M:%S" : 0;

    /* The traditional +FORMAT, which is not an option and so arrives here as
     * an ordinary argument. */
    for (int i = 0; i < cli_argc(); ++i) {
        if (cli_arg(i)[0] != '+')
            cli_usage();
        format = cli_arg(i) + 1;
    }

    const time_t now = time(0);
    struct tm t;
    if ((utc ? gmtime_r(&now, &t) : localtime_r(&now, &t)) == 0) {
        cli_fail("cannot read the clock");
        return 1;
    }

    char line[128];
    if (format == 0) {
        /* The default form, with the offset named so that a reading taken here
         * and one taken elsewhere can be compared. */
        const long off = utc ? 0 : timezone_offset();
        strftime(line, sizeof(line), "%a %e %b %Y %H:%M:%S", &t);
        /* What the zone calls itself, when it says: "CDT" rather than
         * "UTC-05:00" is the same fact told the way people tell it. A zone
         * that has no name for itself - a bare numeric offset - falls back to
         * the number, which is all there is to say about it. */
        char zone[16];
        if (!utc && timezone_name(now, zone, sizeof(zone)) == 0) {
            printf("%s %s\n", line, zone);
        } else if (off == 0) {
            printf("%s UTC\n", line);
        } else {
            const long m = off < 0 ? -off / 60 : off / 60;
            printf("%s UTC%c%02ld:%02ld\n", line, off < 0 ? '-' : '+',
                   m / 60, m % 60);
        }
    } else {
        if (strftime(line, sizeof(line), format, &t) == 0) {
            cli_fail("that format does not fit in %d characters",
                     (int)sizeof(line));
            return 1;
        }
        printf("%s\n", line);
    }
    return 0;
}
