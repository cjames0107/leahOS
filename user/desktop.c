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
#include <fcntl.h>
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
/* Where each icon sits, in desktop pixels rather than in a grid.
 *
 * A grid is easier to write and worse to use: it decides for you, and the only
 * arrangement it can express is the one it already chose. Free positions are
 * saved by name, so a folder that appears and disappears comes back where it
 * was rather than shuffling everything after it along one place. */
static int g_ix[MAX_ICONS], g_iy[MAX_ICONS];
static int g_n;
static int g_sel = -1;
static int g_enter_armed;   /* the first Enter of a double press */
static int g_band, g_band_x, g_band_y, g_band_x2, g_band_y2;
static int g_press_item = -1;
static int g_press_x, g_press_y;
static int g_window_id = -1;

static int being_dragged(int i);
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

/* --- where the icons are ---------------------------------------------------
 *
 * Kept in a file beside the folder's contents rather than in it: a position is
 * about this desktop, not about the file, and writing it into the file would
 * change something the user did not ask to have changed.
 */
#define PLACES_MAX 128
static char g_place_name[PLACES_MAX][64];
static int  g_place_x[PLACES_MAX], g_place_y[PLACES_MAX];
static int  g_places;

static void places_path(char* out, unsigned max)
{
    char name[64] = "";
    username(getuid(), name);
    if (strcmp(name, "root") == 0)
        snprintf(out, max, "/root/.leahdesk");
    else
        snprintf(out, max, "/home/%s/.leahdesk", name);
}

static int place_find(const char* name)
{
    for (int i = 0; i < g_places; ++i)
        if (strcmp(g_place_name[i], name) == 0)
            return i;
    return -1;
}

static void place_set(const char* name, int x, int y)
{
    int i = place_find(name);
    if (i < 0) {
        if (g_places >= PLACES_MAX)
            return;
        i = g_places++;
        snprintf(g_place_name[i], sizeof(g_place_name[i]), "%s", name);
    }
    g_place_x[i] = x;
    g_place_y[i] = y;
}

static void places_load(void)
{
    g_places = 0;
    char path[128];
    places_path(path, sizeof(path));
    const int fd = open(path, O_RDONLY);
    if (fd < 0)
        return;
    static char buf[4096];
    const int len = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (len <= 0)
        return;
    buf[len] = '\0';

    int i = 0;
    while (i < len && g_places < PLACES_MAX) {
        /* "x y name", name last so it may contain spaces. */
        int x = 0, y = 0;
        while (i < len && buf[i] == ' ') ++i;
        while (i < len && buf[i] >= '0' && buf[i] <= '9') x = x * 10 + buf[i++] - '0';
        while (i < len && buf[i] == ' ') ++i;
        while (i < len && buf[i] >= '0' && buf[i] <= '9') y = y * 10 + buf[i++] - '0';
        while (i < len && buf[i] == ' ') ++i;
        int k = 0;
        char name[64];
        while (i < len && buf[i] != '\n' && k < 63) name[k++] = buf[i++];
        name[k] = '\0';
        while (i < len && buf[i] == '\n') ++i;
        if (k > 0)
            place_set(name, x, y);
    }
}

static void places_save(void)
{
    char path[128];
    places_path(path, sizeof(path));
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return;
    for (int i = 0; i < g_places; ++i) {
        char line[128];
        const int n = snprintf(line, sizeof(line), "%d %d %s\n",
                               g_place_x[i], g_place_y[i], g_place_name[i]);
        write(fd, line, (unsigned)n);
    }
    close(fd);
}

/* Somewhere free for an icon nobody has placed yet. The grid is only used for
 * this - as a starting suggestion, not as the arrangement. */
static void auto_place(int index, int* out_x, int* out_y)
{
    const int rows = ((int)g_h - 24) / CELL_H;
    for (int slot = 0; slot < MAX_ICONS; ++slot) {
        const int x = 12 + (slot / (rows > 0 ? rows : 1)) * CELL_W;
        const int y = 12 + (slot % (rows > 0 ? rows : 1)) * CELL_H;
        int taken = 0;
        for (int j = 0; j < index; ++j)
            if (g_ix[j] == x && g_iy[j] == y) { taken = 1; break; }
        if (!taken) { *out_x = x; *out_y = y; return; }
    }
    *out_x = 12;
    *out_y = 12;
}

