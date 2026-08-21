#include <bundle.h>
#include <dialog.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <widget.h>

#define KIND_NONE 0
#define KIND_SAVE 1
#define KIND_OPEN 2

#define PANEL_W 400
#define PANEL_H 300
#define ROW_H   16
#define ROWS    9

/* Icon view geometry, when the save dialogue is showing one. */
#define CELL_W  84
#define CELL_H  50

static int  g_kind;
static char g_dir[256] = "/";
static char g_name[128];
static char g_result[384];
static char g_subject[256];
static int  g_sel = -1;
static int  g_scroll;           /* first visible row, or row of cells */
static int  g_always;           /* the open-with checkbox */
static int  g_icons;            /* save dialogue view: 0 list, 1 icons */

/* A save dialogue is a place-chooser, and choosing a place means wandering.
 * So it gets the same back and forward its bigger sibling has, over the same
 * stack-with-a-cursor. */
#define DLG_HIST 24
static char g_hist[DLG_HIST][256];
static int  g_hist_n;
static int  g_hist_at = -1;

/* The listing: directories to navigate into for a save, applications to choose
 * from for an open-with. One array serves both because both are "a list of
 * short names you pick one of". */
static char g_items[64][64];
static int  g_item_dir[64];
static int  g_items_n;

/* Where the panel sits, recomputed on every draw so a resize cannot leave the
 * hit boxes behind. */
static int g_px, g_py;

static void copy(char* dst, const char* src, int max)
{
    int n = 0;
    while (src[n] != '\0' && n < max - 1) { dst[n] = src[n]; ++n; }
    dst[n] = '\0';
}

static void join(const char* dir, const char* name, char* out, int max)
{
    int n = 0;
    for (const char* p = dir; *p && n < max - 2; ++p) out[n++] = *p;
    if (n > 0 && out[n - 1] != '/') out[n++] = '/';
    for (const char* p = name; *p && n < max - 1; ++p) out[n++] = *p;
    out[n] = '\0';
}

static void parent_of(const char* path, char* out, int max)
{
    int n = (int)strlen(path);
    while (n > 1 && path[n - 1] == '/') --n;
    while (n > 1 && path[n - 1] != '/') --n;
    while (n > 1 && path[n - 1] == '/') --n;
    if (n < 1) n = 1;
    if (n > max - 1) n = max - 1;
    for (int i = 0; i < n; ++i) out[i] = path[i];
    out[n] = '\0';
}

/* Directories first and then the files. The files are listed because seeing
 * what is already in a folder is how you decide whether you are about to
 * overwrite something - and clicking one adopts its name, which is what every
 * save dialogue does and what nobody has to be taught. */
static void list_dirs(void)
{
    static struct dirent scratch[64];
    g_items_n = 0;
    g_sel = -1;
    g_scroll = 0;
    int n = getdents(g_dir, scratch, 64);
    if (n < 0) n = 0;
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < n && g_items_n < 64; ++i) {
            const int is_dir = scratch[i].d_type == S_IFDIR;
            if (is_dir != (pass == 0) || scratch[i].d_name[0] == '.')
                continue;
            copy(g_items[g_items_n], scratch[i].d_name, 64);
            g_item_dir[g_items_n++] = is_dir;
        }
}

static void dlg_remember(const char* path)
{
    if (g_hist_at >= 0 && strcmp(g_hist[g_hist_at], path) == 0)
        return;
    if (g_hist_at + 1 < g_hist_n)
        g_hist_n = g_hist_at + 1;       /* a new turning drops what was ahead */
    if (g_hist_n >= DLG_HIST) {
        for (int i = 1; i < DLG_HIST; ++i)
            memcpy(g_hist[i - 1], g_hist[i], sizeof(g_hist[0]));
        --g_hist_n;
        --g_hist_at;
    }
    copy(g_hist[g_hist_n], path, 256);
    g_hist_at = g_hist_n++;
}

