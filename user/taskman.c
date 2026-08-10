/* taskman - what the system is doing, and what it is using to do it.
 *
 * The list is a snapshot the kernel copies out under its own lock, so it never
 * shows a slot half-changed. Threads appear alongside processes because that is
 * what they are here - a thread is a task with its group's pid in tgid - and
 * hiding them would misreport where the time is going.
 *
 * "CPU" is share of scheduler slices between one refresh and the next, not a
 * duty cycle: this system has no per-task clock, and the slice count is the
 * honest thing it does have. It is labelled as such rather than dressed up as
 * a percentage of wall time.
 */

#include <dialog.h>
#include <proc.h>
#include <wproto.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <widget.h>
#include <window.h>

#define HEAD_H  84
#define ROW_H   16
#define MAX_P   96

static uint32_t* g_px;
static unsigned  g_w = 520, g_h = 400;

static struct proc_info g_procs[MAX_P];
static int g_n;
static uint64_t g_last_ticks[MAX_P];
static uint32_t g_last_pid[MAX_P];
static uint64_t g_delta[MAX_P];
static uint64_t g_delta_total;
static int g_sel = -1;
static int g_scroll;
static struct mem_info g_mem;
static char g_note[96] = "right-click a task for actions";

/* A history per processor, drawn as one strip chart each - which is the whole
 * reason for showing them separately. One combined line cannot tell "every core
 * half busy" from "one core pinned and the rest asleep", and those are different
 * problems. */
#define HIST 120
#define MAX_CPU 8
static unsigned char g_hist[MAX_CPU][HIST];
static int g_hist_at;
static int g_cpus;
static struct cpu_stat g_cpu[MAX_CPU], g_cpu_last[MAX_CPU];
static int g_bar_drag;

static const char* state_name(uint32_t s)
{
    switch (s) {
    case PROC_READY:   return "ready";
    case PROC_RUNNING: return "running";
    case PROC_BLOCKED: return "blocked";
    case PROC_STOPPED: return "stopped";
    case PROC_ZOMBIE:  return "zombie";
    case PROC_DEAD:    return "dead";
    default:           return "?";
    }
}

static void refresh(void)
{
    const int n = proc_list(g_procs, MAX_P);
    g_n = n < 0 ? 0 : n;
    mem_info(&g_mem);

    /* Match this sample against the last one by pid, so a task that exits does
     * not make its successor look enormously busy. */
    g_delta_total = 0;
    for (int i = 0; i < g_n; ++i) {
        uint64_t before = 0;
        for (int k = 0; k < MAX_P; ++k)
            if (g_last_pid[k] == g_procs[i].pid) { before = g_last_ticks[k]; break; }
        g_delta[i] = g_procs[i].ticks > before ? g_procs[i].ticks - before : 0;
        g_delta_total += g_delta[i];
    }
    for (int i = 0; i < MAX_P; ++i) {
        g_last_pid[i] = i < g_n ? g_procs[i].pid : 0;
        g_last_ticks[i] = i < g_n ? g_procs[i].ticks : 0;
    }

    /* Per processor, from the kernel's own counters rather than inferred from
     * the task list: a slice is attributed where it was actually run. */
    g_cpus = cpu_info(g_cpu, MAX_CPU);
    if (g_cpus < 0)
        g_cpus = 0;
    for (int c = 0; c < g_cpus; ++c) {
        const uint64_t db = g_cpu[c].busy > g_cpu_last[c].busy
                          ? g_cpu[c].busy - g_cpu_last[c].busy : 0;
        const uint64_t di = g_cpu[c].idle > g_cpu_last[c].idle
                          ? g_cpu[c].idle - g_cpu_last[c].idle : 0;
        const uint64_t tot = db + di;
        g_hist[c][g_hist_at] = (unsigned char)(tot > 0 ? db * 100 / tot : 0);
        g_cpu_last[c] = g_cpu[c];
    }
    g_hist_at = (g_hist_at + 1) % HIST;
}

static int rows_visible(void)
{
    const int n = ((int)g_h - HEAD_H - 20) / ROW_H;
    return n > 0 ? n : 1;
}

static int bar_x(void) { return (int)g_w - 6 - WG_SCROLL_W; }
static int list_h(void) { return (int)g_h - HEAD_H - 20; }

static void scroll_to(int first)
{
    const int most = g_n - rows_visible();
    if (first > most) first = most;
    if (first < 0) first = 0;
    g_scroll = first;
}

