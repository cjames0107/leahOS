/* console - what the machine has said.
 *
 * Every kernel message used to go to the serial line and the screen and then
 * cease to exist. If a driver complained during boot, the only way to read it
 * was to have been watching; if something faulted while the desktop was up, the
 * report scrolled off a console nobody was looking at. The kernel keeps a ring
 * of its output now, and this is the window onto it.
 *
 * It is a viewer and nothing else. There is no severity to filter on, because
 * the kernel does not label its messages with one, and inventing levels by
 * pattern-matching the text would be a guess presented as a fact. What it does
 * offer is a search, which is what "show me only the disk messages" actually
 * means when every line is just a line.
 */

#include <app.h>
#include <ui.h>
#include <proc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <widget.h>
#include <window.h>

#define MAX_LINES 400
#define LINE_MAX  160
/* The log as lines, oldest first. A ring would save the copying, but the whole
 * point of this window is to scroll back through it, and a flat array indexes
 * directly at the cost of one memmove per overflow. */
static char g_line[MAX_LINES][LINE_MAX];
static int  g_lines;
static char g_partial[LINE_MAX];        /* a line the kernel has not ended yet */
static int  g_partial_n;

static unsigned long long g_at;         /* our position in the kernel's ring */
static int  g_follow = 1;               /* stay at the end as new lines arrive */
static char g_find[64];

static void add_line(const char* text)
{
    if (g_lines >= MAX_LINES) {
        /* Drop the oldest half rather than one line at a time: this happens on
         * a busy log, and shifting the whole array per line is how a log
         * viewer becomes the reason the log is busy. */
        const int keep = MAX_LINES / 2;
        for (int i = 0; i < keep; ++i)
            memcpy(g_line[i], g_line[MAX_LINES - keep + i], LINE_MAX);
        g_lines = keep;
        /* Where the list is looking is the list's own business now, and
         * refilter puts it back at the end when following. */
    }
    snprintf(g_line[g_lines++], LINE_MAX, "%s", text);
}

/* Take whatever the kernel has said since last time and cut it into lines.
 *
 * The ring holds bytes, not lines, so a read can stop mid-message; the tail is
 * held in g_partial until its newline arrives. Without that a message would be
 * split across two rows at whatever byte the poll happened to land on. */
static void pump(void)
{
    char buf[1024];
    for (;;) {
        const unsigned long n = klog_read(&g_at, buf, sizeof(buf));
        if (n == 0)
            break;
        for (unsigned long i = 0; i < n; ++i) {
            const char c = buf[i];
            if (c == '\n' || g_partial_n + 1 >= LINE_MAX) {
                g_partial[g_partial_n] = '\0';
                if (g_partial_n > 0)
                    add_line(g_partial);
                g_partial_n = 0;
            }
            if (c != '\n' && c != '\r')
                g_partial[g_partial_n++] = c;
        }
    }
}

/* --- the interface ---------------------------------------------------------
 *
 * A search field, a Follow toggle, and the log as a list. The list does the
 * scrolling, the selection and the keyboard, which is most of what this file
 * used to be.
 */

static struct app g_app;
static struct ui_view* g_list;
static struct ui_view* g_follow_toggle;

/* Which lines the search leaves. Recomputed when the filter or the log
 * changes rather than per row: a row callback that filtered would scan the
 * whole log once per visible line. */
static int g_shown[MAX_LINES];
static int g_shown_n;

static int fold(char c)
{
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

static int matches(const char* line)
{
    if (g_find[0] == '\0')
        return 1;
    for (const char* p = line; *p != '\0'; ++p) {
        int i = 0;
        while (g_find[i] != '\0' && fold(p[i]) == fold(g_find[i]))
            ++i;
        if (g_find[i] == '\0')
            return 1;
    }
    return 0;
}

static void refilter(void)
{
    g_shown_n = 0;
    for (int i = 0; i < g_lines; ++i)
        if (matches(g_line[i]))
            g_shown[g_shown_n++] = i;
    if (g_list != 0) {
        g_list->rows = g_shown_n;
        if (g_follow) {
            /* Following means the newest line stays visible, which is a scroll
             * position rather than a selection - selecting it would fight
             * whatever the person had chosen to look at. */
            const int page = g_list->frame.h / (g_list->row_h > 0
                                                ? g_list->row_h : 1);
            g_list->scroll = g_shown_n > page ? g_shown_n - page : 0;
        }
    }
}

static const char* log_row(void* user, int row)
{
    (void)user;
    if (row < 0 || row >= g_shown_n)
        return "";
    return g_line[g_shown[row]];
}

static void on_find(struct ui_view* v, void* user)
{
    (void)user;
    snprintf(g_find, sizeof(g_find), "%s", ui_text(v));
    refilter();
}

static void on_follow(struct ui_view* v, void* user)
{
    (void)user;
    g_follow = v->on;
    refilter();
}

static int on_tick(struct app* a)
{
    (void)a;
    const int was = g_lines;
    pump();
    if (g_lines == was)
        return 0;
    refilter();
    return 1;
}

int main(int argc, char** argv)
{
    struct ui_view* root = ui_box(0, UI_STACK_V, 10, 8);

    struct ui_view* bar = ui_box(root, UI_STACK_H, 0, 10);
    ui_size(bar, 0, 26);
    ui_grow(bar, 0);
    ui_on(ui_search(bar, "Search the log"), on_find, 0);
    g_follow_toggle = ui_toggle(bar, "Follow", 1);
    ui_on(g_follow_toggle, on_follow, 0);
    ui_grow(g_follow_toggle, 0);
    ui_spacer(bar);

    g_list = ui_list(root, log_row, 0, 0);

    pump();
    refilter();

    g_app.title = "Console";
    g_app.width = 680; g_app.height = 440;
    g_app.min_width = 480; g_app.min_height = 300;
    /* Four times a second. The ring loses nothing between polls - a reader's
     * position is a byte count - so this can be lazy. */
    g_app.tick_ms = 250;
    g_app.tick = on_tick;
    g_app.root = root;
    return app_run(&g_app, argc, argv);
}