static void dlg_goto(const char* path, int record)
{
    copy(g_dir, path, sizeof(g_dir));
    if (record)
        dlg_remember(g_dir);
    list_dirs();
}

/* The openers this system has. Kept as a table rather than discovered, because
 * "which programs can open a file" is not something the filesystem records. */
/* The installed applications, asked for by walking /Apps - not a list written
 * here. A dialogue that hardcoded three names would be wrong the moment an
 * eleventh application was installed, and would be the last place anyone
 * thought to look. */
static char g_opener_exec[16][192];

static void list_openers(const char* path)
{
    static struct dirent kids[64];
    g_items_n = 0;
    g_scroll = 0;
    const int n = getdents(BUNDLE_DIR, kids, 64);
    for (int i = 0; i < n && g_items_n < 15; ++i) {
        if (!bundle_is_app(kids[i].d_name))
            continue;
        char dir[256];
        snprintf(dir, sizeof(dir), "%s/%s", BUNDLE_DIR, kids[i].d_name);
        struct bundle b;
        if (bundle_load(dir, &b) != 0)
            continue;
        copy(g_items[g_items_n], b.name, 64);
        bundle_exec(&b, g_opener_exec[g_items_n], 192);
        g_item_dir[g_items_n++] = 0;
    }
    /* A program can also just be run. Offered last so it is a deliberate
     * choice rather than the default. */
    const int len = (int)strlen(path);
    if (len > 4 && (path[len-4] == '.') &&
        (path[len-3] == 'E' || path[len-3] == 'e')) {
        copy(g_items[g_items_n], "Run it", 64);
        copy(g_opener_exec[g_items_n], path, 192);
        g_item_dir[g_items_n++] = 0;
    }
    g_sel = 0;
}

void dlg_save(const char* where, const char* suggested)
{
    g_kind = KIND_SAVE;
    copy(g_name, suggested ? suggested : "untitled", sizeof(g_name));
    g_hist_n = 0;
    g_hist_at = -1;
    dlg_goto(where && where[0] ? where : "/", 1);
}

int dlg_always(void) { return g_always; }

void dlg_open_with(const char* path)
{
    g_kind = KIND_OPEN;
    g_always = 0;
    copy(g_subject, path, sizeof(g_subject));
    list_openers(path);
}

int dlg_active(void) { return g_kind != KIND_NONE; }
const char* dlg_path(void) { return g_result; }
const char* dlg_subject(void) { return g_subject; }

static void accept(void)
{
    if (g_kind == KIND_SAVE) {
        join(g_dir, g_name, g_result, sizeof(g_result));
    } else {
        /* Whatever the chosen row resolved to when the list was built. */
        if (g_sel >= 0 && g_sel < g_items_n && g_opener_exec[g_sel][0] != '\0')
            copy(g_result, g_opener_exec[g_sel], sizeof(g_result));
        else
            copy(g_result, g_subject, sizeof(g_result));
    }
    g_kind = KIND_NONE;
}

/* The panel's parts, in one place so the drawing and the hit testing cannot
 * drift apart. A save dialogue carries a toolbar row; an open-with does not. */
static int toolbar_y(void)  { return g_py + 26; }
static int list_top(void)   { return g_kind == KIND_SAVE ? g_py + 52 : g_py + 46; }
static int list_height(void)
{
    return g_kind == KIND_SAVE ? PANEL_H - 52 - 66 : ROWS * ROW_H;
}
static int icon_cols(void)  { return (PANEL_W - 24) / CELL_W; }

/* How far down the content runs, so scrolling can stop at its end. */
static int rows_shown(void) { return list_height() / ROW_H; }
static int content_rows(void)
{
    if (g_kind == KIND_SAVE && g_icons) {
        const int c = icon_cols();
        return (g_items_n + c - 1) / c;
    }
    return g_items_n;
}
static int max_scroll(void)
{
    const int per = (g_kind == KIND_SAVE && g_icons)
                        ? list_height() / CELL_H : rows_shown();
    const int over = content_rows() - per;
    return over > 0 ? over : 0;
}
static void dlg_scroll_to(int row)
{
    const int hi = max_scroll();
    g_scroll = row < 0 ? 0 : (row > hi ? hi : row);
}

