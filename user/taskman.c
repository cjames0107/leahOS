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
 *
 * The strip charts and the memory meter that used to sit above the list are in
 * the Resource Monitor now. They answered a different question - "is the
 * machine busy, and with what kind of work" rather than "what is running and
 * can I stop it" - and keeping both here meant the list was always short and
 * the charts were always small. What is left is the list, with the window to
 * itself.
 */

#include <app.h>
#include <ui.h>
#include <proc.h>
#include <wproto.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <widget.h>
#include <window.h>

#define HEAD_H  30
#define ROW_H   16
#define MAX_P   96

static struct proc_info g_procs[MAX_P];
static int g_n;
static uint64_t g_last_ticks[MAX_P];
static uint32_t g_last_pid[MAX_P];
static uint64_t g_delta[MAX_P];
static uint64_t g_delta_total;
static char g_note[96] = "right-click a task for actions";


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
}

/* --- the interface ---------------------------------------------------------
 *
 * A table. The columns were six calls to wg_text at hand-counted offsets and a
 * scrollbar this file drove itself; they are declared once now, and a cell is
 * asked for by row and column.
 */

static struct app g_app;
static struct ui_view* g_table;
static struct ui_view* g_count;
static char g_count_text[32];

static const char* task_cell(void* user, int row, int col)
{
    (void)user;
    static char text[64];
    if (row < 0 || row >= g_n)
        return "";
    const struct proc_info* p = &g_procs[row];
    switch (col) {
    case 0:
        snprintf(text, sizeof(text), "%u", p->pid);
        return text;
    case 1:
        /* A thread is shown indented under its group, so the shape of a
         * process with threads is visible at a glance. */
        if (p->tgid != 0 && p->tgid != p->pid)
            snprintf(text, sizeof(text), "  %s", p->name);
        else
            snprintf(text, sizeof(text), "%s", p->name);
        return text;
    case 2:
        return state_name(p->state);
    case 3: {
        const unsigned pct = g_delta_total > 0
            ? (unsigned)(g_delta[row] * 100 / g_delta_total) : 0;
        snprintf(text, sizeof(text), "%u%%", pct);
        return text;
    }
    case 4:
        snprintf(text, sizeof(text), "%llu K",
                 (unsigned long long)(p->bytes / 1024));
        return text;
    default:
        snprintf(text, sizeof(text), "%u", p->uid);
        return text;
    }
}

static void sync_table(void)
{
    g_table->rows = g_n;
    snprintf(g_count_text, sizeof(g_count_text), "%d tasks", g_n);
    ui_set_text(g_count, g_count_text);
}

static int on_tick(struct app* a)
{
    (void)a;
    refresh();
    sync_table();
    return 1;
}

static void end_task(struct app* a)
{
    const int row = g_table->selected;
    if (row < 0 || row >= g_n)
        return;
    const uint32_t pid = g_procs[row].pid;
    if (kill((int)pid, SIGTERM) == 0)
        snprintf(g_note, sizeof(g_note), "asked %u to stop", pid);
    else
        snprintf(g_note, sizeof(g_note), "could not signal %u", pid);
    refresh();
    sync_table();
    (void)a;
}

static const char* const kMenu[] = { "End task", "-", "Refresh now" };

static int on_menu(struct app* a, int pick)
{
    if (pick == 0)      end_task(a);
    else if (pick == 2) { refresh(); sync_table(); }
    return 1;
}

static int on_event(struct app* a, const struct win_event* e)
{
    (void)a;
    /* The table has the arrows and the selection; this is only the shortcut
     * that the menu also offers. */
    if (e->type == WIN_EVENT_KEY && e->key == 'k') {
        end_task(a);
        return 1;
    }
    return 0;
}

int main(int argc, char** argv)
{
    struct ui_view* root = ui_box(0, UI_STACK_V, 8, 6);
    g_count = ui_label(root, "");
    ui_grow(g_count, 0);

    g_table = ui_table(root, task_cell, 0, 0);
    ui_column(g_table, "pid", 60);
    ui_column(g_table, "name", 140);
    ui_column(g_table, "state", 80);
    ui_column(g_table, "cpu", 60);
    ui_column(g_table, "memory", 90);
    ui_column(g_table, "uid", 50);

    refresh();
    sync_table();

    g_app.title = "Tasks";
    g_app.width = 560; g_app.height = 400;
    g_app.min_width = 460; g_app.min_height = 260;
    /* Twice a second: often enough to feel live, seldom enough that the
     * monitor is not itself the busiest thing on the list. */
    g_app.tick_ms = 500;
    g_app.tick = on_tick;
    g_app.event = on_event;
    g_app.menu = kMenu;
    g_app.menu_count = 3;
    g_app.menu_pick = on_menu;
    g_app.root = root;
    return app_run(&g_app, argc, argv);
}
