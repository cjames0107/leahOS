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

/* A history of total scheduling activity, drawn as a strip chart - the shape
 * over time says more than any single number. */
#define HIST 120
static unsigned char g_hist[HIST];
static int g_hist_at;

static const char* state_name(uint32_t s)
{
    switch (s) {
    case PROC_READY:   return "ready";
    case PROC_RUNNING: return "running";
    case PROC_BLOCKED: return "blocked";
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

    /* The idle tasks are most of the slices on a quiet machine; charting them
     * would show a flat 100% and say nothing. */
    uint64_t busy = 0;
    for (int i = 0; i < g_n; ++i)
        if (strcmp(g_procs[i].name, "idle") != 0)
            busy += g_delta[i];
    unsigned char bar = 0;
    if (g_delta_total > 0) {
        const uint64_t pct = busy * 100 / g_delta_total;
        bar = (unsigned char)(pct > 100 ? 100 : pct);
    }
    g_hist[g_hist_at] = bar;
    g_hist_at = (g_hist_at + 1) % HIST;
}

static int rows_visible(void) { return ((int)g_h - HEAD_H - 20) / ROW_H; }

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
    wg_fill(0, 0, (int)g_w, (int)g_h, WG_FACE);

    char line[96];
    snprintf(line, sizeof(line), "%d tasks", g_n);
    wg_text(10, 8, line, WG_INK);

    /* Memory, as a proportion of what the machine actually has. */
    snprintf(line, sizeof(line), "%llu of %llu KiB",
             (unsigned long long)(g_mem.used / 1024),
             (unsigned long long)(g_mem.usable / 1024));
    meter(10, 26, 160, 14, g_mem.used, g_mem.usable, line);

    /* The strip chart: oldest on the left. */
    const int cx = (int)g_w - HIST - 14, cy = 8, ch = 46;
    wg_fill(cx, cy, HIST, ch, 0x101820);
    wg_bevel(cx, cy, HIST, ch, 0);
    for (int i = 0; i < HIST; ++i) {
        const int v = g_hist[(g_hist_at + i) % HIST];
        const int bar = v * (ch - 2) / 100;
        if (bar > 0)
            wg_fill(cx + i, cy + ch - 1 - bar, 1, bar, 0x40D040);
    }
    wg_text(cx, cy + ch + 2, "scheduler activity", WG_DIM);

    const int top = HEAD_H;
    wg_text(10, top - 16, "pid", WG_DIM);
    wg_text(56, top - 16, "name", WG_DIM);
    wg_text(180, top - 16, "state", WG_DIM);
    wg_text(250, top - 16, "cpu", WG_DIM);
    wg_text(320, top - 16, "memory", WG_DIM);
    wg_text(420, top - 16, "uid", WG_DIM);

    wg_fill(4, top, (int)g_w - 8, (int)g_h - top - 20, WG_PAPER);
    wg_bevel(4, top, (int)g_w - 8, (int)g_h - top - 20, 0);

    const int rows = rows_visible();
    for (int r = 0; r < rows; ++r) {
        const int i = g_scroll + r;
        if (i >= g_n)
            break;
        const int y = top + 2 + r * ROW_H;
        if (i == g_sel)
            wg_fill(6, y, (int)g_w - 12, ROW_H, 0xB0C4DE);

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
                const int hit = g_scroll + (e.y - HEAD_H - 2) / ROW_H;
                if (e.y >= HEAD_H && hit >= 0 && hit < g_n)
                    g_sel = hit;
                if (e.button == 2 && g_sel >= 0)
                    menu_open(e.x, e.y, kMenu, 3);
            } else if (e.type == WIN_EVENT_KEY) {
                if (e.key == 'r') refresh();
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