/* Going into a thing: a folder is somewhere to be, a file is a name to take. */
static void enter_item(int i)
{
    if (i < 0 || i >= g_items_n)
        return;
    if (!g_item_dir[i]) {
        copy(g_name, g_items[i], sizeof(g_name));
        g_sel = i;
        return;
    }
    char down[256];
    join(g_dir, g_items[i], down, sizeof(down));
    dlg_goto(down, 1);
}

static void dlg_up(void)
{
    char up[256];
    parent_of(g_dir, up, sizeof(up));
    dlg_goto(up, 1);
}

static void dlg_history(int delta)
{
    const int to = g_hist_at + delta;
    if (to < 0 || to >= g_hist_n)
        return;
    g_hist_at = to;
    dlg_goto(g_hist[to], 0);
}

/* dir: 0 left, 1 right, 2 up. */
static void arrow_glyph(int x, int y, int w, int h, int dir, int enabled)
{
    const uint32_t ink = enabled ? WG_INK : WG_DIM;
    const int cx = x + w / 2, cy = y + h / 2;
    for (int i = 0; i < 4; ++i)
        for (int k = -i; k <= i; ++k) {
            if (dir == 0)      wg_plot(cx - 2 + i, cy + k, ink);
            else if (dir == 1) wg_plot(cx + 2 - i, cy + k, ink);
            else               wg_plot(cx + k, cy - 2 + i, ink);
        }
}

