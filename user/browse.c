/* browse - a file browser for the desktop.
 *
 * Three ways of looking at the same directory, because they answer different
 * questions: icons for "what is in here", a list for "how big is it", and a
 * tree for "where does this sit". They share one selection and one notion of
 * the current directory, so switching view never loses your place.
 *
 * Opening something is the same gesture in all three: click to select, click
 * the selected thing again - or press Return - to open it. A directory is
 * entered; a .ELF is run; anything else is handed to the text editor. That last
 * rule is the whole of the "document" story, and it is deliberately a rule
 * about the name rather than about the contents, because there is nothing in
 * the filesystem that records a type.
 */

#include <dialog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <widget.h>
#include <window.h>

#define VIEW_ICON 0
#define VIEW_LIST 1
#define VIEW_TREE 2

#define TOOLBAR_H 30
#define PATH_H    20
#define STATUS_H  20
#define ROW_H     18
#define CELL_W    92
#define CELL_H    64

#define MAX_ENTRIES 256
#define MAX_ROWS    256
#define MAX_EXPAND  32

static uint32_t* g_px;
static unsigned  g_w = 560, g_h = 420;

static char g_path[256] = "/";
static struct dirent g_entries[MAX_ENTRIES];
static int g_count;
static int g_selected = -1;
static int g_view = VIEW_ICON;
static int g_scroll;
static char g_status[128] = "";

/* Tree view: a flat list of visible rows, rebuilt whenever something is
 * expanded or collapsed. Keeping it flat means hit-testing and drawing are the
 * same loop the other views use. */
struct row {
    char path[256];
    char name[128];
    int  depth;
    int  is_dir;
    int  expanded;
    int  scanned;       /* its children have already been spliced in */
};
static struct row g_rows[MAX_ROWS];
static int g_row_count;
static char g_expanded[MAX_EXPAND][256];
static int g_expand_count;

/* Toolbar hit boxes, in window coordinates. */
struct box { int x, y, w, h; };
static struct box g_up      = { 8,   5, 44, 20 };
static struct box g_vicon   = { 60,  5, 52, 20 };
static struct box g_vlist   = { 116, 5, 52, 20 };
static struct box g_vtree   = { 172, 5, 52, 20 };
static struct box g_open    = { 232, 5, 56, 20 };
static struct box g_upline  = { 296, 5, 24, 20 };
static struct box g_dnline  = { 322, 5, 24, 20 };

static int inside(const struct box* b, int x, int y)
{
    return x >= b->x && y >= b->y && x < b->x + b->w && y < b->y + b->h;
}

/* --- paths --------------------------------------------------------------- */

static void join(const char* dir, const char* name, char* out, int max)
{
    int n = 0;
    for (const char* p = dir; *p != '\0' && n < max - 2; ++p)
        out[n++] = *p;
    if (n > 0 && out[n - 1] != '/')
        out[n++] = '/';
    for (const char* p = name; *p != '\0' && n < max - 1; ++p)
        out[n++] = *p;
    out[n] = '\0';
}

static void parent_of(const char* path, char* out, int max)
{
    int n = (int)strlen(path);
    while (n > 1 && path[n - 1] == '/')
        --n;
    while (n > 1 && path[n - 1] != '/')
        --n;
    while (n > 1 && path[n - 1] == '/')
        --n;
    if (n < 1) n = 1;
    if (n > max - 1) n = max - 1;
    for (int i = 0; i < n; ++i)
        out[i] = path[i];
    out[n] = '\0';
}

/* Case-insensitive suffix test, because the FAT names are upper case and the
 * ext ones are not. */
static int ends_with(const char* s, const char* suffix)
{
    const int ls = (int)strlen(s), lx = (int)strlen(suffix);
    if (lx > ls)
        return 0;
    for (int i = 0; i < lx; ++i) {
        char a = s[ls - lx + i], b = suffix[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b)
            return 0;
    }
    return 1;
}

/* --- reading the filesystem ---------------------------------------------- */

static void read_dir(void)
{
    g_count = getdents(g_path, g_entries, MAX_ENTRIES);
    if (g_count < 0)
        g_count = 0;
    g_selected = -1;
    g_scroll = 0;
    snprintf(g_status, sizeof(g_status), "%d item%s", g_count,
             g_count == 1 ? "" : "s");
}

