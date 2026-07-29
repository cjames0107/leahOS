/* desktop - the icons behind every other window.
 *
 * It is an ordinary client with one flag set: WS_FLAG_DESKTOP tells the server
 * to draw it without chrome and keep it at the back. Everything else - its
 * pixels, its events, its right-click - works exactly as any window's does,
 * which is why the server needed thirty lines rather than a new concept.
 *
 * Because it covers the screen it also paints the background, wallpaper
 * included: the server's own fill is only what shows when no desktop is
 * running. That keeps one thing responsible for the backdrop instead of two
 * drawing over each other.
 */

#include <bundle.h>
#include <clipboard.h>
#include <display.h>
#include <dialog.h>
#include <image.h>
#include <shm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <widget.h>
#include <window.h>
#include <wproto.h>

#define CELL_W 88
#define CELL_H 76
#define MAX_ICONS 64

static uint32_t* g_px;
static unsigned  g_w, g_h;
static struct ws_shared* g_ws;

static struct dirent g_items[MAX_ICONS];
static int g_n;
static int g_sel = -1;
static char g_marked[MAX_ICONS];
static int  g_anchor = -1;
static char g_dir[128];
static char g_note[96] = "";

static uint32_t* g_paper;
static unsigned  g_paper_w, g_paper_h;
static uint32_t  g_seen = 0xFFFFFFFFu;

static void home_desktop(char* out, unsigned max)
{
    char name[64] = "";
    username(getuid(), name);
    if (strcmp(name, "root") == 0)
        snprintf(out, max, "/root/Desktop");
    else
        snprintf(out, max, "/home/%s/Desktop", name);
}

static void rescan(void)
{
    /* Made on demand: a desktop folder that does not exist is a folder nobody
     * has put anything in yet, not an error. */
    mkdir(g_dir);
    g_n = getdents(g_dir, g_items, MAX_ICONS);
    if (g_n < 0)
        g_n = 0;
    snprintf(g_note, sizeof(g_note), "%s - %d item%s", g_dir, g_n,
             g_n == 1 ? "" : "s");
}

static void reload_paper(void)
{
    if (g_ws == 0)
        return;
    const uint32_t gen = __atomic_load_n(&g_ws->theme.generation, __ATOMIC_ACQUIRE);
    if (gen == g_seen)
        return;
    g_seen = gen;
    if (g_paper != 0) { free(g_paper); g_paper = 0; }
    if (g_ws->theme.wallpaper[0] != '\0')
        g_paper = img_read_png((const char*)g_ws->theme.wallpaper,
                               &g_paper_w, &g_paper_h);
}

static void folder_icon(int x, int y)
{
    wg_fill(x, y + 3, 14, 4, 0xC8A030);
    wg_fill(x, y + 6, 32, 22, 0xE8C860);
    wg_bevel(x, y + 6, 32, 22, 1);
}

static void file_icon(int x, int y, int program)
{
    wg_fill(x + 4, y + 2, 24, 28, program ? 0xB0C0D8 : WG_PAPER);
    wg_bevel(x + 4, y + 2, 24, 28, 1);
    for (int i = 0; i < 6; ++i)
        wg_plot(x + 27 - i, y + 3 + i, WG_DIM);
    for (int line = 0; line < (program ? 1 : 4); ++line)
        wg_fill(x + 8, y + 13 + line * 4, program ? 16 : 14, 1, WG_DIM);
}

static int ends_elf(const char* s)
{
    const int n = (int)strlen(s);
    return n > 4 && s[n-4] == '.' && (s[n-3] == 'E' || s[n-3] == 'e');
}

static void draw(void)
{
    /* The backdrop, then the icons. */
    if (g_paper != 0 && g_paper_w > 0 && g_paper_h > 0) {
        for (unsigned y = 0; y < g_h; ++y) {
            const uint32_t* src = &g_paper[(unsigned long)(y * g_paper_h / g_h)
                                           * g_paper_w];
            uint32_t* dst = &g_px[(unsigned long)y * g_w];
            for (unsigned x = 0; x < g_w; ++x)
                dst[x] = src[x * g_paper_w / g_w];
        }
    } else {
        wg_fill(0, 0, (int)g_w, (int)g_h,
                g_ws ? g_ws->theme.desktop : 0x008080);
    }

    const int cols = ((int)g_w - 16) / CELL_W;
    for (int i = 0; i < g_n && cols > 0; ++i) {
        const int x = 12 + (i % cols) * CELL_W;
        const int y = 12 + (i / cols) * CELL_H;
        if (y + CELL_H > (int)g_h)
            break;
        if (i == g_sel || g_marked[i])
            wg_fill(x - 2, y - 2, CELL_W - 6, CELL_H - 12, 0x4060A0);
        const int app = bundle_is_app(g_items[i].d_name);
        if (g_items[i].d_type == S_IFDIR && !app)
            folder_icon(x + 22, y);
        else
            file_icon(x + 22, y, app || ends_elf(g_items[i].d_name));
        /* Labels are white with a dark shadow, so they stay readable over a
         * wallpaper of any brightness. */
        wg_text_clipped(x + 1, y + 37, g_items[i].d_name, 0x202020, CELL_W - 10);
        wg_text_clipped(x, y + 36, g_items[i].d_name, WG_PAPER, CELL_W - 10);
    }

    /* Nothing is written on the desktop itself. A status line here is a caption
     * on a backdrop: it belongs to no window, cannot be dismissed, and is in
     * the way of the one thing the desktop is for. */
}

static void launch(const char* app, const char* doc)
{
    if (fork() != 0)
        return;
    char* argv[3];
    argv[0] = (char*)app;
    argv[1] = (char*)doc;
    argv[2] = 0;
    execve(app, argv, 0);
    exit(127);
}