int dlg_event(const struct win_event* e)
{
    if (g_kind == KIND_NONE)
        return DLG_PENDING;

    const int list_x = g_px + 12, list_y = list_top();
    const int ok_x = g_px + PANEL_W - 150, ok_y = g_py + PANEL_H - 32;

    if (e->type == WIN_EVENT_KEY) {
        const char c = (char)e->key;
        if (c == 27) { g_kind = KIND_NONE; return DLG_CANCEL; }
        /* Arrows walk the list. In a save dialogue with icons showing, up and
         * down move a whole row, which is what the eye expects of a grid. */
        if (e->key == WIN_KEY_UP || e->key == WIN_KEY_DOWN ||
            (g_kind == KIND_SAVE && g_icons &&
             (e->key == WIN_KEY_LEFT || e->key == WIN_KEY_RIGHT))) {
            const int step = (g_kind == KIND_SAVE && g_icons &&
                              (e->key == WIN_KEY_UP || e->key == WIN_KEY_DOWN))
                                 ? icon_cols() : 1;
            int to = g_sel < 0 ? 0 : g_sel;
            if (e->key == WIN_KEY_DOWN || e->key == WIN_KEY_RIGHT) to += step;
            else                                                   to -= step;
            if (to >= 0 && to < g_items_n) {
                g_sel = to;
                /* Follow the selection rather than leaving it off screen. */
                const int per = (g_kind == KIND_SAVE && g_icons)
                                    ? list_height() / CELL_H : rows_shown();
                const int row = (g_kind == KIND_SAVE && g_icons)
                                    ? to / icon_cols() : to;
                if (row < g_scroll)             dlg_scroll_to(row);
                else if (row >= g_scroll + per) dlg_scroll_to(row - per + 1);
                if (g_kind == KIND_SAVE && !g_item_dir[to])
                    copy(g_name, g_items[to], sizeof(g_name));
            }
            return DLG_PENDING;
        }
        if (c == '\n' || c == '\r') {
            /* Enter on a folder goes into it; anywhere else it commits. That
             * is the one rule a save dialogue cannot get wrong, so it does not
             * wait for a second press the way the file manager does. */
            if (g_kind == KIND_SAVE && g_sel >= 0 && g_sel < g_items_n &&
                g_item_dir[g_sel]) {
                enter_item(g_sel);
                return DLG_PENDING;
            }
            accept();
            return DLG_ACCEPT;
        }
        if (g_kind == KIND_SAVE) {
            unsigned n = (unsigned)strlen(g_name);
            if ((c == '\b' || c == 0x7F) && n > 0) g_name[n - 1] = '\0';
            else if ((unsigned char)c >= 32 && n + 1 < sizeof(g_name))
                { g_name[n] = c; g_name[n + 1] = '\0'; }
        }
        return DLG_PENDING;
    }
    if (e->type != WIN_EVENT_MOUSE_DOWN)
        return DLG_PENDING;

    if (e->x >= ok_x && e->x < ok_x + 64 && e->y >= ok_y && e->y < ok_y + 22) {
        accept();
        return DLG_ACCEPT;
    }
    if (e->x >= ok_x + 72 && e->x < ok_x + 136 &&
        e->y >= ok_y && e->y < ok_y + 22) {
        g_kind = KIND_NONE;
        return DLG_CANCEL;
    }
    /* The checkbox, on the same line the name field occupies in a save. */
    if (g_kind == KIND_OPEN) {
        const int cy = list_y + ROWS * ROW_H + 10;
        if (e->x >= g_px + 12 && e->x < g_px + 26 &&
            e->y >= cy && e->y < cy + 14) {
            g_always = !g_always;
            return DLG_PENDING;
        }
    }
    /* The toolbar: three ways of moving and two ways of looking. */
    if (g_kind == KIND_SAVE && e->y >= toolbar_y() && e->y < toolbar_y() + 20) {
        const int bx = g_px + 12;
        if (e->x >= bx && e->x < bx + 24)              dlg_history(-1);
        else if (e->x >= bx + 26 && e->x < bx + 50)    dlg_history(1);
        else if (e->x >= bx + 52 && e->x < bx + 76)    dlg_up();
        else if (e->x >= g_px + PANEL_W - 104 && e->x < g_px + PANEL_W - 60)
            { g_icons = 0; g_scroll = 0; }
        else if (e->x >= g_px + PANEL_W - 56 && e->x < g_px + PANEL_W - 12)
            { g_icons = 1; g_scroll = 0; }
        return DLG_PENDING;
    }

    if (e->x >= list_x && e->x < list_x + PANEL_W - 24 &&
        e->y >= list_y && e->y < list_y + list_height()) {
        int hit;
        if (g_kind == KIND_SAVE && g_icons) {
            const int col = (e->x - list_x) / CELL_W;
            const int row = g_scroll + (e->y - list_y) / CELL_H;
            hit = (col >= icon_cols()) ? -1 : row * icon_cols() + col;
        } else {
            hit = g_scroll + (e->y - list_y) / ROW_H;
        }
        if (hit >= 0 && hit < g_items_n) {
            if (g_kind == KIND_SAVE) enter_item(hit);
            else                     g_sel = hit;
        }
    }
    return DLG_PENDING;
}

