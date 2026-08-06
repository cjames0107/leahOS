/* How long the machine has been up, and what time it thinks it is.
 *
 * No load average. A load average is a decaying count of runnable tasks kept
 * by the scheduler, and this one does not keep it - printing a made-up number
 * in the place people read one from would be worse than leaving the space.
 */

#include <stdio.h>
#include <time.h>

int main(void)
{
    const unsigned long ms = uptime_ms();
    const unsigned long seconds = ms / 1000;
    const unsigned long days = seconds / 86400;
    const unsigned long hours = (seconds / 3600) % 24;
    const unsigned long minutes = (seconds / 60) % 60;

    char when[32] = "";
    const time_t now = time(0);
    struct tm t;
    if (localtime_r(&now, &t) != 0)
        strftime(when, sizeof(when), "%H:%M:%S", &t);

    printf("%s  up ", when);
    if (days > 0)
        printf("%lu day%s, ", days, days == 1 ? "" : "s");
    if (hours > 0)
        printf("%lu:%02lu\n", hours, minutes);
    else
        printf("%lu min\n", minutes);
    return 0;
}