static void meter(int x, int y, int w, int h, uint64_t part, uint64_t whole,
                  const char* label)
{
    wg_fill(x, y, w, h, WG_PAPER);
    wg_bevel(x, y, w, h, 0);
    if (whole > 0) {
        int fill = (int)((part * (uint64_t)(w - 2)) / whole);
        if (fill > w - 2) fill = w - 2;
        wg_fill(x + 1, y + 1, fill, h - 2, WG_ACCENT);
    }
    wg_text(x + w + 8, y + (h - WG_GLYPH_H) / 2, label, WG_INK);
}

static void draw(void)
{
    wg_theme();
    wg_glass_clear();

    char line[96];
    snprintf(line, sizeof(line), "%d tasks", g_n);
    wg_text(10, 8, line, WG_INK);

    /* Memory, as a proportion of what the machine actually has. */
    snprintf(line, sizeof(line), "%llu of %llu KiB",
             (unsigned long long)(g_mem.used / 1024),
             (unsigned long long)(g_mem.usable / 1024));
    meter(10, 26, 160, 14, g_mem.used, g_mem.usable, line);

    /* One strip chart per processor, side by side and oldest on the left. They
     * are narrower the more there are, which keeps the header one height
     * whatever the machine turns out to have. */
    const int n = g_cpus > 0 ? g_cpus : 1;
    const int total_w = HIST + 60;
    const int each = (total_w / n) - 4;
    const int cy = 8, ch = 46;
    for (int c = 0; c < n; ++c) {
        const int cx = (int)g_w - total_w - 10 + c * (each + 4);
        if (each < 8)
            break;
        wg_fill(cx, cy, each, ch, 0x101820);
        wg_bevel(cx, cy, each, ch, 0);
        for (int i = 0; i < each; ++i) {
            /* Take the most recent `each` samples, so a narrow chart shows the
             * recent past rather than a squashed whole history. */
            const int s_i = (g_hist_at - each + i + 2 * HIST) % HIST;
            const int v = g_hist[c][s_i];
            const int bar = v * (ch - 2) / 100;
            if (bar > 0)
                wg_fill(cx + i, cy + ch - 1 - bar, 1, bar, 0x40D040);
        }
        char lab[16];
        snprintf(lab, sizeof(lab), "cpu %d", c);
        wg_text(cx, cy + ch + 2, lab, WG_DIM);
    }

    const int top = HEAD_H;
    wg_text(10, top - 16, "pid", WG_DIM);
    wg_text(56, top - 16, "name", WG_DIM);
    wg_text(180, top - 16, "state", WG_DIM);
    wg_text(250, top - 16, "cpu", WG_DIM);
    wg_text(320, top - 16, "memory", WG_DIM);
    wg_text(420, top - 16, "uid", WG_DIM);

    wg_fill(4, top, (int)g_w - 8, list_h(), wg_body_colour());
    wg_bevel(4, top, (int)g_w - 8, list_h(), 0);
    wg_scrollbar_v(bar_x(), top, list_h(), g_scroll, rows_visible(),
                   g_n > 0 ? g_n : 1);

    const int rows = rows_visible();
    for (int r = 0; r < rows; ++r) {
        const int i = g_scroll + r;
        if (i >= g_n)
            break;
        const int y = top + 2 + r * ROW_H;
        if (i == g_sel)
            wg_fill(6, y, (int)g_w - 12, ROW_H, wg_sel_colour());

        snprintf(line, sizeof(line), "%u", g_procs[i].pid);
        wg_text(10, y, line, WG_INK);
        /* A thread is shown indented under its group, so the shape of a process
         * with threads is visible at a glance. */
        const int thread = (g_procs[i].tgid != 0 &&
                            g_procs[i].tgid != g_procs[i].pid);
        wg_text_clipped(56 + (thread ? 10 : 0), y, g_procs[i].name,
                        thread ? WG_DIM : WG_INK, 120);
        wg_text(180, y, state_name(g_procs[i].state), WG_INK);

        const unsigned pct = g_delta_total > 0
            ? (unsigned)(g_delta[i] * 100 / g_delta_total) : 0;
        snprintf(line, sizeof(line), "%u%%", pct);
        wg_text(250, y, line, WG_INK);
        /* A short bar beside the number: the eye finds the busy row faster
         * than it reads six numbers. */
        if (pct > 0)
            wg_fill(286, y + 5, (int)(pct * 28 / 100) + 1, 6, WG_ACCENT);

        snprintf(line, sizeof(line), "%llu K",
                 (unsigned long long)(g_procs[i].bytes / 1024));
        wg_text(320, y, line, WG_INK);
        snprintf(line, sizeof(line), "%u", g_procs[i].uid);
        wg_text(420, y, line, WG_INK);
    }

    wg_fill(0, (int)g_h - 20, (int)g_w, 20, WG_FACE);
    wg_text_clipped(8, (int)g_h - 18, g_note, WG_DIM, (int)g_w - 16);
}

