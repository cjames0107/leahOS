#include <leah/clock.hpp>
#include <leah/console.hpp>
#include <leah/io.hpp>
#include <leah/timer.hpp>

namespace clock {
namespace {

constexpr u16 kAddress = 0x70;
constexpr u16 kData    = 0x71;

// CMOS registers, from the original PC/AT.
constexpr u8 kSeconds = 0x00;
constexpr u8 kMinutes = 0x02;
constexpr u8 kHours   = 0x04;
constexpr u8 kDay     = 0x07;
constexpr u8 kMonth   = 0x08;
constexpr u8 kYear    = 0x09;
constexpr u8 kStatusA = 0x0A;
constexpr u8 kStatusB = 0x0B;
constexpr u8 kCentury = 0x32;   // where ACPI says it is, when it is anywhere

constexpr u8 kUpdateInProgress = 0x80;   // status A
constexpr u8 kBinaryMode       = 0x04;   // status B
constexpr u8 kTwentyFourHour   = 0x02;   // status B

i64 g_epoch_at_boot;
u64 g_uptime_at_boot_ms;
bool g_from_hardware;

u8 read_register(u8 reg)
{
    // The high bit of the address port is the NMI mask. Leaving it clear the
    // whole time would re-enable NMIs as a side effect of reading a clock,
    // which is not this code's business, so whatever it was is preserved.
    const u8 nmi = static_cast<u8>(io::in8(kAddress) & 0x80);
    io::out8(kAddress, static_cast<u8>(nmi | reg));
    return io::in8(kData);
}

u8 from_bcd(u8 value)
{
    return static_cast<u8>((value & 0x0F) + ((value >> 4) * 10));
}

// Days from 1970-01-01 to the given date. The civil-from-days algorithm run
// backwards: shift the year so that March is the first month, which makes the
// leap day the last day of the year and the month-length pattern repeat every
// five months.
i64 days_from_civil(i64 year, unsigned month, unsigned day)
{
    year -= month <= 2;
    const i64 era = (year >= 0 ? year : year - 399) / 400;
    const i64 year_of_era = year - era * 400;                       // 0..399
    const i64 day_of_year =
        (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;   // 0..365
    const i64 day_of_era =
        year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + day_of_era - 719468;
}

}  // namespace

void init()
{
    // The clock updates once a second and the registers are not coherent while
    // it does. Wait for the update to finish, then read; then read again and
    // accept the answer only when two consecutive readings agree, which is the
    // standard way to avoid catching a value mid-carry - 01:59:59 becoming
    // 01:00:00 because the minutes were read before the roll and the seconds
    // after.
    u8 second = 0, minute = 0, hour = 0, day = 0, month = 0, year = 0, century = 0;
    for (int attempt = 0; attempt < 100; ++attempt) {
        while (read_register(kStatusA) & kUpdateInProgress)
            ;
        const u8 s = read_register(kSeconds);
        const u8 mi = read_register(kMinutes);
        const u8 h = read_register(kHours);
        const u8 d = read_register(kDay);
        const u8 mo = read_register(kMonth);
        const u8 y = read_register(kYear);
        const u8 c = read_register(kCentury);

        while (read_register(kStatusA) & kUpdateInProgress)
            ;
        if (s == read_register(kSeconds) && mi == read_register(kMinutes) &&
            h == read_register(kHours) && d == read_register(kDay) &&
            mo == read_register(kMonth) && y == read_register(kYear)) {
            second = s; minute = mi; hour = h;
            day = d; month = mo; year = y; century = c;
            break;
        }
    }

    const u8 status_b = read_register(kStatusB);
    if ((status_b & kBinaryMode) == 0) {
        // Packed decimal, which is the default and what QEMU presents.
        second = from_bcd(second);
        minute = from_bcd(minute);
        day    = from_bcd(day);
        month  = from_bcd(month);
        year   = from_bcd(year);
        century = from_bcd(century);
        // The hour's high bit is the PM flag in 12-hour mode and has to be
        // taken off before the digits are unpacked, or midnight reads as 92.
        hour = static_cast<u8>(from_bcd(static_cast<u8>(hour & 0x7F)) |
                               (hour & 0x80));
    }
    if ((status_b & kTwentyFourHour) == 0 && (hour & 0x80) != 0) {
        // 12-hour mode, afternoon. Noon is 12 PM and stays 12; everything else
        // adds twelve.
        hour = static_cast<u8>(((hour & 0x7F) % 12) + 12);
    }

    i64 full_year = year;
    if (century >= 19 && century <= 25)
        full_year += century * 100;
    else
        full_year += (year < 70) ? 2000 : 1900;   // the usual pivot

    const bool believable = month >= 1 && month <= 12 && day >= 1 && day <= 31 &&
                            hour < 24 && minute < 60 && second < 60 &&
                            full_year >= 1970 && full_year <= 2200;

    if (believable) {
        const i64 days = days_from_civil(full_year, month, day);
        g_epoch_at_boot = days * 86400 + hour * 3600 + minute * 60 + second;
        g_from_hardware = true;
    } else {
        g_epoch_at_boot = 0;
        g_from_hardware = false;
    }
    g_uptime_at_boot_ms = timer::uptime_ms();
}

Time now()
{
    const u64 up = timer::uptime_ms();
    const u64 since_boot = up > g_uptime_at_boot_ms ? up - g_uptime_at_boot_ms : 0;
    Time t;
    t.seconds = g_epoch_at_boot + static_cast<i64>(since_boot / 1000);
    t.nanoseconds = static_cast<u32>((since_boot % 1000) * 1000000u);
    return t;
}

i64 now_seconds() { return now().seconds; }

bool from_hardware() { return g_from_hardware; }

}  // namespace clock
