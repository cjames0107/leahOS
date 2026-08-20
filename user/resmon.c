/* resmon - where the machine's capacity is going.
 *
 * This was the top third of the Tasks window, and it did not belong there. A
 * process list answers "what is running and can I stop it"; a strip chart
 * answers "is the machine busy and with what kind of work". They are looked at
 * at different moments, and cramming both into one window meant the list was
 * always short and the charts were always small.
 *
 * Split out, each gets the room its question needs: the charts get a pane per
 * resource with history, and the list gets the whole window back.
 *
 * Every number here is one the system actually keeps. Nothing is modelled, and
 * where a figure does not exist - a GPU's utilisation, most obviously - the
 * pane says so rather than drawing a flat line and letting it be mistaken for
 * an idle device.
 */

#include <app.h>
#include <ui.h>
#include <net.h>
#include <proc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statfs.h>
#include <unistd.h>
#include <widget.h>
#include <window.h>

#define SIDE_W  150
#define ROW_H   26
#define HIST    160
#define MAX_CPU 8
#define MAX_P   96

enum { PANE_CPU, PANE_MEM, PANE_NET, PANE_DISK, PANE_GPU, PANE_COUNT };
static const char* const kPanes[PANE_COUNT] = {
    "CPU", "Memory", "Network", "Disk", "Graphics"
};
static int g_pane_index;

/* One ring per series. They are all percentages except the network, which is
 * frames per sample scaled against the busiest sample seen - there is no line
 * rate to divide by, and inventing one would make the chart a fiction. */
static unsigned char g_cpu_hist[MAX_CPU][HIST];
static unsigned char g_mem_hist[HIST];
static unsigned char g_net_hist[HIST];
static int g_at;

static int g_cpus;
static struct cpu_stat g_cpu[MAX_CPU], g_cpu_last[MAX_CPU];
static struct mem_info g_mem;
static unsigned long g_load[3];

static struct netinfo g_net;
static int g_have_net;
static uint64_t g_frames_last, g_frames_peak = 1;

static struct statfs g_root;
static int g_have_root;

/* The compositor's share, which is the honest answer to "what is the graphics
 * hardware doing" on a machine whose graphics hardware is a linear
 * framebuffer: all of the drawing is that process. */
static struct proc_info g_procs[MAX_P];
static unsigned g_wserver_pct;

static void sample(void)
{
    /* Per processor, from the kernel's own counters: a slice is attributed
     * where it was actually run, so one pinned core is visible as one pinned
     * core rather than as a machine at 1/n load. */
    g_cpus = cpu_info(g_cpu, MAX_CPU);
    if (g_cpus < 0)
        g_cpus = 0;
    for (int c = 0; c < g_cpus; ++c) {
        const uint64_t db = g_cpu[c].busy > g_cpu_last[c].busy
                          ? g_cpu[c].busy - g_cpu_last[c].busy : 0;
        const uint64_t di = g_cpu[c].idle > g_cpu_last[c].idle
                          ? g_cpu[c].idle - g_cpu_last[c].idle : 0;
        const uint64_t tot = db + di;
        g_cpu_hist[c][g_at] = (unsigned char)(tot > 0 ? db * 100 / tot : 0);
        g_cpu_last[c] = g_cpu[c];
    }

    mem_info(&g_mem);
    g_mem_hist[g_at] = (unsigned char)(g_mem.usable > 0
        ? g_mem.used * 100 / g_mem.usable : 0);
    load_average(g_load);

    g_have_net = netinfo(&g_net) == 0;

    /* No byte counters are exported, so the series is left flat rather than
     * filled with a number that means something else. The pane says as much. */
    g_net_hist[g_at] = 0;
    (void)g_frames_last; (void)g_frames_peak;

    g_have_root = statfs("/", &g_root) == 0 && g_root.f_blocks > 0;

    const int n = proc_list(g_procs, MAX_P);
    uint64_t total = 0, ws = 0;
    static uint64_t last_ticks[MAX_P];
    static uint32_t last_pid[MAX_P];
    for (int i = 0; i < n && i < MAX_P; ++i) {
        uint64_t before = 0;
        for (int k = 0; k < MAX_P; ++k)
            if (last_pid[k] == g_procs[i].pid) { before = last_ticks[k]; break; }
        const uint64_t d = g_procs[i].ticks > before
                         ? g_procs[i].ticks - before : 0;
        total += d;
        if (strcmp(g_procs[i].name, "wserver") == 0)
            ws += d;
    }
    for (int i = 0; i < MAX_P; ++i) {
        last_pid[i]   = i < n ? g_procs[i].pid : 0;
        last_ticks[i] = i < n ? g_procs[i].ticks : 0;
    }
    g_wserver_pct = total > 0 ? (unsigned)(ws * 100 / total) : 0;

    g_at = (g_at + 1) % HIST;
}

/* --- drawing -------------------------------------------------------------- */

/* The pane the charts are drawn into, handed over by the layout rather than
 * measured from the window: the sidebar's width is the layout's business now. */