void dlg_draw(int window_w, int window_h)
{
    if (g_kind == KIND_NONE)
        return;

    g_px = (window_w - PANEL_W) / 2;
    g_py = (window_h - PANEL_H) / 2;
    if (g_px < 4) g_px = 4;
    if (g_py < 4) g_py = 4;

    /* A drop shadow, so it reads as sitting above the window rather than being
     * part of it. */
    wg_fill(g_px + 4, g_py + 4, PANEL_W, PANEL_H, WG_SHADOW);
    wg_fill(g_px, g_py, PANEL_W, PANEL_H, WG_FACE);
    wg_outline(g_px, g_py, PANEL_W, PANEL_H, 1);
    wg_outline(g_px + 1, g_py + 1, PANEL_W - 2, PANEL_H - 2, 1);

    if (g_kind == KIND_SAVE) {
        wg_text(g_px + 12, g_py + 8, "Save as", WG_INK);
        /* Where you are, on the toolbar's right, in the space the buttons do
         * not want. A save dialogue that does not say where it will save is
         * just a text box with an opinion. */
        const int by = toolbar_y(), bx = g_px + 12;
        wg_button(bx, by, 24, 20, "", 0);
        arrow_glyph(bx, by, 24, 20, 0, g_hist_at > 0);
        wg_button(bx + 26, by, 24, 20, "", 0);
        arrow_glyph(bx + 26, by, 24, 20, 1, g_hist_at + 1 < g_hist_n);
        wg_button(bx + 52, by, 24, 20, "", 0);
        arrow_glyph(bx + 52, by, 24, 20, 2, strcmp(g_dir, "/") != 0);
        wg_text_clipped(bx + 82, by + 3, g_dir, WG_DIM, PANEL_W - 200);
        wg_button(g_px + PANEL_W - 104, by, 44, 20, "List", !g_icons);
        wg_button(g_px + PANEL_W - 56, by, 44, 20, "Icons", g_icons);
    } else {
        wg_text(g_px + 12, g_py + 10, "Open with", WG_INK);
        wg_text_clipped(g_px + 12, g_py + 28, g_subject, WG_DIM, PANEL_W - 24);
    }

    const int list_x = g_px + 12, list_y = list_top();
    const int list_w = PANEL_W - 24, list_h = list_height();
    wg_fill(list_x, list_y, list_w, list_h, WG_PAPER);
    wg_outline(list_x, list_y, list_w, list_h, 0);
    if (g_kind == KIND_SAVE && g_icons) {
        const int cols = icon_cols();
        for (int i = 0; i < g_items_n; ++i) {
            const int row = i / cols - g_scroll;
            if (row < 0 || (row + 1) * CELL_H > list_h)
                continue;
            const int cx = list_x + (i % cols) * CELL_W;
            const int cy = list_y + row * CELL_H;
            if (i == g_sel)
                wg_fill(cx + 2, cy + 1, CELL_W - 4, CELL_H - 2, wg_sel_colour());
            /* A folder is a squat box with a tab; a file is a taller one. */
            const uint32_t ink = g_item_dir[i] ? WG_ACCENT : WG_DIM;
            if (g_item_dir[i]) {
                wg_fill(cx + CELL_W / 2 - 14, cy + 8, 10, 3, ink);
                wg_fill(cx + CELL_W / 2 - 14, cy + 11, 28, 16, ink);
            } else {
                /* A page with ruled lines, the same shape the file manager
                 * draws - the two are looking at the same folder, so they had
                 * better agree about what a file looks like. */
                const int fx = cx + CELL_W / 2 - 9;
                wg_fill(fx, cy + 5, 18, 24, ink);
                wg_fill(fx + 1, cy + 6, 16, 22, WG_PAPER);
                for (int r = 0; r < 4; ++r)
                    wg_fill(fx + 4, cy + 10 + r * 4, 10, 1, ink);
            }
            wg_text_clipped(cx + 4, cy + CELL_H - 16, g_items[i], WG_INK,
                            CELL_W - 8);
        }
    } else {
        for (int r = 0; r * ROW_H + ROW_H <= list_h; ++r) {
            const int i = g_scroll + r;
            if (i >= g_items_n)
                break;
            const int y = list_y + r * ROW_H;
            if (i == g_sel)
                wg_fill(list_x + 1, y, list_w - 2, ROW_H, wg_sel_colour());
            wg_text_clipped(list_x + 4, y, g_items[i],
                            g_item_dir[i] ? WG_ACCENT : WG_INK, list_w - 10);
        }
    }

    if (g_kind == KIND_OPEN) {
        const int cy = list_y + list_h + 10;
        wg_fill(g_px + 12, cy, 14, 14, WG_PAPER);
        wg_outline(g_px + 12, cy, 14, 14, 0);
        if (g_always) {
            for (int i = 0; i < 5; ++i) wg_plot(g_px + 15 + i, cy + 6 + i, WG_ACCENT);
            for (int i = 0; i < 5; ++i) wg_plot(g_px + 19 + i, cy + 10 - i, WG_ACCENT);
        }
        wg_text(g_px + 34, cy - 1, "always open this kind this way", WG_INK);
    }

    if (g_kind == KIND_SAVE) {
        const int fy = list_y + list_h + 6;
        wg_text(g_px + 12, fy + 3, "name", WG_DIM);
        wg_fill(g_px + 62, fy, PANEL_W - 76, 20, WG_PAPER);
        wg_outline(g_px + 62, fy, PANEL_W - 76, 20, 0);
        wg_text_clipped(g_px + 66, fy + 2, g_name, WG_INK, PANEL_W - 84);
    }

    const int ok_x = g_px + PANEL_W - 150, ok_y = g_py + PANEL_H - 32;
    wg_button(ok_x, ok_y, 64, 22, g_kind == KIND_SAVE ? "Save" : "Open", 0);
    wg_button(ok_x + 72, ok_y, 64, 22, "Cancel", 0);
}