static int is_expanded(const char* path)
{
    for (int i = 0; i < g_expand_count; ++i)
        if (strcmp(g_expanded[i], path) == 0)
            return 1;
    return 0;
}

static void toggle_expanded(const char* path)
{
    for (int i = 0; i < g_expand_count; ++i) {
        if (strcmp(g_expanded[i], path) != 0)
            continue;
        for (int j = i; j + 1 < g_expand_count; ++j)
            memcpy(g_expanded[j], g_expanded[j + 1], sizeof(g_expanded[0]));
        --g_expand_count;
        return;
    }
    if (g_expand_count < MAX_EXPAND) {
        int n = 0;
        while (path[n] != '\0' && n < 255) {
            g_expanded[g_expand_count][n] = path[n];
            ++n;
        }
        g_expanded[g_expand_count][n] = '\0';
        ++g_expand_count;
    }
}

/* Build the visible rows for the tree.
 *
 * Iteratively, and deliberately so. The obvious recursive walk needs a
 * directory buffer per level, and a struct dirent is 144 bytes - sixty-four of
 * them is nine kilobytes of stack per frame, which overruns a user stack a few
 * levels down and takes the process with it. Expanding in passes needs one
 * buffer no matter how deep the tree goes.
 *
 * Each pass finds the first expanded directory whose children are not yet
 * present and splices them in after it. That terminates because every pass
 * either adds rows or finds nothing left to add.
 */
static struct dirent g_scratch[64];

static void insert_children(int at, const char* dir, int depth)
{
    int n = getdents(dir, g_scratch, 64);
    if (n < 0)
        n = 0;

    /* Drop the ones that will not fit rather than overrunning the array. */
    int room = MAX_ROWS - g_row_count;
    if (n > room)
        n = room;
    if (n <= 0)
        return;

    for (int i = g_row_count - 1; i > at; --i)
        g_rows[i + n] = g_rows[i];
    g_row_count += n;

    int put = at + 1;
    for (int i = 0; i < n; ++i) {
        struct row* r = &g_rows[put++];
        join(dir, g_scratch[i].d_name, r->path, sizeof(r->path));
        int k = 0;
        while (g_scratch[i].d_name[k] != '\0' && k < 127) {
            r->name[k] = g_scratch[i].d_name[k];
            ++k;
        }
        r->name[k] = '\0';
        r->depth = depth;
        r->is_dir = (g_scratch[i].d_type == S_IFDIR);
        r->expanded = 0;        /* filled in by the pass that expands it */
        r->scanned = 0;
    }
}

static void rebuild_tree(void)
{
    g_row_count = 0;
    insert_children(-1, g_path, 0);

    for (;;) {
        int did = 0;
        for (int i = 0; i < g_row_count; ++i) {
            struct row* r = &g_rows[i];
            if (!r->is_dir || r->scanned || !is_expanded(r->path))
                continue;
            r->scanned = 1;
            r->expanded = 1;
            if (r->depth < 8)
                insert_children(i, r->path, r->depth + 1);
            did = 1;
            break;              /* the array moved; start the scan again */
        }
        if (!did)
            break;
    }

    if (g_selected >= g_row_count)
        g_selected = -1;
    snprintf(g_status, sizeof(g_status), "%d row%s shown", g_row_count,
             g_row_count == 1 ? "" : "s");
}

/* --- drawing ------------------------------------------------------------- */

/* A folder: a tab along the top of a body, which is all it takes to read as
 * one at this size. */
static void folder_icon(int x, int y)
{
    wg_fill(x, y + 3, 14, 4, 0xC8A030);
    wg_fill(x, y + 6, 32, 20, 0xE8C860);
    wg_bevel(x, y + 6, 32, 20, 1);
}

/* A document: a page with a folded corner and a couple of text lines. */
static void file_icon(int x, int y, int program)
{
    wg_fill(x + 4, y + 2, 24, 26, program ? 0xB0C0D8 : WG_PAPER);
    wg_bevel(x + 4, y + 2, 24, 26, 1);
    for (int i = 0; i < 6; ++i)
        wg_plot(x + 27 - i, y + 3 + i, WG_DIM);
    for (int line = 0; line < (program ? 1 : 4); ++line)
        wg_fill(x + 8, y + 12 + line * 4, program ? 16 : 14, 1, WG_DIM);
    if (program)
        wg_text(x + 9, y + 8, "*", WG_ACCENT);
}

