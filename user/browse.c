/* browse - a file browser for the desktop.
 *
 * Three ways of looking at the same directory, because they answer different
 * questions: icons for "what is in here", a list for "how big is it", and a
 * tree for "where does this sit". They share one selection and one notion of
 * the current directory, so switching view never loses your place.
 *
 * Opening something is the same gesture in all three: click to select, click
 * the selected thing again - or press Return - to open it. A directory is
 * entered; a program is run; anything else is handed to the text editor. That last
 * rule is the whole of the "document" story, and it is deliberately a rule
 * about the name rather than about the contents, because there is nothing in
 * the filesystem that records a type.
 */

#include <paths.h>
#include <bundle.h>
#include <icon.h>
#include <clipboard.h>
#include <fcntl.h>
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
/* Multiple selection, as a flag per row. Kept beside g_selected rather than
 * replacing it because "the one you last clicked" and "everything marked" are
 * genuinely different questions - opening uses the first, copying the second. */
static char g_marked[MAX_ENTRIES];
static int  g_band;             /* a rubber band is being dragged */
static int  g_bar_drag;         /* the scrollbar's thumb is being dragged */
/* Where a range selection counts from. Shift extends from the last plain click
 * rather than from the last thing touched, so shift-clicking twice re-ranges
 * from the same place instead of creeping. */
static int  g_anchor = -1;
static int  g_blank_menu;   /* which menu is showing */
static int  g_app_menu_on;
static int  g_new_kind = -1; /* 0 file, 1 folder, once the name is chosen */
static int  g_renaming;      /* the save dialogue is being used to rename */
static int  g_enter_armed;   /* the first Enter of a double press */
static char g_rename_from[256];
static int  g_band_x, g_band_y, g_band_x2, g_band_y2;

/* A press on an item is not yet a drag: it becomes one only once the pointer
 * has moved far enough that it cannot be a click with a shaky hand. */
static int  g_press_item = -1;
static int  g_press_x, g_press_y;
static int  g_window_id = -1;

static int being_dragged(int i);
static int g_view = VIEW_ICON;
/* Measured in pixels down the content, not in items.
 *
 * It used to count items, which in the icon view meant a step of one moved
 * every icon one place around the grid: the first one vanished and the rest
 * reflowed, so it looked like the listing was filing out rather than scrolling
 * past. A pixel offset scrolls the grid as a grid, and lets it stop between
 * rows instead of jumping a whole one at a time. */
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
/* Where you have been, so back and forward mean something. A stack with a
 * cursor rather than two stacks: going somewhere new from the middle discards
 * what was ahead, which is what every browser does and what people expect. */
#define MAX_HISTORY 32
static char g_hist[MAX_HISTORY][256];
static int  g_hist_n;
static int  g_hist_at = -1;

/* Three groups and two singletons. Back, forward and up are one control with
 * three ends - they are the same question asked in three directions - so their
 * boxes are contiguous and they share a pill. The three views are another.
 * Open and Rename do different kinds of job and stand alone. */
static struct box g_back    = { 8,   5, 28, 22 };
static struct box g_fwd     = { 36,  5, 28, 22 };
static struct box g_up      = { 64,  5, 28, 22 };
static struct box g_vicon   = { 100, 5, 50, 22 };
static struct box g_vlist   = { 150, 5, 50, 22 };
static struct box g_vtree   = { 200, 5, 50, 22 };
static struct box g_open    = { 258, 5, 52, 22 };
static struct box g_rename  = { 318, 5, 64, 22 };
static struct box g_upline  = { 390, 5, 24, 22 };
static struct box g_dnline  = { 414, 5, 24, 22 };

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


/* --- reading the filesystem ---------------------------------------------- */