static void open_selected(void)
{
    if (g_sel < 0 || g_sel >= g_n)
        return;
    char full[256];
    snprintf(full, sizeof(full), "%s/%s", g_dir, g_items[g_sel].d_name);
    struct bundle b;
    if (bundle_is_app(full)) {
        char exec[256];
        if (bundle_load(full, &b) == 0) {
            bundle_exec(&b, exec, sizeof(exec));
            launch(exec, 0);
        }
    } else if (g_items[g_sel].d_type == S_IFDIR) {
        launch("/BIN/BROWSE.ELF", full);
    } else if (ends_elf(full)) {
        launch(full, 0);
    } else if (bundle_for_document("/Apps", full, &b) == 0) {
        /* Whichever application claims this kind, rather than a name written
         * into the desktop. */
        char exec[256];
        bundle_exec(&b, exec, sizeof(exec));
        launch(exec, full);
    } else {
        launch("/BIN/EDIT.ELF", full);
    }
    snprintf(g_note, sizeof(g_note), "opened %s", g_items[g_sel].d_name);
}

static const char* const kMenu[] = {
    "Open", "-", "New file", "Files here", "Terminal", "Settings", "-", "Refresh"
};

int main(void)
{
    if (wg_font() != 0)
        return 1;
    struct fb_info fb;
    if (fb_info(&fb) != 0)
        return 1;
    g_w = fb.width;
    g_h = fb.height;

    const int cid = shm_open(WS_CONTROL_KEY, 0, 0);
    if (cid >= 0)
        g_ws = (struct ws_shared*)shm_map(cid);

    const int id = win_create(0, 0, g_w, g_h, "Desktop");
    if (id < 0) {
        printf("desktop: no window server\n");
        return 1;
    }
    win_set_desktop(id);
    g_px = win_map(id);
    if (g_px == 0)
        return 1;
    wg_target(g_px, g_w, g_h);

    home_desktop(g_dir, sizeof(g_dir));
    rescan();
    reload_paper();
    draw();
    win_present(id);

    unsigned tick = 0;
    for (;;) {
        struct win_event e;
        while (win_poll(id, &e)) {
            if (e.type == WIN_EVENT_CLOSE) { win_destroy(id); return 0; }

            if (menu_active()) {
                char full[256];
                switch (menu_event(&e)) {
                case 0: open_selected(); break;
                case 2:
                    snprintf(full, sizeof(full), "%s/untitled.txt", g_dir);
                    launch("/BIN/EDIT.ELF", full);
                    break;
                case 3: launch("/BIN/BROWSE.ELF", g_dir); break;
                case 4: launch("/BIN/TERM.ELF", 0); break;
                case 5: launch("/BIN/SETTINGS.ELF", 0); break;
                case 7: rescan(); break;
                }
                draw();
                menu_draw();
                win_present(id);
                continue;
            }

            if (e.type == WIN_EVENT_MOUSE_DOWN) {
                const int cols = ((int)g_w - 16) / CELL_W;
                int hit = -1;
                if (cols > 0 && e.x >= 12 && e.y >= 12) {
                    const int c = (e.x - 12) / CELL_W, r = (e.y - 12) / CELL_H;
                    const int i = r * cols + c;
                    if (c < cols && i >= 0 && i < g_n)
                        hit = i;
                }
                if (e.button == 2) {
                    /* A menu either way: right-clicking bare desktop is still a
                     * question worth answering. */
                    if (hit >= 0 && !g_marked[hit]) { g_sel = hit; g_anchor = hit; }
                    menu_open(e.x, e.y, kMenu, 8);
                } else if (hit >= 0) {
                    const uint32_t m = e.modifiers;
                    if (m & WIN_MOD_CTRL) {
                        g_marked[hit] = (char)!g_marked[hit];
                        g_sel = hit; g_anchor = hit;
                    } else if ((m & WIN_MOD_SHIFT) && g_anchor >= 0) {
                        memset(g_marked, 0, sizeof(g_marked));
                        const int a = g_anchor < hit ? g_anchor : hit;
                        const int b = g_anchor < hit ? hit : g_anchor;
                        for (int i = a; i <= b && i < MAX_ICONS; ++i)
                            g_marked[i] = 1;
                        g_sel = hit;
                    } else if (hit == g_sel) {
                        open_selected();
                    } else {
                        memset(g_marked, 0, sizeof(g_marked));
                        g_marked[hit] = 1;
                        g_sel = hit; g_anchor = hit;
                    }
                } else {
                    /* Clicking the bare desktop lets go of everything. */
                    memset(g_marked, 0, sizeof(g_marked));
                    g_sel = -1;
                }
            } else if (e.type == WIN_EVENT_KEY) {
                const int cols = ((int)g_w - 16) / CELL_W;
                if (e.key == '\n' || e.key == '\r') open_selected();
                else if (e.key == 'r') rescan();
                else if (e.key == WIN_KEY_RIGHT && g_sel + 1 < g_n) ++g_sel;
                else if (e.key == WIN_KEY_LEFT && g_sel > 0) --g_sel;
                else if (e.key == WIN_KEY_DOWN && g_sel + cols < g_n)
                    g_sel += cols;
                else if (e.key == WIN_KEY_UP && g_sel - cols >= 0)
                    g_sel -= cols;
            } else {
                continue;
            }
            draw();
            menu_draw();
            win_present(id);
        }

        /* The folder is shared with everything else; noticing a change costs
         * one directory read a second and saves needing to be told. */
        if (++tick >= 66) {
            tick = 0;
            reload_paper();
            rescan();
            draw();
            win_present(id);
        }
        msleep(15);
    }
}
