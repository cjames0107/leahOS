/* Calendar arithmetic.
 *
 * The kernel hands out a count of seconds and nothing else, which is the right
 * thing for a kernel to know. Turning that into a date is arithmetic, and
 * arithmetic belongs here.
 *
 * The conversions are Howard Hinnant's civil-from-days and days-from-civil,
 * which handle the whole Gregorian rule - four, hundred, four hundred - by
 * shifting the year so that March is its first month. That puts the leap day
 * at the end of the year rather than in the middle, so no month before it has
 * to know whether the year is long, and the month lengths then repeat in a
 * pattern of five that a single division reproduces. It is a page of code
 * instead of a table, and it is correct for any year.
 */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

time_t time(time_t* out)
{
    struct timespec ts;
    if (clock_gettime(&ts) != 0) {
        if (out)
            *out = 0;
        return 0;
    }
    if (out)
        *out = ts.tv_sec;
    return ts.tv_sec;
}

int clock_gettime(struct timespec* out)
{
    int64_t pair[2] = { 0, 0 };
    if (out == 0)
        return -1;
    if (__syscall(SYS_clocktime, (long)pair, 0, 0, 0, 0) != 0)
        return -1;
    out->tv_sec = pair[0];
    out->tv_nsec = (long)pair[1];
    return 0;
}

/* --- the two conversions ---------------------------------------------------- */

static int64_t days_from_civil(int64_t year, unsigned month, unsigned day)
{
    year -= month <= 2;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const int64_t yoe = year - era * 400;
    const int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t days, int64_t* year, unsigned* month,
                            unsigned* day)
{
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const int64_t doe = days - era * 146097;                        /* 0..146096 */
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t y = yoe + era * 400;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);    /* 0..365 */
    const int64_t mp = (5 * doy + 2) / 153;                         /* 0..11 */
    const unsigned d = (unsigned)(doy - (153 * mp + 2) / 5 + 1);    /* 1..31 */
    const unsigned m = (unsigned)(mp + (mp < 10 ? 3 : -9));         /* 1..12 */
    *year = y + (m <= 2);
    *month = m;
    *day = d;
}

/* Floor division and the matching remainder. A plain / truncates toward zero,
 * which for a date before 1970 puts the day off by one and the time of day
 * negative - the one place this arithmetic is easy to get quietly wrong. */