static void rescan(void)
{
    /* Made on demand: a desktop folder that does not exist is a folder nobody
     * has put anything in yet, not an error. */
    mkdir(g_dir);
    g_n = getdents(g_dir, g_items, MAX_ICONS);
    if (g_n < 0)
        g_n = 0;

    int placed = 0;
    for (int i = 0; i < g_n; ++i) {
        const int p = place_find(g_items[i].d_name);
        if (p >= 0) {
            g_ix[i] = g_place_x[p];
            g_iy[i] = g_place_y[p];
        } else {
            auto_place(i, &g_ix[i], &g_iy[i]);
            place_set(g_items[i].d_name, g_ix[i], g_iy[i]);
            placed = 1;
        }
    }
    if (placed)
        places_save();

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

    for (int i = 0; i < g_n; ++i) {
        if (being_dragged(i))
            continue;               /* it is in the air, not on the desktop */
        const int x = g_ix[i];
        const int y = g_iy[i];
        if (i == g_sel || g_marked[i])
            wg_fill(x - 2, y - 2, CELL_W - 6, CELL_H - 12, 0x4060A0);
        const int app = bundle_is_app(g_items[i].d_name);
        const int link = alias_is(g_items[i].d_name);
        if (g_items[i].d_type == S_IFDIR && !app)
            folder_icon(x + 22, y);
        else
            file_icon(x + 22, y, app || link || ends_elf(g_items[i].d_name));
        /* Labels are white with a dark shadow, so they stay readable over a
         * wallpaper of any brightness. */
        char label[64];
        snprintf(label, sizeof(label), "%s", g_items[i].d_name);
        if (link) {
            const int n = (int)strlen(label);
            if (n > 6) label[n - 6] = '\0';     /* drop ".alias" */
        }
        wg_text_clipped(x + 1, y + 37, label, 0x202020, CELL_W - 10);
        wg_text_clipped(x, y + 36, label, WG_PAPER, CELL_W - 10);
    }

    /* The rubber band, over the icons it is choosing. */
    if (g_band) {
        const int x0 = g_band_x < g_band_x2 ? g_band_x : g_band_x2;
        const int x1 = g_band_x < g_band_x2 ? g_band_x2 : g_band_x;
        const int y0 = g_band_y < g_band_y2 ? g_band_y : g_band_y2;
        const int y1 = g_band_y < g_band_y2 ? g_band_y2 : g_band_y;
        for (int x = x0; x <= x1; x += 3) {
            wg_plot(x, y0, WG_PAPER);
            wg_plot(x, y1, WG_PAPER);
        }
        for (int y = y0; y <= y1; y += 3) {
            wg_plot(x0, y, WG_PAPER);
            wg_plot(x1, y, WG_PAPER);
        }
    }

    /* Nothing is written on the desktop itself. A status line here is a caption
     * on a backdrop: it belongs to no window, cannot be dismissed, and is in
     * the way of the one thing the desktop is for. */
}

/* Which icon is under a point. Free placement means icons can be put close
 * enough to overlap, so the later one wins - the same rule the drawing uses,
 * which is what makes "click the one you can see" true. */
static int icon_at(int x, int y)
{
    for (int i = g_n - 1; i >= 0; --i) {
        if (x >= g_ix[i] - 2 && x < g_ix[i] + CELL_W - 8 &&
            y >= g_iy[i] - 2 && y < g_iy[i] + CELL_H - 12)
            return i;
    }
    return -1;
}

static unsigned drag_icon_for(int i)
{
    if (bundle_is_app(g_items[i].d_name))
        return WS_DRAG_APP;
    return g_items[i].d_type == S_IFDIR ? WS_DRAG_FOLDER : WS_DRAG_FILE;
}

static int being_dragged(int i)
{
    if (!win_dragging() || i < 0 || i >= g_n)
        return 0;
    char full[256];
    snprintf(full, sizeof(full), "%s/%s", g_dir, g_items[i].d_name);
    return strcmp(full, win_drag_path()) == 0;
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
    /* An alias stands for something else; follow it before deciding what
     * opening it means. One hop only - an alias to an alias is a mistake worth
     * seeing rather than quietly chasing. */
    char target[256];
    if (alias_target(full, target, sizeof(target)) == 0) {
        int k = 0;
        while (target[k] != '\0' && k < 255) { full[k] = target[k]; ++k; }
        full[k] = '\0';
    }

    struct bundle b;
    if (bundle_is_app(full)) {
        char exec[256];
        if (bundle_load(full, &b) == 0) {
            bundle_exec(&b, exec, sizeof(exec));
            launch(exec, 0);
        }
    } else if (g_items[g_sel].d_type == S_IFDIR) {
        launch(app_path("Files"), full);
    } else if (ends_elf(full)) {
        launch(full, 0);
    } else if (bundle_for_document("/Apps", full, &b) == 0) {
        /* Whichever application claims this kind, rather than a name written
         * into the desktop. */
        char exec[256];
        bundle_exec(&b, exec, sizeof(exec));
        launch(exec, full);
    } else {
        launch(app_path("Edit"), full);
    }
    snprintf(g_note, sizeof(g_note), "opened %s", g_items[g_sel].d_name);
}