static int content_top(void)  { return TOOLBAR_H + PATH_H; }
static int content_h(void)    { return (int)g_h - content_top() - STATUS_H; }

static void draw_toolbar(void)
{
    wg_fill(0, 0, (int)g_w, TOOLBAR_H, WG_FACE);
    wg_button(g_up.x, g_up.y, g_up.w, g_up.h, "Up", 0);
    wg_button(g_vicon.x, g_vicon.y, g_vicon.w, g_vicon.h, "Icon",
              g_view == VIEW_ICON);
    wg_button(g_vlist.x, g_vlist.y, g_vlist.w, g_vlist.h, "List",
              g_view == VIEW_LIST);
    wg_button(g_vtree.x, g_vtree.y, g_vtree.w, g_vtree.h, "Tree",
              g_view == VIEW_TREE);
    wg_button(g_open.x, g_open.y, g_open.w, g_open.h, "Open", 0);
    wg_button(g_upline.x, g_upline.y, g_upline.w, g_upline.h, "^", 0);
    wg_button(g_dnline.x, g_dnline.y, g_dnline.w, g_dnline.h, "v", 0);

    /* The path, in a sunken well so it reads as a display rather than a
     * control - it is not editable. */
    wg_fill(6, TOOLBAR_H + 1, (int)g_w - 12, PATH_H - 3, WG_PAPER);
    wg_bevel(6, TOOLBAR_H + 1, (int)g_w - 12, PATH_H - 3, 0);
    wg_text_clipped(10, TOOLBAR_H + 2, g_path, WG_INK, (int)g_w - 20);
}

static void draw_icons(void)
{
    const int top = content_top();
    const int cols = ((int)g_w - 8) / CELL_W;
    if (cols < 1)
        return;
    for (int i = g_scroll; i < g_count; ++i) {
        const int slot = i - g_scroll;
        const int cx = 8 + (slot % cols) * CELL_W;
        const int cy = top + 6 + (slot / cols) * CELL_H;
        if (cy + CELL_H > top + content_h())
            break;
        if (i == g_selected)
            wg_fill(cx - 2, cy - 2, CELL_W - 4, CELL_H - 6, 0xB0C4DE);
        const int dir = (g_entries[i].d_type == S_IFDIR);
        if (dir)
            folder_icon(cx + 26, cy);
        else
            file_icon(cx + 26, cy, ends_with(g_entries[i].d_name, ".ELF"));
        wg_text_clipped(cx, cy + 32, g_entries[i].d_name, WG_INK, CELL_W - 8);
    }
}

static void draw_list(void)
{
    const int top = content_top();
    wg_text(10, top + 2, "Name", WG_DIM);
    wg_text(300, top + 2, "Size", WG_DIM);
    wg_text(400, top + 2, "Kind", WG_DIM);
    wg_fill(8, top + ROW_H, (int)g_w - 16, 1, WG_DIM);

    for (int i = g_scroll; i < g_count; ++i) {
        const int slot = i - g_scroll;
        const int y = top + ROW_H + 2 + slot * ROW_H;
        if (y + ROW_H > top + content_h())
            break;
        if (i == g_selected)
            wg_fill(8, y, (int)g_w - 16, ROW_H, 0xB0C4DE);
        const int dir = (g_entries[i].d_type == S_IFDIR);
        wg_text_clipped(12, y + 1, g_entries[i].d_name, WG_INK, 280);
        char size[24];
        if (dir)
            snprintf(size, sizeof(size), "--");
        else
            snprintf(size, sizeof(size), "%llu",
                     (unsigned long long)g_entries[i].d_size);
        wg_text(300, y + 1, size, WG_INK);
        wg_text(400, y + 1,
                dir ? "folder"
                    : (ends_with(g_entries[i].d_name, ".ELF") ? "program"
                                                              : "document"),
                WG_INK);
    }
}

