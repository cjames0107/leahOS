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

#include <dialog.h>
#include <proc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <widget.h>
#include <window.h>

#define MAX_LINES 400
#define LINE_MAX  160
#define HEAD_H    46

static uint32_t* g_px;
static unsigned  g_w = 680, g_h = 440;

/* The log as lines, oldest first. A ring would save the copying, but the whole
 * point of this window is to scroll back through it, and a flat array indexes
 * directly at the cost of one memmove per overflow. */
static char g_line[MAX_LINES][LINE_MAX];
static int  g_lines;
static char g_partial[LINE_MAX];        /* a line the kernel has not ended yet */
static int  g_partial_n;

static unsigned long long g_at;         /* our position in the kernel's ring */
static int  g_scroll;
static int  g_follow = 1;               /* stay at the end as new lines arrive */
static int  g_bar_drag;

static char g_find[64];
static int  g_focus;

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
        if (g_scroll > keep) g_scroll -= MAX_LINES - keep;
        if (g_scroll < 0) g_scroll = 0;
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

static int rows_visible(void)
{
    const int n = ((int)g_h - HEAD_H - 8) / WG_GLYPH_H;
    return n > 0 ? n : 1;
}

static int bar_x(void)  { return (int)g_w - 6 - WG_SCROLL_W; }
static int list_h(void) { return (int)g_h - HEAD_H - 8; }

static void scroll_to(int first)
{
    const int most = g_lines - rows_visible();
    if (first > most) first = most;
    if (first < 0) first = 0;
    g_scroll = first;
    /* Following resumes only when the view is back at the bottom, so scrolling
     * up to read something does not fight with new messages arriving. */
    g_follow = (g_scroll >= most);
}

static void to_end(void)
{
    const int most = g_lines - rows_visible();
    g_scroll = most > 0 ? most : 0;
    g_follow = 1;
}

/* Whether a line contains what is being searched for. Case-insensitive,
 * because nobody types "AHCI" when they mean the ahci messages. */
static int matches(const char* line)
{
    if (g_find[0] == '\0')
        return 1;
    for (const char* p = line; *p != '\0'; ++p) {
        unsigned i = 0;
        while (g_find[i] != '\0') {
            char a = p[i], b = g_find[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b)
                break;
            ++i;
        }
        if (g_find[i] == '\0')
            return 1;
    }
    return 0;
}

static int field_w(void) { return (int)g_w - 210; }

static void draw(void)
{
    wg_theme();
    wg_glass_clear();

    char head[64];
    snprintf(head, sizeof(head), "%d lines", g_lines);
    wg_text(12, 14, head, WG_DIM);

    wg_field(96, 10, field_w(), 24, g_find[0] ? g_find : "search", g_focus);
    wg_button((int)g_w - 104, 10, 92, 24, g_follow ? "Following" : "Follow", 0);

    wg_container(4, HEAD_H - 4, (int)g_w - 8, list_h() + 8, 6);
    wg_scrollbar_v(bar_x(), HEAD_H, list_h(), g_scroll, rows_visible(),
                   g_lines > 0 ? g_lines : 1);

    const int rows = rows_visible();
    int drawn = 0;
    for (int r = 0; r < rows; ++r) {
        const int i = g_scroll + r;
        if (i >= g_lines)
            break;
        if (!matches(g_line[i]))
            continue;
        const int y = HEAD_H + drawn * WG_GLYPH_H;
        /* A line the kernel itself called a fault is worth finding by eye. */
        const uint32_t ink = matches(g_line[i]) &&
            (strstr(g_line[i], "faulted") != 0 ||
             strstr(g_line[i], "PANIC") != 0) ? 0xD2413Au : wg_ink_colour();
        wg_text_clipped(12, y, g_line[i], ink, bar_x() - 20);
        ++drawn;
    }
    if (g_lines == 0)
        wg_text(12, HEAD_H, "the kernel has said nothing yet", WG_DIM);
}

static const char* const kMenu[] = { "Follow", "Jump to end", "-", "Clear view" };

