#ifndef _TIME_H
#define _TIME_H

#include <stdint.h>
#include <stddef.h>

/* What time it is.
 *
 * Seconds since the start of 1970, which is how UNIX has counted since it
 * started counting. A 64-bit count, so the 2038 problem is somebody else's.
 *
 * The clock is read from the CMOS once at boot and carried forward on the
 * kernel's tick, so it is monotonic: it cannot jump backwards, and it drifts
 * with whatever the timer drifts with. There is no NTP and nothing to set it
 * from, so the accuracy is the accuracy of the machine's battery clock.
 *
 * Everything here is UTC. Time zones are a database - which offset applied in
 * which place in which year, and every political decision since 1970 - and
 * this system does not carry one. `localtime` is `gmtime` with the offset in
 * /etc/timezone applied, which is a fixed number of minutes and therefore
 * wrong about daylight saving. Saying so is better than pretending.
 */

typedef int64_t time_t;

struct timespec {
    time_t   tv_sec;
    long     tv_nsec;
};

/* Broken-down time, in the fields C has always used - including the two
 * famously awkward ones: tm_year counts from 1900 and tm_mon from zero. */
struct tm {
    int tm_sec;     /* 0..60, the 60 being a leap second */
    int tm_min;     /* 0..59 */
    int tm_hour;    /* 0..23 */
    int tm_mday;    /* 1..31 */
    int tm_mon;     /* 0..11 */
    int tm_year;    /* years since 1900 */
    int tm_wday;    /* 0..6, Sunday first */
    int tm_yday;    /* 0..365 */
    int tm_isdst;   /* always 0: there is no daylight saving here */
};

/* Now. `out` may be null, which is the form everyone actually uses. */
time_t time(time_t* out);

/* Now, to the nanosecond the tick can offer - which is ten milliseconds, so
 * the low digits are always zero and this exists for the shape rather than the
 * resolution. Returns 0, or -1. */
int clock_gettime(struct timespec* out);

/* Seconds to a calendar date and back. Reentrant forms only: the ones that
 * return a pointer to a static are a trap, and this system has threads. */
struct tm* gmtime_r(const time_t* when, struct tm* out);
struct tm* localtime_r(const time_t* when, struct tm* out);

/* A calendar date to seconds. The fields need not be in range - November 32nd
 * is December 2nd - which is what makes this useful for arithmetic. */
time_t timegm(struct tm* broken);

/* The offset local time is ahead of UTC, in seconds. Read once from
 * /etc/timezone, which holds a signed number of minutes and nothing else. */
long timezone_offset(void);

/* Formatting. A subset of strftime's conversions, and it says which:
 *
 *     %Y %m %d   year, month, day, zero-padded
 *     %H %M %S   hour, minute, second
 *     %y         two-digit year
 *     %b %B      month name, short and long
 *     %a %A      weekday name, short and long
 *     %e         day of month, space-padded
 *     %j         day of year
 *     %p         AM or PM
 *     %I         hour on a twelve-hour clock
 *     %F         the same as %Y-%m-%d
 *     %T         the same as %H:%M:%S
 *     %s         seconds since 1970
 *     %%         a percent sign
 *
 * Returns the length written, or 0 if it would not fit. */
size_t strftime(char* out, size_t max, const char* format, const struct tm* t);

/* "Mon Aug  4 21:03:17 2026", the traditional 24-character line plus a
 * newline. Into a caller's buffer of at least 26 bytes. */
char* ctime_r(const time_t* when, char* out);

#endif /* _TIME_H */