static void draw_tree(void)
{
    const int top = content_top();
    for (int i = g_scroll; i < g_row_count; ++i) {
        const int slot = i - g_scroll;
        const int y = top + 2 + slot * ROW_H;
        if (y + ROW_H > top + content_h())
            break;
        const struct row* r = &g_rows[i];
        const int x = 10 + r->depth * 16;
        if (i == g_selected)
            wg_fill(8, y, (int)g_w - 16, ROW_H, 0xB0C4DE);
        if (r->is_dir) {
            /* A twisty: a box with a minus when open, a plus when shut. */
            wg_fill(x, y + 4, 9, 9, WG_PAPER);
            wg_bevel(x, y + 4, 9, 9, 0);
            wg_fill(x + 2, y + 8, 5, 1, WG_INK);
            if (!r->expanded)
                wg_fill(x + 4, y + 6, 1, 5, WG_INK);
        }
        wg_text_clipped(x + 14, y + 1, r->name,
                        r->is_dir ? WG_ACCENT : WG_INK,
                        (int)g_w - x - 30);
    }
}

static void draw(void)
{
    wg_fill(0, 0, (int)g_w, (int)g_h, WG_FACE);
    draw_toolbar();

    /* The listing sits in its own sunken well, the way a document area does. */
    wg_fill(4, content_top(), (int)g_w - 8, content_h(), WG_PAPER);
    wg_bevel(4, content_top(), (int)g_w - 8, content_h(), 0);

    if (g_view == VIEW_ICON)      draw_icons();
    else if (g_view == VIEW_LIST) draw_list();
    else                          draw_tree();

    wg_fill(0, (int)g_h - STATUS_H, (int)g_w, STATUS_H, WG_FACE);
    wg_text_clipped(8, (int)g_h - STATUS_H + 2, g_status, WG_INK, (int)g_w - 16);
}

/* --- opening things ------------------------------------------------------ */

/* Run `app`, handing it `document` when there is one.
 *
 * Detached: the browser is not a shell and has no reason to wait. The desktop
 * is what the new window belongs to. */
static void launch(const char* app, const char* document)
{
    if (fork() != 0)
        return;
    char* argv[3];
    argv[0] = (char*)app;
    argv[1] = (char*)document;      /* null when the program takes none */
    argv[2] = 0;
    execve(app, argv, 0);
    exit(127);
}

/* What the open-with dialogue is deciding about. */
static char g_opening[256];

static void open_path(const char* path, int is_dir)
{
    if (is_dir) {
        int n = 0;
        while (path[n] != '\0' && n < 255) {
            g_path[n] = path[n];
            ++n;
        }
        g_path[n] = '\0';
        read_dir();
        if (g_view == VIEW_TREE)
            rebuild_tree();
        return;
    }
    /* Ask rather than assume. The name is the only hint this system has about
     * what a file is, and a hint is not good enough to decide for someone. */
    int n = 0;
    while (path[n] != '\0' && n < 255) { g_opening[n] = path[n]; ++n; }
    g_opening[n] = '\0';
    dlg_open_with(g_opening);
    snprintf(g_status, sizeof(g_status), "choose an application");
}

static void open_selected(void)
{
    if (g_view == VIEW_TREE) {
        if (g_selected < 0 || g_selected >= g_row_count)
            return;
        const struct row* r = &g_rows[g_selected];
        if (r->is_dir) {
            toggle_expanded(r->path);
            rebuild_tree();
        } else {
            open_path(r->path, 0);
        }
        return;
    }
    if (g_selected < 0 || g_selected >= g_count)
        return;
    char full[256];
    join(g_path, g_entries[g_selected].d_name, full, sizeof(full));
    open_path(full, g_entries[g_selected].d_type == S_IFDIR);
}

static void go_up(void)
{
    char up[256];
    parent_of(g_path, up, sizeof(up));
    open_path(up, 1);
}

/* Which item is under the pointer, or -1. */
static int hit_test(int x, int y)
{
    const int top = content_top();
    if (y < top || y >= top + content_h())
        return -1;
    if (g_view == VIEW_ICON) {
        const int cols = ((int)g_w - 8) / CELL_W;
        if (cols < 1 || x < 8)
            return -1;
        const int col = (x - 8) / CELL_W;
        const int rowi = (y - top - 6) / CELL_H;
        if (col >= cols || rowi < 0)
            return -1;
        const int i = g_scroll + rowi * cols + col;
        return i < g_count ? i : -1;
    }
    if (g_view == VIEW_LIST) {
        const int i = g_scroll + (y - top - ROW_H - 2) / ROW_H;
        return (i >= g_scroll && i < g_count) ? i : -1;
    }
    const int i = g_scroll + (y - top - 2) / ROW_H;
    return (i >= g_scroll && i < g_row_count) ? i : -1;
}