int main(int argc, char** argv)
{
    const int wx = argc > 1 ? atoi_simple(argv[1]) : 170;
    const int wy = argc > 2 ? atoi_simple(argv[2]) : 150;
    if (wg_font() != 0)
        return 1;
    const int id = win_create(wx, wy, g_w, g_h, "Console");
    if (id < 0) {
        printf("console: no window server\n");
        return 1;
    }
    win_set_alpha(id);
    g_px = win_map(id);
    if (g_px == 0)
        return 1;
    win_set_min_size(id, 480, 300);
    wg_target(g_px, g_w, g_h);

    pump();
    to_end();
    draw();
    win_present(id);

    unsigned since = 0;
    for (;;) {
        struct win_event e;
        while (win_poll(id, &e)) {
            if (e.type == WIN_EVENT_CLOSE) { win_destroy(id); return 0; }

            if (menu_active() && e.type != WIN_EVENT_RESIZE) {
                const int pick = menu_event(&e);
                if (pick == 0)      g_follow = !g_follow;
                else if (pick == 1) to_end();
                else if (pick == 3) { g_lines = 0; g_scroll = 0; }
                draw(); menu_draw(); win_present(id);
                continue;
            }

            if (e.type == WIN_EVENT_RESIZE) {
                g_w = (unsigned)e.x; g_h = (unsigned)e.y;
                g_px = win_map(id);
                if (g_px == 0) return 1;
                wg_target(g_px, g_w, g_h);
                if (g_follow) to_end();
            } else if (e.type == WIN_EVENT_MOUSE_DOWN) {
                if (e.button == 2) {
                    menu_open(e.x, e.y, kMenu, 4);
                } else if (e.y < HEAD_H - 8) {
                    if (e.x >= 96 && e.x < 96 + field_w())
                        g_focus = 1;
                    else if (e.x >= (int)g_w - 104) {
                        g_focus = 0;
                        if (g_follow) g_follow = 0; else to_end();
                    }
                } else if (e.x >= bar_x()) {
                    if (wg_scroll_on_thumb_v(e.y, HEAD_H, list_h(), g_scroll,
                                             rows_visible(),
                                             g_lines > 0 ? g_lines : 1))
                        g_bar_drag = 1;
                    else
                        scroll_to(wg_scroll_hit_v(e.x, e.y, bar_x(), HEAD_H,
                            list_h(), g_scroll, rows_visible(),
                            g_lines > 0 ? g_lines : 1));
                } else {
                    g_focus = 0;
                }
            } else if (e.type == WIN_EVENT_MOUSE_UP) {
                g_bar_drag = 0;
            } else if (e.type == WIN_EVENT_MOUSE_MOVE && g_bar_drag) {
                scroll_to(wg_scroll_drag_v(e.y, HEAD_H, list_h(),
                                           rows_visible(),
                                           g_lines > 0 ? g_lines : 1));
            } else if (e.type == WIN_EVENT_KEY) {
                if (g_focus) {
                    const unsigned n = (unsigned)strlen(g_find);
                    if (e.key == '\b' && n > 0)          g_find[n - 1] = '\0';
                    else if (e.key == '\n')              g_focus = 0;
                    else if (e.key >= ' ' && e.key < 127 &&
                             n + 1 < sizeof(g_find)) {
                        g_find[n] = (char)e.key;
                        g_find[n + 1] = '\0';
                    }
                } else if (e.key == WIN_KEY_DOWN) scroll_to(g_scroll + 1);
                else if (e.key == WIN_KEY_UP)     scroll_to(g_scroll - 1);
                else if (e.key == WIN_KEY_RIGHT)  scroll_to(g_scroll + rows_visible());
                else if (e.key == WIN_KEY_LEFT)   scroll_to(g_scroll - rows_visible());
                else if (e.key == 'e')            to_end();
                else continue;
            } else {
                continue;
            }
            draw();
            menu_draw();
            win_present(id);
        }

        /* Four times a second. The ring does not lose anything between polls -
         * the position is a byte count, so a slow reader falls behind rather
         * than missing messages - which is why this can be lazy. */
        if (++since >= 16) {
            since = 0;
            const int was = g_lines;
            pump();
            if (g_lines != was) {
                if (g_follow) to_end();
                draw();
                menu_draw();
                win_present(id);
            }
        }
        msleep(15);
    }
}
