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
#include <sys/stat.h>
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

/* --- the local offset -------------------------------------------------------
 *
 * A zone is a file, and the file is the one every UNIX uses: /etc/localtime, a
 * copy of a compiled zone from /usr/share/zoneinfo. That is what makes this a
 * real time zone rather than a number of minutes - a number cannot know that
 * New York is five hours behind in January and four in July, and every
 * timestamp shown between March and November would be an hour out.
 *
 * The format is TZif (RFC 8536): a header, a list of the instants at which the
 * offset changed, and the offsets themselves. A version 2 file carries the
 * whole thing twice - once with 32-bit instants for readers written before
 * 2038 was a concern, and again with 64-bit ones. The second block is the one
 * read here; the first is skipped, which is most of the fiddly part below.
 *
 * A file that is missing or unreadable falls back to /etc/timezone read as a
 * signed number of minutes, which is what this system had before and what is
 * still written when somebody sets an offset by hand.
 */

#define TZ_MAX_TIMES 512        /* transitions kept, newest first if it spills */
#define TZ_MAX_TYPES 24

struct tz_type { int32_t offset; unsigned char is_dst; unsigned char name; };

static struct {
    int      loaded;            /* 0 not tried, 1 have a zone, -1 no file */
    int      count;
    int64_t  when[TZ_MAX_TIMES];
    unsigned char which[TZ_MAX_TIMES];
    struct tz_type type[TZ_MAX_TYPES];
    int      types;
    char     names[64];         /* "EST\0EDT\0", indexed by tz_type.name */
    long     fallback;          /* /etc/timezone, when there is no zone file */
    int64_t  stamp;             /* the file's mtime when it was read */
    unsigned long checked_at;   /* uptime when the file was last looked at */
} g_zone;