static const char* const kMenu[] = { "End task", "-", "Refresh now" };

int main(int argc, char** argv)
{
    const int wx = argc > 1 ? atoi_simple(argv[1]) : 180;
    const int wy = argc > 2 ? atoi_simple(argv[2]) : 140;
    if (wg_font() != 0)
        return 1;
    const int id = win_create(wx, wy, g_w, g_h, "Tasks");
    /* Its pixels carry alpha, so the glass reaches into it. */
    if (id >= 0)
        win_set_alpha(id);
    if (id < 0) {
        printf("taskman: no window server\n");
        return 1;
    }
    g_px = win_map(id);
    if (g_px == 0)
        return 1;
    win_set_min_size(id, 460, 260);
    wg_target(g_px, g_w, g_h);

    refresh();
    draw();
    win_present(id);

    unsigned since = 0;
    for (;;) {
        struct win_event e;
        while (win_poll(id, &e)) {
            if (e.type == WIN_EVENT_CLOSE) { win_destroy(id); return 0; }

            if (menu_active() && e.type != WIN_EVENT_RESIZE) {
                const int pick = menu_event(&e);
                if (pick == 0 && g_sel >= 0 && g_sel < g_n) {
                    const uint32_t pid = g_procs[g_sel].pid;
                    if (kill((int)pid, SIGTERM) == 0)
                        snprintf(g_note, sizeof(g_note), "asked %u to stop", pid);
                    else
                        snprintf(g_note, sizeof(g_note), "could not signal %u", pid);
                    refresh();
                } else if (pick == 2) {
                    refresh();
                }
                draw();
                menu_draw();
                win_present(id);
                continue;
            }

            if (e.type == WIN_EVENT_RESIZE) {
                g_w = (unsigned)e.x; g_h = (unsigned)e.y;
                g_px = win_map(id);
                if (g_px == 0) return 1;
                wg_target(g_px, g_w, g_h);
            } else if (e.type == WIN_EVENT_MOUSE_DOWN) {
                if (e.x >= bar_x() && e.y >= HEAD_H) {
                    if (wg_scroll_on_thumb_v(e.y, HEAD_H, list_h(), g_scroll,
                                             rows_visible(), g_n > 0 ? g_n : 1))
                        g_bar_drag = 1;
                    else
                        scroll_to(wg_scroll_hit_v(e.x, e.y, bar_x(), HEAD_H,
                            list_h(), g_scroll, rows_visible(),
                            g_n > 0 ? g_n : 1));
                    draw(); menu_draw(); win_present(id);
                    continue;
                }
                const int hit = g_scroll + (e.y - HEAD_H - 2) / ROW_H;
                if (e.y >= HEAD_H && hit >= 0 && hit < g_n)
                    g_sel = hit;
                if (e.button == 2 && g_sel >= 0)
                    menu_open(e.x, e.y, kMenu, 3);
            } else if (e.type == WIN_EVENT_MOUSE_UP) {
                g_bar_drag = 0;
            } else if (e.type == WIN_EVENT_MOUSE_MOVE && g_bar_drag) {
                scroll_to(wg_scroll_drag_v(e.y, HEAD_H, list_h(),
                                           rows_visible(), g_n > 0 ? g_n : 1));
            } else if (e.type == WIN_EVENT_KEY) {
                /* Arrows move the selection and drag the view along with it,
                 * which is what makes a long list navigable without a mouse. */
                if (e.key == WIN_KEY_DOWN || e.key == WIN_KEY_UP) {
                    if (e.key == WIN_KEY_DOWN && g_sel + 1 < g_n) ++g_sel;
                    else if (e.key == WIN_KEY_UP && g_sel > 0) --g_sel;
                    if (g_sel < g_scroll) scroll_to(g_sel);
                    else if (g_sel >= g_scroll + rows_visible())
                        scroll_to(g_sel - rows_visible() + 1);
                } else if (e.key == WIN_KEY_RIGHT) {
                    scroll_to(g_scroll + rows_visible());
                } else if (e.key == WIN_KEY_LEFT) {
                    scroll_to(g_scroll - rows_visible());
                } else if (e.key == 'r') refresh();
                else if (e.key == 'k' && g_sel >= 0 && g_sel < g_n)
                    kill((int)g_procs[g_sel].pid, SIGTERM);
            } else {
                continue;
            }
            draw();
            menu_draw();
            win_present(id);
        }

        /* Twice a second: often enough to feel live, seldom enough that the
         * monitor is not itself the busiest thing on the list. */
        if (++since >= 33) {
            since = 0;
            refresh();
            draw();
            menu_draw();
            win_present(id);
        }
        msleep(15);
    }
}
