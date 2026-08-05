/* date - say what time it is.
 *
 * The clock is read from the CMOS once at boot and carried on the kernel's
 * tick, so this is as accurate as the machine's battery clock was and drifts
 * from there. There is nothing to set it from: no network time, and no way to
 * write the CMOS back.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char** argv)
{
    int utc = 0;
    const char* format = 0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-u") == 0) {
            utc = 1;
        } else if (argv[i][0] == '+') {
            format = argv[i] + 1;       /* the traditional +FORMAT */
        } else if (strcmp(argv[i], "-R") == 0) {
            format = "%a, %d %b %Y %H:%M:%S";
        } else {
            printf("usage: date [-u] [-R] [+format]\n");
            printf("  -u  UTC rather than local    -R  a fuller form\n");
            printf("  %%Y %%m %%d %%H %%M %%S %%a %%A %%b %%B %%e %%j %%p %%I %%F %%T %%s\n");
            return 2;
        }
    }

    const time_t now = time(0);
    struct tm t;
    if ((utc ? gmtime_r(&now, &t) : localtime_r(&now, &t)) == 0) {
        printf("date: cannot read the clock\n");
        return 1;
    }

    char line[128];
    if (format == 0) {
        /* The default form, with the offset named so that a reading taken here
         * and one taken elsewhere can be compared. */
        const long off = utc ? 0 : timezone_offset();
        strftime(line, sizeof(line), "%a %e %b %Y %H:%M:%S", &t);
        if (off == 0) {
            printf("%s UTC\n", line);
        } else {
            const long m = off < 0 ? -off / 60 : off / 60;
            printf("%s UTC%c%02ld:%02ld\n", line, off < 0 ? '-' : '+',
                   m / 60, m % 60);
        }
    } else {
        if (strftime(line, sizeof(line), format, &t) == 0) {
            printf("date: that format does not fit in %d characters\n",
                   (int)sizeof(line));
            return 1;
        }
        printf("%s\n", line);
    }
    return 0;
}