static void read_dir(void)
{
    g_count = getdents(g_path, g_entries, MAX_ENTRIES);
    if (g_count < 0)
        g_count = 0;
    g_selected = -1;
    memset(g_marked, 0, sizeof(g_marked));
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

/* The icon for an entry in the folder being shown. Which picture that is, is
 * icon.c's decision - the desktop shows files too, and it should not be able
 * to disagree with this about what a .PNG looks like. */
static void entry_icon(int x, int y, int i, int size)
{
    const int app = bundle_is_app(g_entries[i].d_name);
    const uint32_t* px = icon_for_entry(g_path, g_entries[i].d_name,
                                        g_entries[i].d_type == S_IFDIR, app,
                                        g_entries[i].d_mode);
    wg_icon_scaled(x, y, px, ICON_SIZE, ICON_SIZE, size, size);
}

static int content_top(void)  { return TOOLBAR_H + PATH_H; }
static int content_h(void)    { return (int)g_h - content_top() - STATUS_H; }
static int bar_x(void)        { return (int)g_w - 4 - WG_SCROLL_W; }

static int icon_cols(void)
{
    const int c = ((int)g_w - 8 - WG_SCROLL_W) / CELL_W;
    return c > 0 ? c : 1;
}

/* The whole content's height in pixels - what the bar's span is measured in
 * now that scrolling is smooth. */
static int content_span(void)
{
    int h;
    if (g_view == VIEW_ICON)
        h = ((g_count + icon_cols() - 1) / icon_cols()) * CELL_H + 12;
    else if (g_view == VIEW_LIST)
        h = ROW_H + 2 + g_count * ROW_H;
    else
        h = 2 + g_row_count * ROW_H;
    return h > 0 ? h : 1;
}

/* A step small enough to feel continuous and large enough to get somewhere. */
static int scroll_step(void)
{
    return (g_view == VIEW_ICON) ? CELL_H / 4 : ROW_H;
}

static void reveal_selected(void);

static void scroll_to(int px)
{
    const int most = content_span() - content_h();
    if (px > most) px = most;
    if (px < 0) px = 0;
    g_scroll = px;
}

/* Bring the selection into view after the keyboard moves it. */
static void reveal_selected(void)
{
    if (g_selected < 0)
        return;
    int top, bottom;
    if (g_view == VIEW_ICON) {
        const int row = g_selected / icon_cols();
        top = row * CELL_H;
        bottom = top + CELL_H;
    } else if (g_view == VIEW_LIST) {
        top = ROW_H + 2 + g_selected * ROW_H;
        bottom = top + ROW_H;
    } else {
        top = 2 + g_selected * ROW_H;
        bottom = top + ROW_H;
    }
    if (top < g_scroll)
        scroll_to(top);
    else if (bottom > g_scroll + content_h())
        scroll_to(bottom - content_h());
}


/* A triangle, filled by rows. dir: 0 left, 1 right, 2 up. */
static void arrow_glyph(const struct box* b, int dir, int enabled)
{
    const uint32_t ink = enabled ? WG_INK : WG_DIM;
    const int cx = b->x + b->w / 2, cy = b->y + b->h / 2;
    for (int i = 0; i < 5; ++i) {
        const int run = i * 2 + 1;
        for (int k = -i; k <= i; ++k) {
            (void)run;
            /* i is the distance from the apex, so the apex leads and the
             * base trails: that is which way the arrow points. */
            if (dir == 0)      wg_plot(cx - 2 + i, cy + k, ink);
            else if (dir == 1) wg_plot(cx + 2 - i, cy + k, ink);
            else               wg_plot(cx + k, cy - 2 + i, ink);
        }
    }
}

static void draw_toolbar(void)
{

    /* Back and forward are icons because they are the two controls used most
     * and the two whose meaning a word does not improve. Up keeps a glyph of
     * its own rather than a label for the same reason. */
    {
        static const char* const nav[3]  = { "", "", "" };
        static const char* const view[3] = { "Icon", "List", "Tree" };
        static const char* const line[2] = { "^", "v" };
        wg_pill_group(g_back.x, g_back.y, g_back.w, g_back.h, 3, nav, -1);
        /* The arrows go on top of the pill: it is one control, and these say
         * which of its three ends is which. */
        arrow_glyph(&g_back, 0, g_hist_at > 0);
        arrow_glyph(&g_fwd,  1, g_hist_at >= 0 && g_hist_at + 1 < g_hist_n);
        arrow_glyph(&g_up,   2, strcmp(g_path, "/") != 0);
        wg_pill_group(g_vicon.x, g_vicon.y, g_vicon.w, g_vicon.h, 3, view,
                      g_view);
        wg_pill(g_open.x, g_open.y, g_open.w, g_open.h, "Open", 0);
        wg_pill(g_rename.x, g_rename.y, g_rename.w, g_rename.h, "Rename", 0);
        wg_pill_group(g_upline.x, g_upline.y, g_upline.w, g_upline.h, 2,
                      line, -1);
    }

    /* The path, in a sunken well so it reads as a display rather than a
     * control - it is not editable. */
    wg_container(6, TOOLBAR_H + 1, (int)g_w - 12, PATH_H - 3, 6);
    wg_bevel(6, TOOLBAR_H + 1, (int)g_w - 12, PATH_H - 3, 0);
    wg_text_clipped(10, TOOLBAR_H + 2, g_path, WG_INK, (int)g_w - 20);
}

static void draw_icons(void)
{
    const int top = content_top();
    const int cols = icon_cols();

    /* Whole rows are laid out as always; the offset just slides them, so a
     * partly-visible row at either edge is drawn and clipped rather than
     * skipped. */
    const int first = g_scroll / CELL_H;
    const int shift = g_scroll % CELL_H;
    for (int i = first * cols; i < g_count; ++i) {
        if (being_dragged(i))
            continue;               /* it is in the air, not in the folder */
        const int slot = i - first * cols;
        const int cx = 8 + (slot % cols) * CELL_W;
        const int cy = top + 6 + (slot / cols) * CELL_H - shift;
        if (cy >= top + content_h())
            break;
        if (cy + CELL_H < top)
            continue;
        if (i == g_selected || g_marked[i])
            wg_fill(cx - 2, cy - 2, CELL_W - 4, CELL_H - 6, wg_sel_colour());
        /* A bundle is drawn as the application it is, not as the directory it
         * happens to be made of. */
        entry_icon(cx + 26, cy, i, ICON_SIZE);
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

    const int first = (g_scroll > ROW_H + 2) ? (g_scroll - ROW_H - 2) / ROW_H : 0;
    for (int i = first; i < g_count; ++i) {
        if (being_dragged(i))
            continue;
        const int y = top + ROW_H + 2 + i * ROW_H - g_scroll;
        if (y >= top + content_h())
            break;
        if (y + ROW_H < top + ROW_H)
            continue;               /* under the column headings */
        if (i == g_selected || g_marked[i])
            wg_fill(8, y, (int)g_w - 16, ROW_H, wg_sel_colour());
        const int dir = (g_entries[i].d_type == S_IFDIR) &&
                        !bundle_is_app(g_entries[i].d_name);
        entry_icon(11, y + 1, i, 16);
        wg_text_clipped(32, y + 1, g_entries[i].d_name, WG_INK, 260);
        char size[24];
        if (dir)
            snprintf(size, sizeof(size), "--");
        else
            snprintf(size, sizeof(size), "%llu",
                     (unsigned long long)g_entries[i].d_size);
        wg_text(300, y + 1, size, WG_INK);
        wg_text(400, y + 1,
                bundle_is_app(g_entries[i].d_name) ? "application"
                    : dir ? "folder"
                    : (S_ISEXEC(g_entries[i].d_mode) ? "program" : "document"),
                WG_INK);
    }
}

static void draw_tree(void)
{
    const int top = content_top();
    const int first = g_scroll / ROW_H;
    for (int i = first; i < g_row_count; ++i) {
        const int y = top + 2 + i * ROW_H - g_scroll;
        if (y >= top + content_h())
            break;
        if (y + ROW_H < top)
            continue;
        const struct row* r = &g_rows[i];
        const int x = 10 + r->depth * 16;
        if (i == g_selected)
            wg_fill(8, y, (int)g_w - 16, ROW_H, wg_sel_colour());
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
    wg_theme();                 /* whatever settings last chose */
    wg_glass_clear();

    /* The listing sits in its own sunken well, the way a document area does. */
    /* A container rather than a white sheet: it sits four pixels in from the
     * window's edge, so it gets the small corner that goes with that. */
    wg_container(4, content_top(), (int)g_w - 8, content_h(), 4);

    if (g_view == VIEW_ICON)      draw_icons();
    else if (g_view == VIEW_LIST) draw_list();
    else                          draw_tree();

    /* The chrome goes on last. Now that a row can be half-scrolled off the top,
     * it draws above the content area - the drawing primitives clip to the
     * window, not to the well - so the toolbar has to be painted over it rather
     * than under it. The well's edge is redrawn for the same reason. */
    draw_toolbar();
    wg_bevel(4, content_top(), (int)g_w - 8, content_h(), 0);

    if (g_band) {
        const int x0 = g_band_x < g_band_x2 ? g_band_x : g_band_x2;
        const int x1 = g_band_x < g_band_x2 ? g_band_x2 : g_band_x;
        const int y0 = g_band_y < g_band_y2 ? g_band_y : g_band_y2;
        const int y1 = g_band_y < g_band_y2 ? g_band_y2 : g_band_y;
        for (int x = x0; x <= x1; ++x) { wg_plot(x, y0, WG_INK); wg_plot(x, y1, WG_INK); }
        for (int y = y0; y <= y1; ++y) { wg_plot(x0, y, WG_INK); wg_plot(x1, y, WG_INK); }
    }

    /* A vertical bar, since a directory rarely fits. */
    wg_scrollbar_v(bar_x(), content_top(), content_h(),
                   g_scroll, content_h(), content_span());

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

/* Remembered "always open this kind with" choices, keyed by extension.
 *
 * Held in memory for this session only: there is no per-user settings store to
 * write them to, and inventing a file format for three associations would be
 * worse than being clear that they do not survive a logout. */
#define MAX_ASSOC 12
static char g_assoc_ext[MAX_ASSOC][12];
static char g_assoc_app[MAX_ASSOC][64];
static int  g_assoc_n;

static void extension_of(const char* path, char* out, int max)
{
    int dot = -1;
    for (int i = 0; path[i] != '\0'; ++i)
        if (path[i] == '.') dot = i;
    int n = 0;
    if (dot >= 0)
        for (int i = dot; path[i] != '\0' && n < max - 1; ++i) {
            char c = path[i];
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            out[n++] = c;
        }
    out[n] = '\0';
}

static const char* remembered(const char* path)
{
    char ext[12];
    extension_of(path, ext, sizeof(ext));
    if (ext[0] == '\0')
        return 0;
    for (int i = 0; i < g_assoc_n; ++i)
        if (strcmp(g_assoc_ext[i], ext) == 0)
            return g_assoc_app[i];
    return 0;
}

static void remember(const char* path, const char* app)
{
    char ext[12];
    extension_of(path, ext, sizeof(ext));
    if (ext[0] == '\0')
        return;
    for (int i = 0; i < g_assoc_n; ++i)
        if (strcmp(g_assoc_ext[i], ext) == 0) {
            int k = 0;
            while (app[k] && k < 63) { g_assoc_app[i][k] = app[k]; ++k; }
            g_assoc_app[i][k] = '\0';
            return;
        }
    if (g_assoc_n >= MAX_ASSOC)
        return;
    int k = 0;
    while (ext[k] && k < 11) { g_assoc_ext[g_assoc_n][k] = ext[k]; ++k; }
    g_assoc_ext[g_assoc_n][k] = '\0';
    k = 0;
    while (app[k] && k < 63) { g_assoc_app[g_assoc_n][k] = app[k]; ++k; }
    g_assoc_app[g_assoc_n][k] = '\0';
    ++g_assoc_n;
}

static void remember_place(const char* path);

static void goto_path(const char* path, int record)
{
    int n = 0;
    while (path[n] != '\0' && n < 255) { g_path[n] = path[n]; ++n; }
    g_path[n] = '\0';
    if (record)
        remember_place(g_path);
    read_dir();
    if (g_view == VIEW_TREE)
        rebuild_tree();
}

static void open_path(const char* path, int is_dir)
{
    /* A bundle is entered by running it, not by descending into it - which is
     * the whole difference between an application and the directory it is made
     * of. Its contents are still reachable: nothing hides them, they are simply
     * not what opening it means. */
    if (bundle_is_app(path)) {
        struct bundle b;
        if (bundle_load(path, &b) == 0) {
            char exec[256];
            bundle_exec(&b, exec, sizeof(exec));
            launch(exec, 0);
            snprintf(g_status, sizeof(g_status), "launched %s", b.name);
        } else {
            snprintf(g_status, sizeof(g_status),
                     "%s has no usable Info", path);
        }
        return;
    }

    if (is_dir) {
        goto_path(path, 1);
        return;
    }
    int n = 0;
    while (path[n] != '\0' && n < 255) { g_opening[n] = path[n]; ++n; }
    g_opening[n] = '\0';

    /* An application that says it opens this kind is asked first: the bundle
     * declaring `opens .txt` is the system's only actual knowledge of what a
     * document is for, and it beats guessing. A remembered choice still wins
     * over it, because that was the user saying so explicitly. */
    const char* known = remembered(g_opening);
    if (known == 0) {
        struct bundle b;
        if (bundle_for_document(PATH_APPS, g_opening, &b) == 0) {
            char exec[256];
            bundle_exec(&b, exec, sizeof(exec));
            launch(exec, g_opening);
            snprintf(g_status, sizeof(g_status), "opened with %s", b.name);
            return;
        }
    }
    if (known != 0) {
        launch(known, g_opening);
        snprintf(g_status, sizeof(g_status), "opened with %s", known);
        return;
    }

    /* Otherwise ask. The name is the only hint this system has about what a
     * file is, and a hint is not good enough to decide for someone. */
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

/* Copy what is marked - one path per line, which is what a shell or an editor
 * on the other end can actually use. */
static void copy_marked(void)
{
    static char buf[CLIP_MAX];
    int n = 0, count = 0;
    for (int i = 0; i < g_count && n < (int)sizeof(buf) - 300; ++i) {
        if (!g_marked[i] && i != g_selected)
            continue;
        char full[256];
        join(g_path, g_entries[i].d_name, full, sizeof(full));
        n += snprintf(&buf[n], sizeof(buf) - (unsigned)n, "%s\n", full);
        ++count;
    }
    if (count == 0) {
        snprintf(g_status, sizeof(g_status), "nothing selected");
        return;
    }
    clip_put(buf, (unsigned)n);
    snprintf(g_status, sizeof(g_status), "copied %d path%s", count,
             count == 1 ? "" : "s");
}

/* Click, ctrl-click and shift-click, which between them are the whole of how
 * anyone expects to pick things out of a list. */
static void select_at(int hit, uint32_t mods)
{
    const int limit = (g_view == VIEW_TREE) ? g_row_count : g_count;
    if (hit < 0 || hit >= limit)
        return;

    if (mods & WIN_MOD_CTRL) {
        /* Add or remove one, and leave the rest alone. */
        g_marked[hit] = (char)!g_marked[hit];
        g_selected = hit;
        g_anchor = hit;
    } else if ((mods & WIN_MOD_SHIFT) && g_anchor >= 0) {
        /* Everything between the anchor and here, replacing what was marked -
         * the anchor deliberately stays put so a second shift-click re-ranges
         * from the same origin rather than walking. */
        memset(g_marked, 0, sizeof(g_marked));
        const int a = g_anchor < hit ? g_anchor : hit;
        const int b = g_anchor < hit ? hit : g_anchor;
        for (int i = a; i <= b && i < MAX_ENTRIES; ++i)
            g_marked[i] = 1;
        g_selected = hit;
    } else {
        /* A plain click starts again, and sets the anchor a later shift-click
         * will measure from. */
        memset(g_marked, 0, sizeof(g_marked));
        g_marked[hit] = 1;
        g_selected = hit;
        g_anchor = hit;
    }
    int n = 0;
    for (int i = 0; i < limit && i < MAX_ENTRIES; ++i)
        n += g_marked[i] ? 1 : 0;
    snprintf(g_status, sizeof(g_status), "%d selected", n);
}

static void select_all(void)
{
    for (int i = 0; i < g_count; ++i)
        g_marked[i] = 1;
    snprintf(g_status, sizeof(g_status), "selected %d", g_count);
}

static void remember_place(const char* path)
{
    if (g_hist_at >= 0 && strcmp(g_hist[g_hist_at], path) == 0)
        return;                     /* already here */
    /* Going somewhere new from the middle drops what was ahead. */
    if (g_hist_at + 1 < g_hist_n)
        g_hist_n = g_hist_at + 1;
    if (g_hist_n >= MAX_HISTORY) {
        for (int i = 1; i < MAX_HISTORY; ++i)
            memcpy(g_hist[i - 1], g_hist[i], sizeof(g_hist[0]));
        --g_hist_n;
        --g_hist_at;
    }
    int k = 0;
    while (path[k] != '\0' && k < 255) { g_hist[g_hist_n][k] = path[k]; ++k; }
    g_hist[g_hist_n][k] = '\0';
    g_hist_at = g_hist_n++;
}

static void goto_path(const char* path, int record);

static void go_history(int delta)
{
    const int to = g_hist_at + delta;
    if (to < 0 || to >= g_hist_n)
        return;
    g_hist_at = to;
    goto_path(g_hist[to], 0);
}

static void go_up(void)
{
    char up[256];
    parent_of(g_path, up, sizeof(up));
    open_path(up, 1);
}

/* Which item is under the pointer, or -1. */
/* The top-left of an item's cell, in this window's coordinates. The inverse of
 * hit_test, and needed for the same reason a drag needs a home: the ghost has
 * to start where the thing was and land where it went. */
static void cell_at(int i, int* out_x, int* out_y)
{
    const int top = content_top();
    if (g_view == VIEW_ICON) {
        const int cols = icon_cols();
        *out_x = 8 + (i % cols) * CELL_W;
        *out_y = top + 6 + (i / cols) * CELL_H - g_scroll;
    } else if (g_view == VIEW_LIST) {
        *out_x = 8;
        *out_y = top + ROW_H + 2 + i * ROW_H - g_scroll;
    } else {
        *out_x = 8;
        *out_y = top + 2 + i * ROW_H - g_scroll;
    }
}

/* What the server should draw while this is in the air. */
static unsigned drag_icon_for(const char* name, unsigned char type)
{
    if (bundle_is_app(name))
        return WS_DRAG_APP;
    return type == S_IFDIR ? WS_DRAG_FOLDER : WS_DRAG_FILE;
}

/* True when this entry is the one currently being carried, so it can be left
 * out of the drawing: an icon that is in the air is not also in the folder. */
static int being_dragged(int i)
{
    if (!win_dragging() || i < 0 || i >= g_count)
        return 0;
    char full[256];
    join(g_path, g_entries[i].d_name, full, sizeof(full));
    return strcmp(full, win_drag_path()) == 0;
}

static int hit_test(int x, int y)
{
    const int top = content_top();
    if (y < top || y >= top + content_h())
        return -1;
    if (g_view == VIEW_ICON) {
        const int cols = icon_cols();
        if (x < 8)
            return -1;
        const int col = (x - 8) / CELL_W;
        const int rowi = (y - top - 6 + g_scroll) / CELL_H;
        if (col >= cols || rowi < 0)
            return -1;
        const int i = rowi * cols + col;
        return i < g_count ? i : -1;
    }
    if (g_view == VIEW_LIST) {
        const int i = (y - top - ROW_H - 2 + g_scroll) / ROW_H;
        return (i >= 0 && i < g_count) ? i : -1;
    }
    const int i = (y - top - 2 + g_scroll) / ROW_H;
    return (i >= 0 && i < g_row_count) ? i : -1;
}

static void scroll_by(int steps)
{
    scroll_to(g_scroll + steps * scroll_step());
}

/* Moving something here.
 *
 * The whole operation is one rename: within a filesystem that is what a move
 * is, and doing it as copy-then-delete would be slower, would need twice the
 * space, and would turn a failure half way through into two half files.
 */
static int move_into(const char* from, const char* dir, char* landed, int max)
{
    /* The last component of the source is the name it keeps. */
    int last = -1;
    for (int i = 0; from[i] != '\0'; ++i)
        if (from[i] == '/')
            last = i;
    const char* name = last >= 0 ? &from[last + 1] : from;

    char dest[256];
    join(dir, name, dest, sizeof(dest));
    if (strcmp(from, dest) == 0)
        return -1;                  /* already where it is being put */

    /* A folder cannot be moved inside itself: the path would still resolve and
     * the tree would be detached from the root with no way back to it. */
    const int flen = (int)strlen(from);
    int prefix = 1;
    for (int i = 0; i < flen; ++i)
        if (dir[i] != from[i]) { prefix = 0; break; }
    if (prefix && (dir[flen] == '/' || dir[flen] == '\0'))
        return -1;

    if (rename(from, dest) < 0)
        return -1;
    if (landed != 0) {
        int i = 0;
        while (dest[i] != '\0' && i < max - 1) { landed[i] = dest[i]; ++i; }
        landed[i] = '\0';
    }
    return 0;
}

/* Where a dropped thing came to rest, so the ghost can be sent there. Its own
 * new cell if it landed in this folder, or the folder it went into. */
static void settle_on(const char* dest_name, int over_folder, int folder_index)
{
    read_dir();
    int at = -1;
    if (over_folder) {
        at = folder_index;
    } else {
        for (int i = 0; i < g_count; ++i)
            if (strcmp(g_entries[i].d_name, dest_name) == 0) { at = i; break; }
    }
    if (at < 0) {
        win_drop_reject();
        return;
    }
    int cx, cy, ox, oy;
    cell_at(at, &cx, &cy);
    win_origin(g_window_id, &ox, &oy);
    win_drop_accept(ox + cx, oy + cy);
}

static const char* const kMenu[] = {
    "Open", "Open with...", "Rename", "Copy", "Select all", "-", "Refresh"
};

/* Renaming reuses the save dialogue: it is already "choose a directory and a
 * name", which is exactly what a rename is. The old path is remembered so the
 * move can be made when a name comes back. */
static void begin_rename(void)
{
    if (g_selected < 0 || g_selected >= g_count) {
        snprintf(g_status, sizeof(g_status), "select something to rename");
        return;
    }
    join(g_path, g_entries[g_selected].d_name, g_rename_from,
         sizeof(g_rename_from));
    g_renaming = 1;
    dlg_save(g_path, g_entries[g_selected].d_name);
    snprintf(g_status, sizeof(g_status), "rename %s to...",
             g_entries[g_selected].d_name);
}

/* A right-click menu for a bundle: the standard entries, then whatever the
 * application itself declares. This is what makes `menu` in an Info file real
 * rather than decorative - the application extends the shell, instead of the
 * shell knowing about applications. */
static char g_app_menu[4 + BUNDLE_MAX_MENU][32];
static const char* g_app_menu_p[4 + BUNDLE_MAX_MENU];
static int g_app_menu_n;
static struct bundle g_app_menu_bundle;

static int build_app_menu(const char* path)
{
    if (bundle_load(path, &g_app_menu_bundle) != 0)
        return 0;
    static const char* base[] = { "Open", "-" };
    g_app_menu_n = 0;
    for (unsigned i = 0; i < 2; ++i) {
        snprintf(g_app_menu[g_app_menu_n], 32, "%s", base[i]);
        g_app_menu_p[g_app_menu_n] = g_app_menu[g_app_menu_n];
        ++g_app_menu_n;
    }
    for (int i = 0; i < g_app_menu_bundle.menu_n; ++i) {
        snprintf(g_app_menu[g_app_menu_n], 32, "%s",
                 g_app_menu_bundle.menu[i]);
        g_app_menu_p[g_app_menu_n] = g_app_menu[g_app_menu_n];
        ++g_app_menu_n;
    }
    return g_app_menu_n;
}

/* The menu for the empty space around the items. Right-clicking nothing is
 * still a question - "what can I do here?" - and answering it with silence is
 * the difference between a window and a picture of one. */
static const char* const kBlankMenu[] = {
    "New file", "New folder", "-", "Paste", "Select all", "-", "Refresh"
};

int main(int argc, char** argv)
{
    const int x = argc > 1 ? atoi_simple(argv[1]) : 60;
    const int y = argc > 2 ? atoi_simple(argv[2]) : 60;

    if (wg_font() != 0) {
        printf("browse: cannot read the console font\n");
        return 1;
    }
    const int id = win_create(x, y, g_w, g_h, "Files");
    /* Its pixels carry alpha, so the glass reaches past the title bar. */
    if (id >= 0)
        win_set_alpha(id);
    if (id < 0) {
        printf("browse: no window server\n");
        return 1;
    }
    g_px = win_map(id);
    if (g_px == 0)
        return 1;
    g_window_id = id;
    win_set_min_size(id, 440, 260);
    wg_target(g_px, g_w, g_h);

    /* Where we start is somewhere we have been, so back can come home to it. */
    remember_place(g_path);
    read_dir();
    draw();
    win_present(id);

    int was_dragging = 0;
    for (;;) {
        /* Something dragged out of this folder into another window leaves no
         * trace here but the drag ending. */
        const int dragging_now = win_dragging();
        if (was_dragging && !dragging_now) {
            read_dir();
            draw();
            win_present(id);
        }
        was_dragging = dragging_now;

        struct win_event event;
        while (win_poll(id, &event)) {
            if (event.type == WIN_EVENT_CLOSE) {
                win_destroy(id);
                return 0;
            }
            if (menu_active() && event.type != WIN_EVENT_RESIZE) {
                const int pick = menu_event(&event);
                if (g_app_menu_on) {
                    if (pick == 0) {
                        open_selected();
                    } else if (pick > 1) {
                        /* An application's own entry. The system does not know
                         * what it means - only the application does - so it is
                         * launched with the entry as an argument and left to
                         * decide. */
                        char exec[256];
                        bundle_exec(&g_app_menu_bundle, exec, sizeof(exec));
                        launch(exec, g_app_menu[pick]);
                        snprintf(g_status, sizeof(g_status), "%s: %s",
                                 g_app_menu_bundle.name, g_app_menu[pick]);
                    }
                    if (pick != -1)
                        g_app_menu_on = 0;
                    draw();
                    dlg_draw((int)g_w, (int)g_h);
                    menu_draw();
                    win_present(id);
                    continue;
                }
                if (g_blank_menu) {
                    char full[300];
                    if (pick == 0 || pick == 1) {
                        /* Ask for the name rather than inventing one: an
                         * "untitled" that has to be renamed immediately is not
                         * a convenience. */
                        dlg_save(g_path, pick == 0 ? "untitled.txt" : "folder");
                        g_new_kind = pick;
                    } else if (pick == 3) {
                        static char buf[CLIP_MAX];
                        if (clip_get(buf, sizeof(buf)) > 0)
                            snprintf(g_status, sizeof(g_status),
                                     "clipboard holds: %.60s", buf);
                        else
                            snprintf(g_status, sizeof(g_status),
                                     "the clipboard is empty");
                    } else if (pick == 4) {
                        select_all();
                    } else if (pick == 6) {
                        read_dir();
                    }
                    (void)full;
                    if (pick != -1)
                        g_blank_menu = 0;
                    draw();
                    dlg_draw((int)g_w, (int)g_h);
                    menu_draw();
                    win_present(id);
                    continue;
                }
                if (pick == 0) {
                    open_selected();
                } else if (pick == 1 && g_selected >= 0) {
                    /* Ask even when a choice is remembered: this is how it gets
                     * changed. */
                    char full[256];
                    if (g_view == VIEW_TREE) {
                        int k = 0;
                        while (g_rows[g_selected].path[k] && k < 255) {
                            full[k] = g_rows[g_selected].path[k]; ++k;
                        }
                        full[k] = '\0';
                    } else {
                        join(g_path, g_entries[g_selected].d_name, full, sizeof(full));
                    }
                    int k = 0;
                    while (full[k] && k < 255) { g_opening[k] = full[k]; ++k; }
                    g_opening[k] = '\0';
                    dlg_open_with(g_opening);
                } else if (pick == 2) {
                    begin_rename();
                } else if (pick == 3) {
                    copy_marked();
                } else if (pick == 4) {
                    select_all();
                } else if (pick == 6) {
                    read_dir();
                }
                draw();
                dlg_draw((int)g_w, (int)g_h);
                menu_draw();
                win_present(id);
                continue;
            }
            if (dlg_active() && event.type != WIN_EVENT_RESIZE) {
                /* Once, and once only: the dialogue closes itself on accept,
                 * so asking it twice would leave the second caller looking at
                 * a dialogue that is no longer there. */
                const int answer = dlg_event(&event);
                if (answer == DLG_ACCEPT && g_renaming) {
                    g_renaming = 0;
                    if (rename(g_rename_from, dlg_path()) < 0)
                        snprintf(g_status, sizeof(g_status),
                                 "could not rename to %s", dlg_path());
                    else
                        snprintf(g_status, sizeof(g_status), "renamed to %s",
                                 dlg_path());
                    read_dir();
                } else if (answer == DLG_CANCEL && g_renaming) {
                    g_renaming = 0;
                } else if (answer == DLG_ACCEPT && g_new_kind >= 0) {
                    const char* where = dlg_path();
                    if (g_new_kind == 1) {
                        if (mkdir(where) < 0)
                            snprintf(g_status, sizeof(g_status),
                                     "could not create %s", where);
                        else
                            snprintf(g_status, sizeof(g_status), "created %s", where);
                    } else {
                        const int fd = open(where, O_WRONLY | O_CREAT | O_TRUNC);
                        if (fd < 0)
                            snprintf(g_status, sizeof(g_status),
                                     "could not create %s", where);
                        else {
                            close(fd);
                            snprintf(g_status, sizeof(g_status), "created %s", where);
                        }
                    }
                    g_new_kind = -1;
                    read_dir();
                }
                else if (answer == DLG_ACCEPT) {
                    const char* app = dlg_path();
                    /* "Run it" hands back the file itself: then it is the
                     * program, not an argument to one. */
                    const int itself = (strcmp(app, g_opening) == 0);
                    launch(app, itself ? 0 : g_opening);
                    /* Remembered by extension, so ticking the box on one .txt
                     * settles every .txt - see remember(). */
                    if (dlg_always() && !itself)
                        remember(g_opening, app);
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
                if (inside(&g_back, event.x, event.y)) {
                    go_history(-1);
                } else if (inside(&g_fwd, event.x, event.y)) {
                    go_history(1);
                } else if (inside(&g_rename, event.x, event.y)) {
                    begin_rename();
                } else if (inside(&g_up, event.x, event.y)) {
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
                } else if (event.x >= bar_x() && event.y >= content_top() &&
                           event.y < content_top() + content_h()) {
                    /* The bar was drawn but never listened to, which made it
                     * look broken rather than absent. */
                    if (wg_scroll_on_thumb_v(event.y, content_top(), content_h(),
                                             g_scroll, content_h(),
                                             content_span()))
                        g_bar_drag = 1;
                    else
                        scroll_to(wg_scroll_hit_v(event.x, event.y, bar_x(),
                            content_top(), content_h(), g_scroll,
                            content_h(), content_span()));
                } else if (event.button == 1 && event.y >= content_top() &&
                           hit_test(event.x, event.y) < 0) {
                    /* A press on empty space starts a rubber band rather than
                     * doing nothing. */
                    /* Clicking the empty space around the items lets go of
                     * them - which is the only way to end up with nothing
                     * selected once something is. */
                    g_band = 1;
                    g_band_x = g_band_x2 = event.x;
                    g_band_y = g_band_y2 = event.y;
                    g_selected = -1;
                    memset(g_marked, 0, sizeof(g_marked));
                    snprintf(g_status, sizeof(g_status), "%d item%s", g_count,
                             g_count == 1 ? "" : "s");
                } else if (event.button == 2) {
                    const int hit = hit_test(event.x, event.y);
                    if (hit >= 0) {
                        if (!g_marked[hit])
                            select_at(hit, 0);
                        char full[256];
                        join(g_path, g_entries[hit].d_name, full, sizeof(full));
                        if (bundle_is_app(full) && build_app_menu(full) > 0) {
                            menu_open(event.x, event.y, g_app_menu_p,
                                      g_app_menu_n);
                            g_app_menu_on = 1;
                        } else {
                            menu_open(event.x, event.y, kMenu, 7);
                        }
                    } else {
                        menu_open(event.x, event.y, kBlankMenu, 7);
                        g_blank_menu = 1;
                    }
                } else {
                    const int hit = hit_test(event.x, event.y);
                    if (hit >= 0 && g_view != VIEW_TREE) {
                        /* Remember where this began. It becomes a drag only
                         * once the pointer has travelled far enough that it
                         * cannot be a click with an unsteady hand. */
                        g_press_item = hit;
                        g_press_x = event.x;
                        g_press_y = event.y;
                    }
                    if (hit >= 0) {
                        /* Click to select, click again to open - the same
                         * gesture in every view, and no timing to get wrong.
                         * A modified click is always a selection, never an
                         * open: ctrl-clicking a thing twice must not launch it. */
                        const uint32_t m = event.modifiers;
                        if (hit == g_selected && !(m & (WIN_MOD_CTRL | WIN_MOD_SHIFT)))
                            open_selected();
                        else
                            select_at(hit, m);
                    }
                }
            } else if (event.type == WIN_EVENT_MOUSE_MOVE && g_press_item >= 0 &&
                       !win_dragging()) {
                const int dx = event.x - g_press_x, dy = event.y - g_press_y;
                if (dx * dx + dy * dy > 25) {       /* five pixels */
                    const int i = g_press_item;
                    char full[256];
                    join(g_path, g_entries[i].d_name, full, sizeof(full));
                    int cx, cy, ox, oy;
                    cell_at(i, &cx, &cy);
                    win_origin(g_window_id, &ox, &oy);
                    /* The grab is where the *press* was, not where the
                     * pointer had got to by the time the threshold was
                     * crossed. Using the latter makes the ghost jump by
                     * however far the first movement happened to be - which
                     * with a quick flick is most of the screen. */
                    win_drag_begin(full, g_entries[i].d_name,
                                   drag_icon_for(g_entries[i].d_name,
                                                 g_entries[i].d_type),
                                   g_press_x - cx, g_press_y - cy,
                                   ox + cx, oy + cy);
                    g_press_item = -1;
                }
            } else if (event.type == WIN_EVENT_MOUSE_MOVE && g_bar_drag) {
                scroll_to(wg_scroll_drag_v(event.y, content_top(), content_h(),
                                           content_h(), content_span()));
            } else if (event.type == WIN_EVENT_MOUSE_MOVE && g_band) {
                g_band_x2 = event.x;
                g_band_y2 = event.y;
                /* Mark by hit-testing the band's corners across the grid: the
                 * views already know how to turn a point into an index, so this
                 * needs no second layout calculation. */
                const int x0 = g_band_x < g_band_x2 ? g_band_x : g_band_x2;
                const int x1 = g_band_x < g_band_x2 ? g_band_x2 : g_band_x;
                const int y0 = g_band_y < g_band_y2 ? g_band_y : g_band_y2;
                const int y1 = g_band_y < g_band_y2 ? g_band_y2 : g_band_y;
                memset(g_marked, 0, sizeof(g_marked));
                for (int yy = y0; yy <= y1; yy += 4)
                    for (int xx = x0; xx <= x1; xx += 8) {
                        const int i = hit_test(xx, yy);
                        if (i >= 0 && i < MAX_ENTRIES)
                            g_marked[i] = 1;
                    }
            } else if (event.type == WIN_EVENT_DROP) {
                /* The payload is still in the drag record: the server leaves
                 * it there until the next drag begins, precisely so the
                 * receiver can read it when the drop arrives. */
                char from[256];
                snprintf(from, sizeof(from), "%s", win_drop_path());
                const int hit = hit_test(event.x, event.y);
                const int onto_folder =
                    hit >= 0 && g_entries[hit].d_type == S_IFDIR &&
                    !bundle_is_app(g_entries[hit].d_name);

                char dir[256];
                if (onto_folder)
                    join(g_path, g_entries[hit].d_name, dir, sizeof(dir));
                else
                    snprintf(dir, sizeof(dir), "%s", g_path);

                char landed[256] = "";
                if (move_into(from, dir, landed, sizeof(landed)) == 0) {
                    /* The name it kept, for finding where it ended up. */
                    int last = -1;
                    for (int i = 0; landed[i] != '\0'; ++i)
                        if (landed[i] == '/') last = i;
                    settle_on(last >= 0 ? &landed[last + 1] : landed,
                              onto_folder, hit);
                    snprintf(g_status, sizeof(g_status), "moved to %s", dir);
                } else {
                    win_drop_reject();
                    snprintf(g_status, sizeof(g_status), "could not move here");
                }
                g_press_item = -1;
            } else if (event.type == WIN_EVENT_MOUSE_UP) {
                g_band = 0;
                g_bar_drag = 0;
                g_press_item = -1;
            } else if (event.type == WIN_EVENT_KEY) {
                /* Arrows move the selection, not the view: moving the view and
                 * leaving the selection behind is how you lose your place. The
                 * view follows whatever is chosen. */
                if (event.key != '\n' && event.key != '\r')
                    g_enter_armed = 0;
                const int limit = (g_view == VIEW_TREE) ? g_row_count : g_count;
                const int per_row = (g_view == VIEW_ICON) ? icon_cols() : 1;
                if (event.key == WIN_KEY_DOWN || event.key == WIN_KEY_UP ||
                    (g_view == VIEW_ICON &&
                     (event.key == WIN_KEY_LEFT || event.key == WIN_KEY_RIGHT))) {
                    int to = g_selected < 0 ? 0 : g_selected;
                    if (event.key == WIN_KEY_DOWN)       to += per_row;
                    else if (event.key == WIN_KEY_UP)    to -= per_row;
                    else if (event.key == WIN_KEY_RIGHT) to += 1;
                    else                                 to -= 1;
                    if (to >= 0 && to < limit)
                        select_at(to, 0);
                    reveal_selected();
                } else if (event.key == WIN_KEY_RIGHT) {
                    scroll_to(g_scroll + content_h());      /* a page */
                } else if (event.key == WIN_KEY_LEFT) {
                    scroll_to(g_scroll - content_h());
                } else if (event.key == 1) {    /* ctrl+a */
                    select_all();
                } else if (event.key == 3) {    /* ctrl+c */
                    copy_marked();
                } else if (event.key == '\n' || event.key == '\r') {
                    /* Two presses to open, one to settle on a thing - the same
                     * rule the mouse follows, so the keyboard needs no rule of
                     * its own. Any other key disarms it. */
                    if (g_enter_armed) {
                        g_enter_armed = 0;
                        open_selected();
                    } else {
                        g_enter_armed = 1;
                        snprintf(g_status, sizeof(g_status),
                                 "press enter again to open");
                    }
                }
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
            menu_draw();
            win_present(id);
        }
        msleep(15);
    }
}