static struct ui_rect g_pane;
static int cx(void) { return g_pane.x; }
static int cw(void) { return g_pane.w; }

/* One strip chart. Oldest on the left, newest on the right, and the most
 * recent `w` samples rather than the whole ring squashed - a narrow chart
 * should show the recent past, not a compressed history. */
static void chart(int x, int y, int w, int h, const unsigned char* hist,
                  uint32_t colour, const char* label)
{
    wg_fill(x, y, w, h, 0x101820);
    wg_bevel(x, y, w, h, 0);
    for (int i = 0; i < w - 2 && i < HIST; ++i) {
        const int s = (g_at - (w - 2) + i + 2 * HIST) % HIST;
        const int bar = hist[s] * (h - 2) / 100;
        if (bar > 0)
            wg_fill(x + 1 + i, y + h - 1 - bar, 1, bar, colour);
    }
    if (label != 0)
        wg_text(x, y + h + 2, label, WG_DIM);
}

static void bar(int x, int y, int w, uint64_t part, uint64_t whole)
{
    wg_fill(x, y, w, 12, WG_PAPER);
    wg_bevel(x, y, w, 12, 0);
    if (whole > 0) {
        int fill = (int)((part * (uint64_t)(w - 2)) / whole);
        if (fill > w - 2) fill = w - 2;
        wg_fill(x + 1, y + 1, fill, 10,
                part * 10 > whole * 9 ? 0xD2413Au : WG_ACCENT);
    }
}

static void human(uint64_t bytes, char* out, unsigned max)
{
    if (bytes >= (1ull << 30))
        snprintf(out, max, "%llu GiB", (unsigned long long)(bytes >> 30));
    else if (bytes >= (1ull << 20))
        snprintf(out, max, "%llu MiB", (unsigned long long)(bytes >> 20));
    else
        snprintf(out, max, "%llu KiB", (unsigned long long)(bytes >> 10));
}

static void draw_cpu(void)
{
    char line[96];
    const int n = g_cpus > 0 ? g_cpus : 1;
    /* One chart per processor, stacked: the shape of "one core pinned" and
     * "every core half busy" is the thing worth seeing, and a single averaged
     * line cannot tell them apart. */
    const int each = ((g_pane.h - 70) / n) - 22;
    for (int c = 0; c < n; ++c) {
        const int y = 60 + c * (each + 22);
        if (each < 16)
            break;
        snprintf(line, sizeof(line), "cpu %d - %u%%", c, g_cpu_hist[c][
            (g_at - 1 + HIST) % HIST]);
        chart(cx(), y, cw(), each, g_cpu_hist[c], 0x40D040, line);
    }
    snprintf(line, sizeof(line), "load  %lu.%02lu  %lu.%02lu  %lu.%02lu",
             g_load[0] / 100, g_load[0] % 100, g_load[1] / 100, g_load[1] % 100,
             g_load[2] / 100, g_load[2] % 100);
    wg_text(cx(), g_pane.y + g_pane.h - 20, line, wg_ink_colour());
}

static void draw_mem(void)
{
    char line[96], a[24], b[24];
    chart(cx(), g_pane.y + 10, cw(), 120, g_mem_hist, 0x4090E0, "in use, per cent");

    human(g_mem.used, a, sizeof(a));
    human(g_mem.usable, b, sizeof(b));
    snprintf(line, sizeof(line), "%s used of %s", a, b);
    wg_text(cx(), g_pane.y + 150, line, wg_ink_colour());
    bar(cx(), g_pane.y + 172, cw(), g_mem.used, g_mem.usable);

    human(g_mem.free, a, sizeof(a));
    snprintf(line, sizeof(line), "%s free", a);
    wg_text(cx(), g_pane.y + 194, line, WG_DIM);
}

static void draw_net(void)
{
    char line[96], a[24];
    if (!g_have_net) {
        wg_text(cx(), g_pane.y + 10, "no network: netd is not answering", WG_DIM);
        return;
    }
    snprintf(a, sizeof(a), "%u.%u.%u.%u", (g_net.ip >> 24) & 0xFF,
             (g_net.ip >> 16) & 0xFF, (g_net.ip >> 8) & 0xFF, g_net.ip & 0xFF);
    snprintf(line, sizeof(line), "address    %s", a);
    wg_text(cx(), g_pane.y + 10, line, wg_ink_colour());
    snprintf(line, sizeof(line), "hardware   %02x:%02x:%02x:%02x:%02x:%02x",
             g_net.mac[0], g_net.mac[1], g_net.mac[2],
             g_net.mac[3], g_net.mac[4], g_net.mac[5]);
    wg_text(cx(), g_pane.y + 30, line, wg_ink_colour());

    /* Said plainly rather than drawn as an idle line: netd counts frames in
     * and out for itself but exports no way to ask, so a throughput chart here
     * would be a picture of zero pretending to be a picture of quiet. */
    wg_container(cx(), g_pane.y + 62, cw(), 60, 8);
    wg_text(cx() + 8, g_pane.y + 74, "throughput is not counted yet", wg_ink_colour());
    wg_text(cx() + 8, g_pane.y + 94, "netd keeps the totals but exports no way to read",
            WG_DIM);
    wg_text(cx() + 8, g_pane.y + 110, "them - a chart here would show zero, not quiet.",
            WG_DIM);
}

