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

#include <dialog.h>
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

static uint32_t* g_px;
static unsigned  g_w = 640, g_h = 420;

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
static int  g_scroll;                   /* into the report, in lines */

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
    g_scroll = 0;
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

/* Buttons live along the bottom of the content pane. */
#define BTN_W 92
#define BTN_H 26
static int btn_y(void) { return (int)g_h - 12 - BTN_H; }
static int btn_x(int i) { return SIDE_W + 16 + i * (BTN_W + 8); }

static void draw(void)
{
    wg_theme();
    wg_glass_clear();

    /* The sidebar runs the full height, and the volumes are its contents. */
    wg_sidebar(0, 0, SIDE_W, (int)g_h);
    wg_text(14, 12, "Volumes", WG_DIM);
    for (int i = 0; i < g_n; ++i) {
        const int y = 34 + i * ROW_H;
        if (i == g_sel)
            wg_row_select(6, y - 4, SIDE_W - 12, ROW_H);
        wg_text_clipped(14, y, g_vol[i].at, wg_ink_colour(), SIDE_W - 28);
        wg_text_clipped(14, y + 14, g_vol[i].device, WG_DIM, SIDE_W - 28);
    }

    if (g_sel < 0 || g_sel >= g_n) {
        wg_text(SIDE_W + 16, 20, "nothing is mounted", WG_DIM);
        return;
    }
    const struct volume* v = &g_vol[g_sel];
    const int cx = SIDE_W + 16;
    char line[128], a[32], b[32];

    wg_text(cx, 14, v->at, wg_ink_colour());
    snprintf(line, sizeof(line), "%s on %s", v->type, v->device);
    wg_text(cx, 34, line, WG_DIM);

    if (v->have_stat) {
        const uint64_t total = v->st.f_blocks * v->st.f_bsize;
        const uint64_t freeb = v->st.f_bfree * v->st.f_bsize;
        const uint64_t used  = total - freeb;

        /* One bar, because the question a person actually has about a disk is
         * "how much is left", and that is a proportion rather than a number. */
        const int bw = (int)g_w - cx - 16;
        wg_container(cx, 60, bw, 40, 8);
        const int inner = bw - 16;
        wg_fill(cx + 8, 76, inner, 12, WG_PAPER);
        if (total > 0) {
            int fill = (int)((used * (uint64_t)inner) / total);
            if (fill > inner) fill = inner;
            /* Red past nine tenths: at that point the number has stopped being
             * information and started being a warning. */
            wg_fill(cx + 8, 76, fill, 12,
                    used * 10 > total * 9 ? 0xD2413Au : WG_ACCENT);
        }
        human(used, a, sizeof(a));
        human(total, b, sizeof(b));
        snprintf(line, sizeof(line), "%s used of %s", a, b);
        wg_text(cx, 108, line, wg_ink_colour());
        human(freeb, a, sizeof(a));
        snprintf(line, sizeof(line), "%s free  -  %llu blocks of %llu bytes",
                 a, (unsigned long long)v->st.f_blocks,
                 (unsigned long long)v->st.f_bsize);
        wg_text(cx, 126, line, WG_DIM);
    } else {
        wg_text(cx, 68, "not storage - nothing to measure", WG_DIM);
    }

    /* The report, when there is one. */
    const int ry = 152;
    const int rh = btn_y() - ry - 12;
    if (rh > 20) {
        wg_container(cx, ry, (int)g_w - cx - 16, rh, 8);
        int y = ry + 8;
        const char* p = g_report;
        int skip = g_scroll;
        while (*p != '\0' && y + WG_GLYPH_H < ry + rh) {
            char text[128];
            unsigned n = 0;
            while (p[n] != '\0' && p[n] != '\n' && n + 1 < sizeof(text)) {
                text[n] = p[n]; ++n;
            }
            text[n] = '\0';
            p += n + (p[n] == '\n' ? 1 : 0);
            if (skip > 0) { --skip; continue; }
            wg_text_clipped(cx + 8, y, text, wg_ink_colour(),
                            (int)g_w - cx - 32);
            y += WG_GLYPH_H;
        }
        if (g_report[0] == '\0')
            wg_text(cx + 8, ry + 8, "no check has been run", WG_DIM);
    }

    static const char* const kLabels[3] = { "Verify", "Repair", "Detach" };
    for (int i = 0; i < 3; ++i)
        wg_button(btn_x(i), btn_y(), BTN_W, BTN_H, kLabels[i], 0);
    wg_text_clipped(btn_x(3) + 8, btn_y() + 5, g_note, WG_DIM,
                    (int)g_w - btn_x(3) - 24);
}

static const char* const kMenu[] = { "Verify", "Repair", "-", "Detach",
                                     "Refresh" };

int main(int argc, char** argv)
{
    const int wx = argc > 1 ? atoi_simple(argv[1]) : 160;
    const int wy = argc > 2 ? atoi_simple(argv[2]) : 120;
    if (wg_font() != 0)
        return 1;
    const int id = win_create(wx, wy, g_w, g_h, "Disk Utility");
    if (id < 0) {
        printf("diskutil: no window server\n");
        return 1;
    }
    win_set_alpha(id);
    win_set_sidebar(id, SIDE_W);
    g_px = win_map(id);
    if (g_px == 0)
        return 1;
    win_set_min_size(id, 520, 340);
    wg_target(g_px, g_w, g_h);

    reload();
    draw();
    win_present(id);

    unsigned since = 0;
    for (;;) {
        struct win_event e;
        while (win_poll(id, &e)) {
            if (e.type == WIN_EVENT_CLOSE) { win_destroy(id); return 0; }

            if (menu_active() && e.type != WIN_EVENT_RESIZE) {
                const int pick = menu_event(&e);
                if (pick == 0)      verify(0);
                else if (pick == 1) verify(1);
                else if (pick == 3) detach();
                else if (pick == 4) reload();
                draw(); menu_draw(); win_present(id);
                continue;
            }

            if (e.type == WIN_EVENT_RESIZE) {
                g_w = (unsigned)e.x; g_h = (unsigned)e.y;
                g_px = win_map(id);
                if (g_px == 0) return 1;
                wg_target(g_px, g_w, g_h);
            } else if (e.type == WIN_EVENT_MOUSE_DOWN) {
                if (e.button == 2) {
                    menu_open(e.x, e.y, kMenu, 5);
                } else if (e.x < SIDE_W) {
                    const int hit = (e.y - 30) / ROW_H;
                    if (hit >= 0 && hit < g_n) {
                        g_sel = hit;
                        g_report[0] = '\0';
                        g_note[0] = '\0';
                    }
                } else if (e.y >= btn_y() && e.y < btn_y() + BTN_H) {
                    for (int i = 0; i < 3; ++i)
                        if (e.x >= btn_x(i) && e.x < btn_x(i) + BTN_W) {
                            if (i == 0) verify(0);
                            else if (i == 1) verify(1);
                            else detach();
                        }
                }
            } else if (e.type == WIN_EVENT_KEY) {
                if (e.key == WIN_KEY_DOWN && g_sel + 1 < g_n) ++g_sel;
                else if (e.key == WIN_KEY_UP && g_sel > 0) --g_sel;
                else if (e.key == 'v') verify(0);
                else if (e.key == 'r') reload();
                else continue;
            } else {
                continue;
            }
            draw();
            menu_draw();
            win_present(id);
        }

        /* Every few seconds: free space moves while other programs run, and a
         * capacity bar that only updates when clicked is a lie with a delay. */
        if (++since >= 200) {
            since = 0;
            reload();
            draw();
            menu_draw();
            win_present(id);
        }
        msleep(15);
    }
}
