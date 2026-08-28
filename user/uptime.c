/* How long the machine has been up, what time it thinks it is, and how busy
 * it has been.
 *
 * The load average is a decaying count of runnable tasks. The scheduler keeps
 * it now - sampled every five seconds and decayed towards each sample over
 * one, five and fifteen minutes, with the constants every other UNIX uses - so
 * the three numbers here mean what they mean everywhere.
 */

#include <cli.h>
#include <proc.h>
#include <stdio.h>
#include <time.h>

int main(int argc, char** argv)
{
    cli_begin(argc, argv, "", "");
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
        printf("%lu:%02lu,  ", hours, minutes);
    else
        printf("%lu min,  ", minutes);

    unsigned long load[3] = { 0, 0, 0 };
    load_average(load);
    printf("load average: %lu.%02lu, %lu.%02lu, %lu.%02lu\n",
           load[0] / 100, load[0] % 100,
           load[1] / 100, load[1] % 100,
           load[2] / 100, load[2] % 100);
    return 0;
}
