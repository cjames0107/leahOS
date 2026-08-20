/* diskutil - what is mounted, how full it is, and whether it is sound.
 *
 * Everything here already existed as a command: df reads statfs, mount reads
 * /proc/mounts, fsck asks vfsd to check itself. What it did not have was one
 * place showing all three at once, which is the thing that makes a disk problem
 * visible - a filesystem that is nearly full and a filesystem that is damaged
 * look identical from a shell until you go looking for each separately.
 *
 * It deliberately cannot format or partition. Those are the two operations a
 * disk utility is expected to have and the two that destroy data when they go
 * wrong, and neither has a tested implementation underneath it here. An
 * application that offers a button for something it cannot do properly is
 * worse than one that does not offer it.
 */

#include <app.h>
#include <ui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statfs.h>
#include <unistd.h>
#include <widget.h>
#include <window.h>

#define SIDE_W  180
#define MAX_FS  12
#define ROW_H   34

struct volume {
    char device[32];
    char at[64];
    char type[16];
    struct statfs st;
    int  have_stat;
};

static struct volume g_vol[MAX_FS];
static int g_n;
static int g_sel;
static char g_report[1024] = "";
static char g_note[96] = "";

/* /proc/mounts is the list, because it is what the system itself believes.
 * Asking vfsd directly would mean a second answer to the same question, and
 * two answers that can differ is how a utility ends up showing a volume that
 * was unmounted a second ago. */
static void reload(void)
{
    g_n = 0;
    FILE* in = fopen("/proc/mounts", "r");
    if (in == 0)
        return;
    char line[192];
    while (g_n < MAX_FS && fgets(line, sizeof(line), in) != 0) {
        /* "<device> on <point> type <type> (<flags>)" - four fields matter and
         * the rest is decoration. */
        char dev[64], on[8], at[96], word[8], type[32];
        if (sscanf(line, "%63s %7s %95s %7s %31s",
                   dev, on, at, word, type) != 5)
            continue;
        struct volume* v = &g_vol[g_n++];
        memset(v, 0, sizeof(*v));
        snprintf(v->device, sizeof(v->device), "%s", dev);
        snprintf(v->at, sizeof(v->at), "%s", at);
        snprintf(v->type, sizeof(v->type), "%s", type);
        /* A filesystem with no blocks is not storage - /proc says so by
         * reporting zeros, and a capacity bar for it would be an invention. */
        v->have_stat = statfs(v->at, &v->st) == 0 && v->st.f_blocks > 0;
    }
    fclose(in);
    if (g_sel >= g_n)
        g_sel = g_n > 0 ? g_n - 1 : 0;
}

static void human(uint64_t bytes, char* out, unsigned max)
{
    if (bytes >= (1ull << 30))
        snprintf(out, max, "%llu.%llu GiB", (unsigned long long)(bytes >> 30),
                 (unsigned long long)((bytes >> 20) % 1024) * 10 / 1024);
    else if (bytes >= (1ull << 20))
        snprintf(out, max, "%llu MiB", (unsigned long long)(bytes >> 20));
    else
        snprintf(out, max, "%llu KiB", (unsigned long long)(bytes >> 10));
}

/* Check the filesystem, and keep what it said.
 *
 * fsck reports as lines of text rather than a number, so the report is shown
 * as it arrives instead of being summarised into a word. "Clean" is a fact
 * about a filesystem; "3 problems" is a fact somebody has to act on, and they
 * need to read what the three were. */
static void verify(int repair)
{
    unsigned fixed = 0;
    const long problems = fsck(repair, g_report, sizeof(g_report), &fixed);
    if (problems < 0) {
        snprintf(g_note, sizeof(g_note), "the filesystem could not be checked");
        return;
    }
    if (problems == 0)
        snprintf(g_note, sizeof(g_note), "clean");
    else if (repair)
        snprintf(g_note, sizeof(g_note), "%ld problem(s), %u repaired",
                 problems, fixed);
    else
        snprintf(g_note, sizeof(g_note), "%ld problem(s) - repair to fix them",
                 problems);
}

static void detach(void)
{
    if (g_sel < 0 || g_sel >= g_n)
        return;
    /* The root is not detachable, and saying so is better than letting vfsd
     * refuse and reporting its refusal as a mystery. */
    if (strcmp(g_vol[g_sel].at, "/") == 0) {
        snprintf(g_note, sizeof(g_note), "the root filesystem stays mounted");
        return;
    }
    if (fs_umount(g_vol[g_sel].at) == 0)
        snprintf(g_note, sizeof(g_note), "detached %s", g_vol[g_sel].at);
    else
        snprintf(g_note, sizeof(g_note), "%s would not detach",
                 g_vol[g_sel].at);
    reload();
}

/* --- the interface ---------------------------------------------------------
 *
 * A sidebar of volumes and a pane about the chosen one: what it is, how full,
 * and what a check of it said. The capacity is a level rather than a bar drawn
 * by hand, and the report is a list, so a long one scrolls.
 */

static struct app g_app;
static struct ui_view* g_side;
static struct ui_view* g_where;
static struct ui_view* g_what;
static struct ui_view* g_bar;
static struct ui_view* g_used_label;
static struct ui_view* g_free_label;
static struct ui_view* g_report_list;
static struct ui_view* g_note_label;

/* The report, cut into lines once, because a row callback that walked the
 * text would walk it again per visible row per repaint. */
#define REPORT_MAX 40
static char g_report_line[REPORT_MAX][128];
static int  g_report_n;

