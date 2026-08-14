/* calendar - the month, as a grid.
 *
 * There is no appointment store on this system and this does not pretend to be
 * one: a calendar that offers to remember something and then forgets it at the
 * next boot is worse than one that only ever shows the date. What it does show
 * it gets from the clock, which means the grid is right whenever the clock is.
 *
 * The month is built from first principles rather than from a table. Zeller and
 * friends give the weekday of a date directly, but the library already turns a
 * time_t into a struct tm with tm_wday filled in, and asking it about noon on
 * the first of the month is both shorter and impossible to get wrong by one.
 */

#include <app.h>
#include <dialog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <widget.h>
#include <window.h>


/* What is being looked at, and what is actually today - the two are the same
 * until somebody presses an arrow, and the difference is what lets today keep
 * its mark while a different month is on screen. */
static int g_year, g_month;             /* month is 0..11 */
static int g_today_year, g_today_month, g_today_day;

static const char* const kMonths[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};
static const char* const kDays[7] = { "S", "M", "T", "W", "T", "F", "S" };

static int leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static int days_in(int year, int month)
{
    static const int kLength[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    return month == 1 && leap(year) ? 29 : kLength[month];
}

/* Which weekday the first of the month falls on, 0 = Sunday.
 *
 * Asked of the library rather than computed here: timegm turns the broken-down
 * date back into a time_t, and gmtime_r fills in tm_wday on the way out. Noon
 * rather than midnight so that no arithmetic anywhere can land the moment on
 * the previous day. */
static int first_weekday(int year, int month)
{
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = year - 1900;
    t.tm_mon  = month;
    t.tm_mday = 1;
    t.tm_hour = 12;
    const time_t when = timegm(&t);
    struct tm back;
    if (gmtime_r(&when, &back) == 0)
        return 0;
    return back.tm_wday;
}

static void read_today(void)
{
    const time_t now = time(0);
    struct tm t;
    if (localtime_r(&now, &t) == 0)
        return;
    g_today_year  = t.tm_year + 1900;
    g_today_month = t.tm_mon;
    g_today_day   = t.tm_mday;
}

static void go_today(void)
{
    read_today();
    g_year = g_today_year;
    g_month = g_today_month;
}

static void step(int months)
{
    int m = g_month + months;
    while (m < 0)   { m += 12; --g_year; }
    while (m > 11)  { m -= 12; ++g_year; }
    g_month = m;
}

/* The grid's geometry, worked out once so that drawing and hit-testing cannot
 * disagree about where a cell is. */
#define HEAD_H 76
static struct app* g_app;
static unsigned g_w(void) { return g_app->w; }
static unsigned g_h(void) { return g_app->h; }

static int cell_w(void) { return ((int)g_w() - 32) / 7; }
static int cell_h(void) { return ((int)g_h() - HEAD_H - 16) / 6; }
static int grid_x(void) { return 16; }
static int grid_y(void) { return HEAD_H; }

/* The three controls in the header, as one conjoined pill each side of the
 * title - previous, today, next - because they are the same kind of action. */
static int pill_x(void) { return (int)g_w() - 16 - 3 * 44; }
static int pill_y(void) { return 30; }
#define PILL_W 44
#define PILL_H 26

static void draw(struct app* a)
{
    (void)a;
    wg_theme();
    wg_glass_clear();

    char line[64];
    snprintf(line, sizeof(line), "%s %d", kMonths[g_month], g_year);
    wg_text(16, 24, line, WG_INK);

    static const char* const kPills[3] = { "<", "Today", ">" };
    wg_pill_group(pill_x(), pill_y(), PILL_W, PILL_H, 3, kPills, -1);

    /* Weekday initials, dim, above the grid. */
    for (int d = 0; d < 7; ++d) {
        const int x = grid_x() + d * cell_w();
        wg_text(x + (cell_w() - WG_GLYPH_W) / 2, HEAD_H - 20, kDays[d], WG_DIM);
    }

    wg_container(grid_x() - 6, grid_y() - 6, 7 * cell_w() + 12,
                 6 * cell_h() + 12, 6);

    const int first = first_weekday(g_year, g_month);
    const int count = days_in(g_year, g_month);
    for (int day = 1; day <= count; ++day) {
        const int slot = first + day - 1;
        const int col = slot % 7, row = slot / 7;
        if (row >= 6)
            break;                      /* a month never needs a seventh row */
        const int x = grid_x() + col * cell_w();
        const int y = grid_y() + row * cell_h();

        const int is_today = (g_year == g_today_year &&
                              g_month == g_today_month &&
                              day == g_today_day);
        if (is_today) {
            /* Today is marked by its background rather than by a different
             * ink, so it survives every theme and reads at a glance. */
            wg_fill(x + 2, y + 2, cell_w() - 4, cell_h() - 4, WG_ACCENT);
        }
        snprintf(line, sizeof(line), "%d", day);
        const int tw = (int)strlen(line) * WG_GLYPH_W;
        wg_text(x + (cell_w() - tw) / 2, y + (cell_h() - WG_GLYPH_H) / 2,
                line, is_today ? WG_PAPER : wg_ink_colour());
    }
}

/* Which pill a click landed on, or -1. */
static int pill_hit(int x, int y)
{
    if (y < pill_y() || y >= pill_y() + PILL_H)
        return -1;
    const int i = (x - pill_x()) / PILL_W;
    return (x >= pill_x() && i >= 0 && i < 3) ? i : -1;
}

static int on_event(struct app* a, const struct win_event* e)
{
    (void)a;
    if (e->type == WIN_EVENT_MOUSE_DOWN) {
        const int p = pill_hit(e->x, e->y);
        if (p == 0)      step(-1);
        else if (p == 1) go_today();
        else if (p == 2) step(1);
        else return 0;
        return 1;
    }
    if (e->type == WIN_EVENT_KEY) {
        if (e->key == WIN_KEY_LEFT)       step(-1);
        else if (e->key == WIN_KEY_RIGHT) step(1);
        else if (e->key == WIN_KEY_UP)    step(-12);
        else if (e->key == WIN_KEY_DOWN)  step(12);
        else if (e->key == 't')           go_today();
        else return 0;
        return 1;
    }
    return 0;
}

/* Once a second, and only to notice midnight: the grid is otherwise static,
 * and returning 0 is what keeps a static grid off the busy list. */
static int on_tick(struct app* a)
{
    (void)a;
    const int was = g_today_day;
    read_today();
    return was != g_today_day;
}

int main(int argc, char** argv)
{
    struct app a = {
        .title = "Calendar",
        .width = 420, .height = 380,
        .min_width = 320, .min_height = 300,
        .tick_ms = 1000,
        .draw = draw, .event = on_event, .tick = on_tick,
    };
    g_app = &a;
    go_today();
    return app_run(&a, argc, argv);
}