static uint32_t be32(const unsigned char* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int64_t be64(const unsigned char* p)
{
    return (int64_t)(((uint64_t)be32(p) << 32) | (uint64_t)be32(p + 4));
}

/* The signed number of minutes in /etc/timezone, or 0. What this system used
 * before zones were files, and still the answer when there is no zone file. */
static long read_minutes(void)
{
    const int fd = open("/etc/timezone", O_RDONLY);
    if (fd < 0)
        return 0;
    char text[32];
    const long n = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    text[n] = '\0';
    /* A zone name rather than a number is the normal case now - the name is
     * what /etc/localtime was copied from - and it is not an offset. */
    int sign = 1, i = 0;
    if (text[0] == '-') { sign = -1; i = 1; }
    else if (text[0] == '+') { i = 1; }
    if (text[i] < '0' || text[i] > '9')
        return 0;
    long minutes = 0;
    for (; text[i] >= '0' && text[i] <= '9'; ++i)
        minutes = minutes * 10 + (text[i] - '0');
    if (minutes > 24 * 60)
        return 0;                       /* nonsense; stay on UTC */
    return sign * minutes * 60;
}

/* One TZif data block's worth of counts. */
struct tz_counts { uint32_t isut, isstd, leap, times, types, chars; };

static void tz_counts(const unsigned char* h, struct tz_counts* c)
{
    c->isut  = be32(h + 20);
    c->isstd = be32(h + 24);
    c->leap  = be32(h + 28);
    c->times = be32(h + 32);
    c->types = be32(h + 36);
    c->chars = be32(h + 40);
}

/* How many bytes a data block takes, at this instant size. */
static unsigned long tz_block(const struct tz_counts* c, int wide)
{
    const unsigned long stamp = wide ? 8 : 4;
    return (unsigned long)c->times * (stamp + 1) +
           (unsigned long)c->types * 6 +
           (unsigned long)c->chars +
           (unsigned long)c->leap * (stamp + 4) +
           (unsigned long)c->isstd + (unsigned long)c->isut;
}

/* Read /etc/localtime into g_zone. Returns 1 when there is a zone to use. */
static int load_zone(void)
{
    static unsigned char file[16384];
    const int fd = open("/etc/localtime", O_RDONLY);
    if (fd < 0)
        return 0;
    long len = 0, n;
    while ((n = read(fd, file + len, (unsigned long)(sizeof(file) - (unsigned long)len))) > 0)
        len += n;
    close(fd);
    if (len < 44 || file[0] != 'T' || file[1] != 'Z' || file[2] != 'i' ||
        file[3] != 'f')
        return 0;

    const int version = file[4];
    struct tz_counts c;
    tz_counts(file, &c);

    unsigned long at = 44;
    int wide = 0;
    if (version >= '2') {
        /* Step over the 32-bit block and its header, and read the 64-bit one -
         * which is the same zone described again without the year 2038 in it. */
        at += tz_block(&c, 0);
        if (at + 44 > (unsigned long)len)
            return 0;
        tz_counts(file + at, &c);
        at += 44;
        wide = 1;
    }
    if (c.types == 0 || at + tz_block(&c, wide) > (unsigned long)len)
        return 0;

    /* Only the transitions that will be asked about. A zone has a couple of
     * hundred and this keeps five, which is every one of them for every zone
     * there is - but if a file ever carried more, the recent ones are the ones
     * worth having and the oldest are dropped. */
    unsigned long skip = 0;
    int keep = (int)c.times;
    if (keep > TZ_MAX_TIMES) {
        skip = (unsigned long)(keep - TZ_MAX_TIMES);
        keep = TZ_MAX_TIMES;
    }
    const unsigned long stamp = wide ? 8u : 4u;
    const unsigned char* times = file + at;
    const unsigned char* index = times + (unsigned long)c.times * stamp;
    const unsigned char* infos = index + c.times;

    g_zone.count = keep;
    for (int i = 0; i < keep; ++i) {
        const unsigned char* p = times + (skip + (unsigned long)i) * stamp;
        g_zone.when[i] = wide ? be64(p) : (int32_t)be32(p);
        g_zone.which[i] = index[skip + (unsigned long)i];
    }

    g_zone.types = (int)(c.types > TZ_MAX_TYPES ? TZ_MAX_TYPES : c.types);
    for (int i = 0; i < g_zone.types; ++i) {
        g_zone.type[i].offset = (int32_t)be32(infos + (unsigned long)i * 6);
        g_zone.type[i].is_dst = infos[(unsigned long)i * 6 + 4];
        g_zone.type[i].name   = infos[(unsigned long)i * 6 + 5];
    }

    /* The designations - "EST", "EDT" - as one run of terminated strings that
     * the types index into. What lets a date say CDT rather than UTC-05:00,
     * which is the same fact told the way nobody says it. */
    const unsigned char* chars = infos + (unsigned long)c.types * 6;
    unsigned long keep_chars = c.chars;
    if (keep_chars > sizeof(g_zone.names) - 1)
        keep_chars = sizeof(g_zone.names) - 1;
    for (unsigned long i = 0; i < keep_chars; ++i)
        g_zone.names[i] = (char)chars[i];
    g_zone.names[keep_chars] = '\0';
    return 1;
}

/* Look the zone up again when the file underneath has changed, and at most
 * once a second: the offset used to be read once and kept for the life of the
 * process, which is right for `date` and wrong for a clock on the desktop -
 * and `ls -l` over a thousand files must still cost one open, not a thousand. */
static void refresh_zone(void)
{
    const unsigned long now = uptime_ms();
    if (g_zone.loaded != 0 && now - g_zone.checked_at < 1000)
        return;
    g_zone.checked_at = now;

    struct stat st;
    const int64_t stamp = stat("/etc/localtime", &st) == 0
                        ? st.st_mtime + (int64_t)st.st_size : 0;
    if (g_zone.loaded != 0 && stamp == g_zone.stamp)
        return;
    g_zone.stamp = stamp;
    g_zone.count = 0;
    g_zone.types = 0;
    g_zone.loaded = load_zone() ? 1 : -1;
    if (g_zone.loaded < 0)
        g_zone.fallback = read_minutes();
}

static int type_at(time_t when);

/* The offset in force at `when`. */
static long offset_at(time_t when)
{
    const int t = type_at(when);
    return t < 0 ? g_zone.fallback : g_zone.type[t].offset;
}

/* The type in force at `when`, or -1. The offset and the name both want it, so
 * the search that finds it is written once. */
static int type_at(time_t when)
{
    refresh_zone();
    if (g_zone.loaded < 0 || g_zone.types == 0)
        return -1;

    /* The last transition at or before this instant. Binary search, because a
     * zone has a couple of hundred of them and this is called once per file in
     * a long listing. */
    int lo = 0, hi = g_zone.count - 1, found = -1;
    while (lo <= hi) {
        const int mid = lo + (hi - lo) / 2;
        if (g_zone.when[mid] <= (int64_t)when) { found = mid; lo = mid + 1; }
        else                                    { hi = mid - 1; }
    }
    if (found < 0) {
        /* Before the first transition the file describes. The first type that
         * is not daylight saving is what every reader uses here, because a
         * zone's first entry is often the local mean time it kept before the
         * zone existed. */
        for (int i = 0; i < g_zone.types; ++i)
            if (!g_zone.type[i].is_dst)
                return i;
        return 0;
    }
    const unsigned char which = g_zone.which[found];
    return which < (unsigned char)g_zone.types ? (int)which : -1;
}

int timezone_name(time_t when, char* out, int max)
{
    if (out == 0 || max <= 0)
        return -1;
    out[0] = '\0';
    const int t = type_at(when);
    if (t < 0)
        return -1;
    const unsigned char at = g_zone.type[t].name;
    if (at >= sizeof(g_zone.names))
        return -1;
    const char* name = &g_zone.names[at];
    if (name[0] == '\0')
        return -1;
    int i = 0;
    while (name[i] != '\0' && i < max - 1) { out[i] = name[i]; ++i; }
    out[i] = '\0';
    return 0;
}

long timezone_offset(void)
{
    /* The offset now, which is what a caller asking without saying when
     * means. Anything converting a particular instant goes through
     * localtime_r, which asks about that instant instead - the difference is
     * an hour, twice a year, and it is the difference between a file listing
     * that is right and one that is right for half the year. */
    return offset_at(time(0));
}

struct tm* localtime_r(const time_t* when, struct tm* out)
{
    if (when == 0)
        return 0;
    /* The offset in force at *that* instant, not the one in force now. A
     * listing made in July showing a file written in January must show the
     * January time - asking for today's offset would put every winter
     * timestamp an hour out for half the year. */
    const time_t shifted = *when + offset_at(*when);
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
