#include <dialog.h>
#include <string.h>
#include <sys/stat.h>
#include <widget.h>

#define KIND_NONE 0
#define KIND_SAVE 1
#define KIND_OPEN 2

#define PANEL_W 340
#define PANEL_H 240
#define ROW_H   16
#define ROWS    7

static int  g_kind;
static char g_dir[256] = "/";
static char g_name[128];
static char g_result[384];
static char g_subject[256];
static int  g_sel = -1;
static int  g_scroll;
static int  g_always;           /* the open-with checkbox */

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

/* Only directories: a save dialogue is for choosing *where*, and listing the
 * files as well would just be noise you cannot pick. */
static void list_dirs(void)
{
    static struct dirent scratch[64];
    g_items_n = 0;
    g_sel = -1;
    g_scroll = 0;
    copy(g_items[g_items_n], "..", 64);
    g_item_dir[g_items_n++] = 1;
    int n = getdents(g_dir, scratch, 64);
    if (n < 0) n = 0;
    for (int i = 0; i < n && g_items_n < 64; ++i) {
        if (scratch[i].d_type != S_IFDIR || scratch[i].d_name[0] == '.')
            continue;
        copy(g_items[g_items_n], scratch[i].d_name, 64);
        g_item_dir[g_items_n++] = 1;
    }
}

/* The openers this system has. Kept as a table rather than discovered, because
 * "which programs can open a file" is not something the filesystem records. */
static void list_openers(const char* path)
{
    static const char* kApps[] = { "Text editor", "Image viewer", "Paint" };
    g_items_n = 0;
    g_scroll = 0;
    for (unsigned i = 0; i < sizeof(kApps) / sizeof(kApps[0]); ++i) {
        copy(g_items[g_items_n], kApps[i], 64);
        g_item_dir[g_items_n++] = 0;
    }
    /* A program can also just be run. Offered last so it is a deliberate
     * choice rather than the default. */
    const int len = (int)strlen(path);
    if (len > 4 && (path[len-4] == '.') &&
        (path[len-3] == 'E' || path[len-3] == 'e')) {
        copy(g_items[g_items_n], "Run it", 64);
        g_item_dir[g_items_n++] = 0;
    }
    g_sel = 0;
}

void dlg_save(const char* where, const char* suggested)
{
    g_kind = KIND_SAVE;
    copy(g_dir, where && where[0] ? where : "/", sizeof(g_dir));
    copy(g_name, suggested ? suggested : "untitled", sizeof(g_name));
    list_dirs();
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
        const char* app = "/BIN/EDIT.ELF";
        if (g_sel == 1) app = "/BIN/IMGVIEW.ELF";
        else if (g_sel == 2) app = "/BIN/PAINT.ELF";
        else if (g_sel == 3) app = g_subject;      /* run it directly */
        copy(g_result, app, sizeof(g_result));
    }
    g_kind = KIND_NONE;
}

int dlg_event(const struct win_event* e)
{
    if (g_kind == KIND_NONE)
        return DLG_PENDING;

    const int list_x = g_px + 12, list_y = g_py + 46;
    const int ok_x = g_px + PANEL_W - 150, ok_y = g_py + PANEL_H - 32;

    if (e->type == WIN_EVENT_KEY) {
        const char c = (char)e->key;
        if (c == 27) { g_kind = KIND_NONE; return DLG_CANCEL; }
        if (c == '\n' || c == '\r') { accept(); return DLG_ACCEPT; }
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
    if (e->x >= list_x && e->x < list_x + PANEL_W - 24 &&
        e->y >= list_y && e->y < list_y + ROWS * ROW_H) {
        const int hit = g_scroll + (e->y - list_y) / ROW_H;
        if (hit >= 0 && hit < g_items_n) {
            if (g_kind == KIND_SAVE) {
                /* Clicking a folder goes into it; that is the navigation. */
                if (strcmp(g_items[hit], "..") == 0) {
                    char up[256];
                    parent_of(g_dir, up, sizeof(up));
                    copy(g_dir, up, sizeof(g_dir));
                } else {
                    char down[256];
                    join(g_dir, g_items[hit], down, sizeof(down));
                    copy(g_dir, down, sizeof(g_dir));
                }
                list_dirs();
            } else {
                g_sel = hit;
            }
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
    wg_bevel(g_px, g_py, PANEL_W, PANEL_H, 1);
    wg_bevel(g_px + 1, g_py + 1, PANEL_W - 2, PANEL_H - 2, 1);

    if (g_kind == KIND_SAVE) {
        wg_text(g_px + 12, g_py + 10, "Save as", WG_INK);
        wg_text_clipped(g_px + 12, g_py + 28, g_dir, WG_DIM, PANEL_W - 24);
    } else {
        wg_text(g_px + 12, g_py + 10, "Open with", WG_INK);
        wg_text_clipped(g_px + 12, g_py + 28, g_subject, WG_DIM, PANEL_W - 24);
    }

    const int list_x = g_px + 12, list_y = g_py + 46;
    const int list_w = PANEL_W - 24, list_h = ROWS * ROW_H;
    wg_fill(list_x, list_y, list_w, list_h, WG_PAPER);
    wg_bevel(list_x, list_y, list_w, list_h, 0);
    for (int r = 0; r < ROWS; ++r) {
        const int i = g_scroll + r;
        if (i >= g_items_n)
            break;
        const int y = list_y + r * ROW_H;
        if (i == g_sel)
            wg_fill(list_x + 1, y, list_w - 2, ROW_H, 0xB0C4DE);
        wg_text_clipped(list_x + 4, y, g_items[i],
                        g_item_dir[i] ? WG_ACCENT : WG_INK, list_w - 10);
    }

    if (g_kind == KIND_OPEN) {
        const int cy = list_y + list_h + 10;
        wg_fill(g_px + 12, cy, 14, 14, WG_PAPER);
        wg_bevel(g_px + 12, cy, 14, 14, 0);
        if (g_always) {
            for (int i = 0; i < 5; ++i) wg_plot(g_px + 15 + i, cy + 6 + i, WG_ACCENT);
            for (int i = 0; i < 5; ++i) wg_plot(g_px + 19 + i, cy + 10 - i, WG_ACCENT);
        }
        wg_text(g_px + 34, cy - 1, "always open this kind this way", WG_INK);
    }

    if (g_kind == KIND_SAVE) {
        const int fy = list_y + list_h + 8;
        wg_text(g_px + 12, fy + 3, "name", WG_DIM);
        wg_fill(g_px + 62, fy, PANEL_W - 76, 20, WG_PAPER);
        wg_bevel(g_px + 62, fy, PANEL_W - 76, 20, 0);
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
    wg_bevel(g_mx, g_my, g_mw, h, 1);
    for (int i = 0; i < g_menu_n; ++i) {
        const int y = g_my + 2 + i * MENU_ROW;
        if (g_menu_items[i][0] == '-') {
            wg_fill(g_mx + 4, y + MENU_ROW / 2, g_mw - 8, 1, WG_SHADOW);
            continue;
        }
        wg_text_clipped(g_mx + 10, y, g_menu_items[i], WG_INK, g_mw - 16);
    }
}