static void split_report(void)
{
    g_report_n = 0;
    const char* p = g_report;
    while (*p != '\0' && g_report_n < REPORT_MAX) {
        unsigned n = 0;
        while (p[n] != '\0' && p[n] != '\n' &&
               n + 1 < sizeof(g_report_line[0])) {
            g_report_line[g_report_n][n] = p[n];
            ++n;
        }
        g_report_line[g_report_n][n] = '\0';
        if (n > 0) ++g_report_n;
        p += n;
        if (*p == '\n') ++p;
    }
    if (g_report_list != 0)
        g_report_list->rows = g_report_n;
}

static const char* report_row(void* user, int row)
{
    (void)user;
    return (row >= 0 && row < g_report_n) ? g_report_line[row] : "";
}

static const char* volume_row(void* user, int row)
{
    (void)user;
    return (row >= 0 && row < g_n) ? g_vol[row].at : "";
}

static void human(uint64_t bytes, char* out, unsigned max);

/* Everything the pane says about whichever volume is chosen. */
static void sync_pane(void)
{
    if (g_side != 0)
        g_side->rows = g_n;
    if (g_sel < 0 || g_sel >= g_n) {
        ui_set_text(g_where, "nothing is mounted");
        ui_set_text(g_what, "");
        ui_set_text(g_used_label, "");
        ui_set_text(g_free_label, "");
        g_bar->value = 0;
        ui_set_text(g_note_label, g_note);
        return;
    }
    const struct volume* v = &g_vol[g_sel];
    char line[128], a[32], b[32];
    ui_set_text(g_where, v->at);
    snprintf(line, sizeof(line), "%s on %s", v->type, v->device);
    ui_set_text(g_what, line);

    if (v->have_stat) {
        const uint64_t total = v->st.f_blocks * v->st.f_bsize;
        const uint64_t freeb = v->st.f_bfree * v->st.f_bsize;
        const uint64_t used  = total - freeb;
        /* A level, not a bar: how full a disk is is exactly what a level is
         * for, and it already turns red at the end that matters. */
        g_bar->max = 1000;
        g_bar->value = total > 0 ? (int)((total - used) * 1000 / total) : 0;
        human(used, a, sizeof(a));
        human(total, b, sizeof(b));
        snprintf(line, sizeof(line), "%s used of %s", a, b);
        ui_set_text(g_used_label, line);
        human(freeb, a, sizeof(a));
        snprintf(line, sizeof(line), "%s free, in blocks of %llu bytes", a,
                 (unsigned long long)v->st.f_bsize);
        ui_set_text(g_free_label, line);
    } else {
        g_bar->value = 0;
        ui_set_text(g_used_label, "not storage - nothing to measure");
        ui_set_text(g_free_label, "");
    }
    ui_set_text(g_note_label, g_note);
    split_report();
    app_relayout(&g_app);
}

static void on_volume(struct ui_view* v, void* user)
{
    (void)user;
    if (v->selected < 0 || v->selected >= g_n)
        return;
    g_sel = v->selected;
    g_report[0] = '\0';
    g_note[0] = '\0';
    sync_pane();
}

static void on_verify(struct ui_view* v, void* user)
{
    (void)v; (void)user;
    verify(0);
    sync_pane();
}

static void on_repair(struct ui_view* v, void* user)
{
    (void)v; (void)user;
    verify(1);
    sync_pane();
}

static void on_detach(struct ui_view* v, void* user)
{
    (void)v; (void)user;
    detach();
    sync_pane();
}

static int on_tick(struct app* a)
{
    (void)a;
    /* Free space moves while other programs run, and a capacity that only
     * updates when clicked is a lie with a delay. */
    reload();
    sync_pane();
    return 1;
}

int main(int argc, char** argv)
{
    reload();

    struct ui_view* root = ui_box(0, UI_STACK_H, 0, 0);
    g_side = ui_sidebar(root, volume_row, g_n, 0);
    ui_on(g_side, on_volume, 0);
    ui_size(g_side, 180, 0);
    g_side->selected = g_n > 0 ? 0 : -1;

    struct ui_view* pane = ui_box(root, UI_STACK_V, 16, 8);
    g_where = ui_label(pane, ""); ui_grow(g_where, 0);
    g_what  = ui_label(pane, ""); ui_grow(g_what, 0);
    g_bar   = ui_level(pane, 0, 1000, 0);
    ui_size(g_bar, 0, 16); ui_grow(g_bar, 0);
    g_used_label = ui_label(pane, ""); ui_grow(g_used_label, 0);
    g_free_label = ui_label(pane, ""); ui_grow(g_free_label, 0);

    ui_grow(ui_separator(pane), 0);
    g_report_list = ui_list(pane, report_row, 0, 0);

    struct ui_view* row = ui_box(pane, UI_STACK_H, 0, 8);
    ui_size(row, 0, 26); ui_grow(row, 0);
    ui_grow(ui_button(row, "Verify", on_verify, 0), 0);
    ui_grow(ui_button(row, "Repair", on_repair, 0), 0);
    ui_grow(ui_button(row, "Detach", on_detach, 0), 0);
    ui_spacer(row);
    g_note_label = ui_label(pane, ""); ui_grow(g_note_label, 0);

    g_app.title = "Disk Utility";
    g_app.width = 640; g_app.height = 420;
    g_app.min_width = 520; g_app.min_height = 340;
    g_app.sidebar = 180;
    g_app.tick_ms = 3000;
    g_app.tick = on_tick;
    g_app.root = root;
    return app_run(&g_app, argc, argv);
}