static void scroll_by(int delta)
{
    const int total = (g_view == VIEW_TREE) ? g_row_count : g_count;
    g_scroll += delta;
    if (g_scroll > total - 1) g_scroll = total - 1;
    if (g_scroll < 0) g_scroll = 0;
}

int main(int argc, char** argv)
{
    const int x = argc > 1 ? atoi_simple(argv[1]) : 60;
    const int y = argc > 2 ? atoi_simple(argv[2]) : 60;

    if (wg_font() != 0) {
        printf("browse: cannot read the console font\n");
        return 1;
    }
    const int id = win_create(x, y, g_w, g_h, "Files");
    if (id < 0) {
        printf("browse: no window server\n");
        return 1;
    }
    g_px = win_map(id);
    if (g_px == 0)
        return 1;
    win_set_min_size(id, 380, 260);
    wg_target(g_px, g_w, g_h);

    read_dir();
    draw();
    win_present(id);

    for (;;) {
        struct win_event event;
        while (win_poll(id, &event)) {
            if (event.type == WIN_EVENT_CLOSE) {
                win_destroy(id);
                return 0;
            }
            if (dlg_active() && event.type != WIN_EVENT_RESIZE) {
                if (dlg_event(&event) == DLG_ACCEPT) {
                    const char* app = dlg_path();
                    /* "Run it" hands back the file itself: then it is the
                     * program, not an argument to one. */
                    const int itself = (strcmp(app, g_opening) == 0);
                    launch(app, itself ? 0 : g_opening);
                    snprintf(g_status, sizeof(g_status), "opened with %s", app);
                }
                draw();
                dlg_draw((int)g_w, (int)g_h);
                win_present(id);
                continue;
            }
            if (event.type == WIN_EVENT_RESIZE) {
                g_w = (unsigned)event.x;
                g_h = (unsigned)event.y;
                g_px = win_map(id);
                if (g_px == 0)
                    return 1;
                wg_target(g_px, g_w, g_h);
            } else if (event.type == WIN_EVENT_MOUSE_DOWN) {
                if (inside(&g_up, event.x, event.y)) {
                    go_up();
                } else if (inside(&g_vicon, event.x, event.y)) {
                    g_view = VIEW_ICON; g_scroll = 0;
                } else if (inside(&g_vlist, event.x, event.y)) {
                    g_view = VIEW_LIST; g_scroll = 0;
                } else if (inside(&g_vtree, event.x, event.y)) {
                    g_view = VIEW_TREE; g_scroll = 0; g_selected = -1;
                    rebuild_tree();
                } else if (inside(&g_open, event.x, event.y)) {
                    open_selected();
                } else if (inside(&g_upline, event.x, event.y)) {
                    scroll_by(-1);
                } else if (inside(&g_dnline, event.x, event.y)) {
                    scroll_by(1);
                } else {
                    const int hit = hit_test(event.x, event.y);
                    if (hit >= 0) {
                        /* Click to select, click again to open - the same
                         * gesture in every view, and no timing to get wrong. */
                        if (hit == g_selected)
                            open_selected();
                        else
                            g_selected = hit;
                    }
                }
            } else if (event.type == WIN_EVENT_KEY) {
                if (event.key == '\n' || event.key == '\r')
                    open_selected();
                else if (event.key == 'u')
                    go_up();
                else if (event.key == 'i')
                    { g_view = VIEW_ICON; g_scroll = 0; }
                else if (event.key == 'l')
                    { g_view = VIEW_LIST; g_scroll = 0; }
                else if (event.key == 't')
                    { g_view = VIEW_TREE; g_scroll = 0; rebuild_tree(); }
            } else {
                continue;       /* nothing else changes what is on screen */
            }
            draw();
            dlg_draw((int)g_w, (int)g_h);
            win_present(id);
        }
        msleep(15);
    }
}