static int64_t floor_div(int64_t a, int64_t b)
{
    const int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

static int64_t floor_mod(int64_t a, int64_t b)
{
    const int64_t r = a % b;
    return (r != 0 && ((r < 0) != (b < 0))) ? r + b : r;
}

struct tm* gmtime_r(const time_t* when, struct tm* out)
{
    if (when == 0 || out == 0)
        return 0;

    const int64_t seconds = *when;
    const int64_t days = floor_div(seconds, 86400);
    const int64_t rest = floor_mod(seconds, 86400);

    int64_t year;
    unsigned month, day;
    civil_from_days(days, &year, &month, &day);

    out->tm_sec  = (int)(rest % 60);
    out->tm_min  = (int)((rest / 60) % 60);
    out->tm_hour = (int)(rest / 3600);
    out->tm_mday = (int)day;
    out->tm_mon  = (int)month - 1;
    out->tm_year = (int)(year - 1900);
    /* 1970-01-01 was a Thursday, which is the 4 - and floor_mod keeps that
     * true for dates before it as well. */
    out->tm_wday = (int)floor_mod(days + 4, 7);
    out->tm_yday = (int)(days - days_from_civil(year, 1, 1));
    out->tm_isdst = 0;
    return out;
}

time_t timegm(struct tm* b)
{
    if (b == 0)
        return 0;
    /* The month is normalised first so that a month outside 0..11 rolls into
     * the year, which is what makes date arithmetic work by just adding. */
    int64_t year = (int64_t)b->tm_year + 1900;
    int64_t mon = b->tm_mon;
    year += floor_div(mon, 12);
    mon = floor_mod(mon, 12);

    const int64_t days = days_from_civil(year, (unsigned)mon + 1, 1) +
                         (b->tm_mday - 1);
    return days * 86400 + (int64_t)b->tm_hour * 3600 +
           (int64_t)b->tm_min * 60 + b->tm_sec;
}

/* --- the local offset -------------------------------------------------------- */

long timezone_offset(void)
{
    static int read_yet;
    static long offset;

    if (read_yet)
        return offset;
    read_yet = 1;

    /* A signed number of minutes, on its own. Not a zone name: naming a zone
     * means carrying the table that says what the zone did in 1987, and this
     * system does not have one. A file that is missing means UTC. */
    const int fd = open("/etc/timezone", O_RDONLY);
    if (fd < 0)
        return 0;
    char text[32];
    const long n = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    text[n] = '\0';

    int sign = 1, i = 0;
    if (text[0] == '-') { sign = -1; i = 1; }
    else if (text[0] == '+') { i = 1; }
    long minutes = 0;
    for (; text[i] >= '0' && text[i] <= '9'; ++i)
        minutes = minutes * 10 + (text[i] - '0');
    if (minutes > 24 * 60)
        return 0;                       /* nonsense; stay on UTC */
    offset = sign * minutes * 60;
    return offset;
}

struct tm* localtime_r(const time_t* when, struct tm* out)
{
    if (when == 0)
        return 0;
    const time_t shifted = *when + timezone_offset();
    return gmtime_r(&shifted, out);
}

/* --- formatting -------------------------------------------------------------- */

static const char* const kDayShort[] =
    { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char* const kDayLong[] =
    { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
      "Saturday" };
static const char* const kMonShort[] =
    { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static const char* const kMonLong[] =
    { "January", "February", "March", "April", "May", "June", "July",
      "August", "September", "October", "November", "December" };

struct sink { char* out; size_t max, len; };

static void put(struct sink* s, char c)
{
    if (s->len + 1 < s->max)
        s->out[s->len] = c;
    ++s->len;
}

static void put_str(struct sink* s, const char* text)
{
    while (*text != '\0')
        put(s, *text++);
}

static void put_number(struct sink* s, long value, int width, char pad)
{
    char digits[24];
    int n = 0;
    int negative = value < 0;
    unsigned long v = negative ? (unsigned long)(-(value + 1)) + 1
                               : (unsigned long)value;
    do {
        digits[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v != 0);
    if (negative)
        digits[n++] = '-';
    for (int i = n; i < width; ++i)
        put(s, pad);
    while (n > 0)
        put(s, digits[--n]);
}

size_t strftime(char* out, size_t max, const char* format, const struct tm* t)
{
    struct sink s = { out, max, 0 };
    if (out == 0 || max == 0 || t == 0)
        return 0;

    /* Guarded so that an out-of-range field cannot index a name table off the
     * end - a struct tm arrives from callers, and one of them will be wrong. */
    const int wday = (t->tm_wday >= 0 && t->tm_wday < 7) ? t->tm_wday : 0;
    const int mon  = (t->tm_mon  >= 0 && t->tm_mon  < 12) ? t->tm_mon : 0;

    for (const char* f = format; *f != '\0'; ++f) {
        if (*f != '%') {
            put(&s, *f);
            continue;
        }
        switch (*++f) {
        case '\0': --f; break;
        case 'Y': put_number(&s, t->tm_year + 1900, 0, '0'); break;
        case 'y': put_number(&s, (t->tm_year + 1900) % 100, 2, '0'); break;
        case 'm': put_number(&s, t->tm_mon + 1, 2, '0'); break;
        case 'd': put_number(&s, t->tm_mday, 2, '0'); break;
        case 'e': put_number(&s, t->tm_mday, 2, ' '); break;
        case 'H': put_number(&s, t->tm_hour, 2, '0'); break;
        case 'M': put_number(&s, t->tm_min, 2, '0'); break;
        case 'S': put_number(&s, t->tm_sec, 2, '0'); break;
        case 'j': put_number(&s, t->tm_yday + 1, 3, '0'); break;
        case 'a': put_str(&s, kDayShort[wday]); break;
        case 'A': put_str(&s, kDayLong[wday]); break;
        case 'b': case 'h': put_str(&s, kMonShort[mon]); break;
        case 'B': put_str(&s, kMonLong[mon]); break;
        case 'p': put_str(&s, t->tm_hour < 12 ? "AM" : "PM"); break;
        case 'I': {
            int h = t->tm_hour % 12;
            put_number(&s, h == 0 ? 12 : h, 2, '0');
            break;
        }
        case 'F':
            put_number(&s, t->tm_year + 1900, 0, '0'); put(&s, '-');
            put_number(&s, t->tm_mon + 1, 2, '0');     put(&s, '-');
            put_number(&s, t->tm_mday, 2, '0');
            break;
        case 'T':
            put_number(&s, t->tm_hour, 2, '0'); put(&s, ':');
            put_number(&s, t->tm_min, 2, '0');  put(&s, ':');
            put_number(&s, t->tm_sec, 2, '0');
            break;
        case 's': {
            struct tm copy = *t;
            put_number(&s, (long)timegm(&copy), 0, '0');
            break;
        }
        case '%': put(&s, '%'); break;
        default:
            /* An unknown conversion is printed as written rather than eaten,
             * so a typo in a format string is visible. */
            put(&s, '%');
            put(&s, *f);
            break;
        }
    }

    if (s.len >= max) {
        out[max - 1] = '\0';
        return 0;                       /* it did not fit, and says so */
    }
    out[s.len] = '\0';
    return s.len;
}

char* ctime_r(const time_t* when, char* out)
{
    struct tm t;
    if (when == 0 || out == 0 || localtime_r(when, &t) == 0)
        return 0;
    strftime(out, 26, "%a %b %e %H:%M:%S %Y\n", &t);
    return out;
}

unsigned long uptime_ms(void)
{
    return (unsigned long)__syscall(SYS_uptime, 0, 0, 0, 0, 0);
}