static void draw_disk(void)
{
    char line[96], a[24], b[24];
    if (!g_have_root) {
        wg_text(cx(), g_pane.y + 10, "the root filesystem did not answer", WG_DIM);
        return;
    }
    const uint64_t total = g_root.f_blocks * g_root.f_bsize;
    const uint64_t freeb = g_root.f_bfree * g_root.f_bsize;
    human(total - freeb, a, sizeof(a));
    human(total, b, sizeof(b));
    snprintf(line, sizeof(line), "root filesystem: %s of %s used", a, b);
    wg_text(cx(), g_pane.y + 10, line, wg_ink_colour());
    bar(cx(), g_pane.y + 32, cw(), total - freeb, total);

    human(freeb, a, sizeof(a));
    snprintf(line, sizeof(line), "%s free, in blocks of %llu bytes",
             a, (unsigned long long)g_root.f_bsize);
    wg_text(cx(), g_pane.y + 54, line, WG_DIM);

    wg_container(cx(), g_pane.y + 86, cw(), 56, 8);
    wg_text(cx() + 8, g_pane.y + 98, "transfer rates are not counted yet",
            wg_ink_colour());
    wg_text(cx() + 8, g_pane.y + 118, "Disk Utility shows every mounted volume.", WG_DIM);
}

static void draw_gpu(void)
{
    char line[96];
    /* The honest pane. There is no GPU driver, no command queue and no
     * utilisation counter, because the display is a framebuffer the compositor
     * writes to with the processor. Reporting "0%" would be a lie of a
     * particularly bad kind: it looks like a working measurement of an idle
     * device. What is true, and is worth showing, is what the drawing costs. */
    wg_text(cx(), g_pane.y + 10, "no graphics processor is driven", wg_ink_colour());
    wg_container(cx(), g_pane.y + 34, cw(), 96, 8);
    wg_text(cx() + 8, g_pane.y + 46, "The display is a linear framebuffer. Every pixel",
            WG_DIM);
    wg_text(cx() + 8, g_pane.y + 62, "is composited by the window server on the CPU,",
            WG_DIM);
    wg_text(cx() + 8, g_pane.y + 78, "so the cost of graphics appears there.", WG_DIM);

    snprintf(line, sizeof(line), "window server   %u%% of recent slices",
             g_wserver_pct);
    wg_text(cx() + 8, g_pane.y + 102, line, wg_ink_colour());
}

/* --- the interface ---------------------------------------------------------
 *
 * A sidebar of resources and a pane of charts. The charts stay hand-drawn -
 * they are a picture of a number over time, which is what a custom view is
 * for - but everything around them is components now.
 */

static struct app g_app;
static struct ui_view* g_side;

static const char* pane_row(void* user, int row)
{
    (void)user;
    return (row >= 0 && row < PANE_COUNT) ? kPanes[row] : "";
}

/* The charts, into whatever room the layout gave them. */
static void draw_pane(struct ui_view* v, void* user)
{
    (void)user;
    g_pane = v->frame;
    wg_text(g_pane.x, g_pane.y - 20, kPanes[g_pane_index], wg_ink_colour());
    if (g_pane_index == PANE_CPU)       draw_cpu();
    else if (g_pane_index == PANE_MEM)  draw_mem();
    else if (g_pane_index == PANE_NET)  draw_net();
    else if (g_pane_index == PANE_DISK) draw_disk();
    else                                draw_gpu();
}

static void on_pane(struct ui_view* v, void* user)
{
    (void)user;
    if (v->selected >= 0 && v->selected < PANE_COUNT)
        g_pane_index = v->selected;
}

static int on_tick(struct app* a)
{
    (void)a;
    sample();
    return 1;
}

int main(int argc, char** argv)
{
    struct ui_view* root = ui_box(0, UI_STACK_H, 0, 0);
    g_side = ui_sidebar(root, pane_row, PANE_COUNT, 0);
    ui_on(g_side, on_pane, 0);
    ui_size(g_side, 150, 0);
    g_side->selected = 0;

    struct ui_view* pane = ui_box(root, UI_STACK_V, 16, 8);
    ui_size(ui_spacer(pane), 0, 14);        /* room for the pane's heading */
    ui_custom(pane, draw_pane, 0);

    sample();

    g_app.title = "Resource Monitor";
    g_app.width = 660; g_app.height = 420;
    g_app.min_width = 520; g_app.min_height = 340;
    g_app.sidebar = 150;
    /* Twice a second, which is the rate the history is drawn at. */
    g_app.tick_ms = 500;
    g_app.tick = on_tick;
    g_app.root = root;
    return app_run(&g_app, argc, argv);
}
