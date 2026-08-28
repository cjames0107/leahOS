/* clock - the time, and how long the machine has been up.
 *
 * It used to draw a bar that grew and wrapped, to show that the server kept
 * redrawing. That was worth having when a window that repainted at all was the
 * thing being demonstrated; it is not worth a window now. What a thing called
 * Clock should show is the time.
 *
 * On the component library, which is why it is nine lines of interface: a
 * label per fact, and a tick that rewrites them.
 */

#include <app.h>
#include <prefs.h>
#include <stdio.h>
#include <time.h>
#include <ui.h>

static struct ui_view* g_time;
static struct ui_view* g_date;
static struct ui_view* g_up;

/* Filled in by the tick and pointed at by the labels, so the text a label
 * shows is rewritten in place rather than by rebuilding the interface. */
static char g_time_text[32];
static char g_date_text[48];
static char g_up_text[48];

/* Whether to show a twelve or a twenty-four hour clock.
 *
 * Re-read from the file rather than remembered, because Settings writes it
 * while this is running and a clock that needed restarting to change format
 * would make the setting look broken. Once every few seconds, not every tick:
 * this runs twice a second and the answer changes about never. */
static int wants_24_hour(void)
{
    static int answer = 1;
    static unsigned long asked;
    const unsigned long now = uptime_ms();
    if (asked == 0 || now - asked > 3000) {
        asked = now;
        prefs_scope(PREFS_DESKTOP);
        prefs_load();
        answer = prefs_get_u32("clock.24hour", 1) != 0;
        /* And back to this application's own, which is the scope app_run
         * chose and the one it saves this window's geometry into. Leaving the
         * desktop's selected would have put the clock's window position in
         * the file that describes the desktop. */
        prefs_scope("Clock");
        prefs_load();
    }
    return answer;
}

static void refresh(void)
{
    const time_t now = time(0);
    struct tm t;
    if (localtime_r(&now, &t) != 0) {
        if (wants_24_hour()) {
            snprintf(g_time_text, sizeof(g_time_text), "%02d:%02d:%02d",
                     t.tm_hour, t.tm_min, t.tm_sec);
        } else {
            int h = t.tm_hour % 12;
            if (h == 0) h = 12;         /* midnight and noon are twelve */
            snprintf(g_time_text, sizeof(g_time_text), "%d:%02d:%02d %s",
                     h, t.tm_min, t.tm_sec, t.tm_hour < 12 ? "am" : "pm");
        }
        static const char* const kMonths[12] = {
            "January", "February", "March", "April", "May", "June", "July",
            "August", "September", "October", "November", "December"
        };
        snprintf(g_date_text, sizeof(g_date_text), "%d %s %d", t.tm_mday,
                 kMonths[t.tm_mon >= 0 && t.tm_mon < 12 ? t.tm_mon : 0],
                 t.tm_year + 1900);
    }

    /* Uptime from the monotonic clock rather than from the wall one: the wall
     * clock can be set, and an uptime that jumped when somebody corrected the
     * date would be measuring the wrong thing. */
    const unsigned long ms = uptime_ms();
    const unsigned long secs = ms / 1000;
    if (secs >= 3600)
        snprintf(g_up_text, sizeof(g_up_text), "up %lu h %lu m",
                 secs / 3600, (secs % 3600) / 60);
    else if (secs >= 60)
        snprintf(g_up_text, sizeof(g_up_text), "up %lu m %lu s",
                 secs / 60, secs % 60);
    else
        snprintf(g_up_text, sizeof(g_up_text), "up %lu s", secs);

    ui_set_text(g_time, g_time_text);
    ui_set_text(g_date, g_date_text);
    ui_set_text(g_up, g_up_text);
}

static int on_tick(struct app* a)
{
    (void)a;
    refresh();
    return 1;                   /* the seconds moved; it always has to redraw */
}

int main(int argc, char** argv)
{
    struct ui_view* root = ui_box(0, UI_STACK_V, 16, 4);
    g_time = ui_label(root, "");
    g_date = ui_label(root, "");
    ui_grow(ui_separator(root), 0);
    g_up = ui_label(root, "");
    ui_spacer(root);
    refresh();

    struct app a = {
        .title = "Clock",
        .width = 240, .height = 130,
        .min_width = 180, .min_height = 110,
        .tick_ms = 500,         /* twice a second, so a second never looks stuck */
        .tick = on_tick,
        .root = root,
    };
    return app_run(&a, argc, argv);
}