/* --- context menus --------------------------------------------------------
 *
 * Kept apart from the dialogue state on purpose: choosing from a menu is very
 * often what raises a dialogue, so the two have to be able to exist at once.
 */
#define MENU_MAX  10
#define MENU_ROW  17

static int  g_menu_on;
static int  g_mx, g_my, g_mw;
static const char* const* g_menu_items;
static int  g_menu_n;

void menu_open(int x, int y, const char* const* items, int count)
{
    g_menu_on = 1;
    g_mx = x;
    g_my = y;
    g_menu_items = items;
    g_menu_n = count > MENU_MAX ? MENU_MAX : count;
    g_mw = 0;
    for (int i = 0; i < g_menu_n; ++i) {
        const int w = (int)strlen(items[i]) * WG_GLYPH_W + 24;
        if (w > g_mw) g_mw = w;
    }
    if (g_mw < 100) g_mw = 100;
}

int menu_active(void) { return g_menu_on; }

int menu_event(const struct win_event* e)
{
    if (!g_menu_on)
        return -1;
    if (e->type == WIN_EVENT_KEY) {
        g_menu_on = 0;
        return -2;                      /* any key dismisses it */
    }
    if (e->type != WIN_EVENT_MOUSE_DOWN)
        return -1;

    const int h = g_menu_n * MENU_ROW + 4;
    if (e->x < g_mx || e->x >= g_mx + g_mw || e->y < g_my || e->y >= g_my + h) {
        /* A click outside is a dismissal, and is *not* passed on to the
         * application: the first click after a menu closes it and nothing
         * else, which is what every menu everywhere does. */
        g_menu_on = 0;
        return -2;
    }
    const int i = (e->y - g_my - 2) / MENU_ROW;
    g_menu_on = 0;
    if (i < 0 || i >= g_menu_n || g_menu_items[i][0] == '-')
        return -2;
    return i;
}

void menu_draw(void)
{
    if (!g_menu_on)
        return;
    const int h = g_menu_n * MENU_ROW + 4;
    wg_fill(g_mx + 3, g_my + 3, g_mw, h, WG_SHADOW);
    wg_fill(g_mx, g_my, g_mw, h, WG_FACE);
    wg_outline(g_mx, g_my, g_mw, h, 1);
    for (int i = 0; i < g_menu_n; ++i) {
        const int y = g_my + 2 + i * MENU_ROW;
        if (g_menu_items[i][0] == '-') {
            wg_fill(g_mx + 4, y + MENU_ROW / 2, g_mw - 8, 1, WG_SHADOW);
            continue;
        }
        wg_text_clipped(g_mx + 10, y, g_menu_items[i], WG_INK, g_mw - 16);
    }
}