static const char* const kMenu[] = {
    "Open", "Rename", "-", "New file", "Files here", "Terminal", "Settings",
    "-", "Refresh"
};

/* Renaming borrows the save dialogue: it already asks "which folder, what
 * name", which is exactly the question. */
static int  g_renaming;
static char g_rename_from[256];

static void begin_rename(void)
{
    if (g_sel < 0 || g_sel >= g_n)
        return;
    snprintf(g_rename_from, sizeof(g_rename_from), "%s/%s", g_dir,
             g_items[g_sel].d_name);
    g_renaming = 1;
    dlg_save(g_dir, g_items[g_sel].d_name);
}

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
    g_window_id = id;
    win_set_desktop(id);
    g_px = win_map(id);
    if (g_px == 0)
        return 1;
    wg_target(g_px, g_w, g_h);

    home_desktop(g_dir, sizeof(g_dir));
    places_load();
    rescan();
    reload_paper();
    draw();
    win_present(id);

    unsigned tick = 0;
    int was_dragging = 0;
    for (;;) {
        /* A drag that ended in another window still took something out of
         * this one, and nothing tells us but the drag going away. */
        const int dragging_now = win_dragging();
        if (was_dragging && !dragging_now) {
            rescan();
            draw();
            win_present(id);
        }
        was_dragging = dragging_now;

        struct win_event e;
        while (win_poll(id, &e)) {
            if (e.type == WIN_EVENT_CLOSE) { win_destroy(id); return 0; }

            /* While a dialogue is up it takes the input: a click meant for it
             * must not also land on whatever icon is underneath. */
            if (dlg_active()) {
                const int answer = dlg_event(&e);
                if (answer == DLG_ACCEPT && g_renaming) {
                    g_renaming = 0;
                    rename(g_rename_from, dlg_path());
                    rescan();
                } else if (answer == DLG_CANCEL) {
                    g_renaming = 0;
                }
                draw();
                dlg_draw((int)g_w, (int)g_h);
                win_present(id);
                continue;
            }

            if (menu_active()) {
                char full[256];
                switch (menu_event(&e)) {
                case 0: open_selected(); break;
                case 1: begin_rename(); break;
                case 3:
                    snprintf(full, sizeof(full), "%s/untitled.txt", g_dir);
                    launch(app_path("Edit"), full);
                    break;
                case 4: launch(app_path("Files"), g_dir); break;
                case 5: launch(app_path("Terminal"), 0); break;
                case 6: launch(app_path("Settings"), 0); break;
                case 8: rescan(); break;
                }
                draw();
                menu_draw();
                dlg_draw((int)g_w, (int)g_h);
                win_present(id);
                continue;
            }

            if (e.type == WIN_EVENT_MOUSE_DOWN) {
                const int hit = icon_at(e.x, e.y);
                if (e.button == 2) {
                    /* A menu either way: right-clicking bare desktop is still a
                     * question worth answering. */
                    if (hit >= 0 && !g_marked[hit]) { g_sel = hit; g_anchor = hit; }
                    menu_open(e.x, e.y, kMenu, 9);
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
                    /* Clicking the bare desktop lets go of everything, and
                     * dragging from there chooses a region rather than doing
                     * nothing at all. */
                    memset(g_marked, 0, sizeof(g_marked));
                    g_sel = -1;
                    g_band = 1;
                    g_band_x = g_band_x2 = e.x;
                    g_band_y = g_band_y2 = e.y;
                }
                if (hit >= 0 && e.button == 1) {
                    g_press_item = hit;
                    g_press_x = e.x;
                    g_press_y = e.y;
                }
            } else if (e.type == WIN_EVENT_MOUSE_MOVE) {
                if (g_band) {
                    g_band_x2 = e.x;
                    g_band_y2 = e.y;
                    const int x0 = g_band_x < g_band_x2 ? g_band_x : g_band_x2;
                    const int x1 = g_band_x < g_band_x2 ? g_band_x2 : g_band_x;
                    const int y0 = g_band_y < g_band_y2 ? g_band_y : g_band_y2;
                    const int y1 = g_band_y < g_band_y2 ? g_band_y2 : g_band_y;
                    memset(g_marked, 0, sizeof(g_marked));
                    for (int i = 0; i < g_n; ++i) {
                        /* Any overlap counts, rather than requiring the icon
                         * to be swallowed whole: a band that has to contain
                         * something is a band you have to be careful with. */
                        const int ix1 = g_ix[i] + CELL_W - 8;
                        const int iy1 = g_iy[i] + CELL_H - 12;
                        if (g_ix[i] <= x1 && ix1 >= x0 &&
                            g_iy[i] <= y1 && iy1 >= y0)
                            g_marked[i] = 1;
                    }
                } else if (g_press_item >= 0 && !win_dragging()) {
                    const int dx = e.x - g_press_x, dy = e.y - g_press_y;
                    if (dx * dx + dy * dy > 25) {
                        const int i = g_press_item;
                        char full[256];
                        snprintf(full, sizeof(full), "%s/%s", g_dir,
                                 g_items[i].d_name);
                        int ox, oy;
                        win_origin(g_window_id, &ox, &oy);
                        /* Where the press was, not where the pointer had got
                         * to when the threshold was crossed: otherwise the
                         * ghost jumps by the length of the first movement. */
                        win_drag_begin(full, g_items[i].d_name, drag_icon_for(i),
                                       g_press_x - g_ix[i], g_press_y - g_iy[i],
                                       ox + g_ix[i], oy + g_iy[i]);
                        g_press_item = -1;
                    }
                }
            } else if (e.type == WIN_EVENT_MOUSE_UP) {
                g_band = 0;
                g_press_item = -1;
            } else if (e.type == WIN_EVENT_DROP) {
                /* Two quite different things arrive here. Something already on
                 * the desktop is being rearranged, and only its position
                 * changes. Something from elsewhere is being moved in, and
                 * the file has to follow. */
                char from[256];
                snprintf(from, sizeof(from), "%s", win_drop_path());

                int last = -1;
                for (int i = 0; from[i] != '\0'; ++i)
                    if (from[i] == '/') last = i;
                const char* name = last >= 0 ? &from[last + 1] : from;

                char dest[256];
                snprintf(dest, sizeof(dest), "%s/%s", g_dir, name);

                /* Where the ghost's corner is, not where the cursor is: the
                 * icon should land under the hand exactly as it was picked
                 * up, not jump so its corner meets the pointer. */
                const int nx = e.x - (g_ws ? g_ws->drag.grab_x : 0);
                const int ny = e.y - (g_ws ? g_ws->drag.grab_y : 0);

                int ok = 1;
                if (strcmp(from, dest) != 0)
                    ok = rename(from, dest) == 0;
                if (ok) {
                    place_set(name, nx < 0 ? 0 : nx, ny < 0 ? 0 : ny);
                    places_save();
                    rescan();
                    int ox, oy;
                    win_origin(g_window_id, &ox, &oy);
                    const int at = place_find(name);
                    win_drop_accept(ox + g_place_x[at], oy + g_place_y[at]);
                } else {
                    win_drop_reject();
                }
                g_press_item = -1;
            } else if (e.type == WIN_EVENT_KEY) {
                const int cols = ((int)g_w - 16) / CELL_W;
                if (e.key != '\n' && e.key != '\r')
                    g_enter_armed = 0;
                /* Two presses to open, one to settle on an icon: the same rule
                 * the mouse follows, so the keyboard needs no rule of its own
                 * and a stray Enter cannot launch something. */
                if (e.key == '\n' || e.key == '\r') {
                    if (g_enter_armed) { g_enter_armed = 0; open_selected(); }
                    else                 g_enter_armed = 1;
                }
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
            dlg_draw((int)g_w, (int)g_h);
            win_present(id);
        }

        /* The folder is shared with everything else; noticing a change costs
         * one directory read a second and saves needing to be told.
         *
         * Not while a menu is open, though. This refresh repainted and
         * presented without redrawing the menu, so an open menu was wiped off
         * the screen roughly once a second - which looks random from the
         * outside and is not: it is this timer. Holding the refresh is the
         * better fix than repainting the menu over it, because rescanning would
         * also move the selection the menu was opened about. */
        if (++tick >= 66 && !menu_active() && !dlg_active() &&
            !win_dragging()) {
            tick = 0;
            reload_paper();
            rescan();
            draw();
            menu_draw();
            dlg_draw((int)g_w, (int)g_h);
            win_present(id);
        }
        msleep(15);
    }
}
