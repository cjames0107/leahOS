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
#include <app.h>
#include <ui.h>
#include <fcntl.h>
#include <dialog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>
#include <widget.h>
#include <window.h>

#define VIEW_ICON 0
#define VIEW_LIST 1
#define VIEW_TREE 2

/* The sidebar is the window's spine: places above, whatever is mounted below.
 * Everything else measures from its right edge. */
#define SIDEBAR_W 140
/* One band, and now genuinely one: the window's title strip is this window's
 * to draw, so the navigation, the path, the search box and the menus share the
 * line the title used to have to itself. Before this there were two bars of
 * almost the same height touching each other, saying the same kind of thing.
 *
 * The height is the chrome's, because that is the strip being taken over. */
#define TOOLBAR_H WS_TITLE_HEIGHT
#define PATH_H    0
/* No status bar. It said "14 items", which is a number you can see by looking,
 * and it cost twenty pixels of every window forever to say it. The messages
 * that used to share it - "moved to /tmp", "cannot create a folder here" -
 * were worth more than the count and are now shown in the toolbar, where
 * there is already a band and they cost nothing. */
#define STATUS_H  0
#define ROW_H     18
#define CELL_W    92
#define CELL_H    64

#define MAX_ENTRIES 256
#define MAX_ROWS    256
#define MAX_EXPAND  32

static uint32_t* g_px;
static unsigned  g_w = 760, g_h = 460;

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
/* Clear of the window's own close/minimise/maximise pill, which the server
 * draws at the left of this same strip. The sidebar is wider than that pill,
 * so measuring from its edge already leaves the room. */
/* The view modes and the file actions are not on the toolbar any more: they
 * are in the menu behind the button at its right-hand end. A toolbar with
 * everything on it is a list of everything the program can do, in the place a
 * person has to look past to get to their files. */

/* Three menus rather than one list of nine.
 *
 * The single menu held everything the browser could do, which meant reading
 * past "View as Tree" to reach "Delete" - two unrelated kinds of thing in one
 * column, sorted by nothing. Split by the question being asked: what do I do
 * to this file, how do I look at this folder, and where do I want to be.
 *
 * A row whose label is "-" is a separator, drawn as a line and not selectable,
 * which is how a menu groups without needing a second level. */
#define MENU_NONE -1

enum { M_FILE, M_VIEW, M_GO, M_COUNT };

static const char* const kFileItems[] = {
    "Open", "Rename", "-", "New Folder", "Delete", "-", "Properties",
};
static const char* const kViewItems[] = {
    "as Icons", "as List", "as Tree", "-", "Refresh", "Rebuild Search Index",
};
static const char* const kGoItems[] = {
    "Back", "Forward", "Up", "-",
    "Home", "Applications", "Computer", "-", "Unmount Volume",
};

static const struct {
    const char*        title;
    const char* const* items;
    int                count;
} kMenus[M_COUNT] = {
    { "File", kFileItems, (int)(sizeof(kFileItems) / sizeof(kFileItems[0])) },
    { "View", kViewItems, (int)(sizeof(kViewItems) / sizeof(kViewItems[0])) },
    { "Go",   kGoItems,   (int)(sizeof(kGoItems)   / sizeof(kGoItems[0]))   },
};

/* How many items each menu has, as an array, because that is what the menu
 * bar component asks for. Derived from the table above rather than written
 * out again. */
static int g_menu_counts[M_COUNT];

#define INDEX_MAX   2048
#define INDEX_PATH  192
#define INDEX_DEPTH 8

static char g_index[INDEX_MAX][INDEX_PATH];
static int  g_index_n;
static int  g_index_ready;

static char g_query[64];
static int  g_results[INDEX_MAX];
static int  g_results_n;
static int  g_searching;        /* the content area is showing results */

static void index_build(int force);
static void search_run(void);


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
    g_status[0] = '\0';         /* the last message was about the last folder */
    g_count = getdents(g_path, g_entries, MAX_ENTRIES);
    if (g_count < 0)
        g_count = 0;
    g_selected = -1;
    memset(g_marked, 0, sizeof(g_marked));
    g_scroll = 0;
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

/* --- the sidebar ------------------------------------------------------------
 *
 * Two lists in one column. The places are where a person keeps things and are
 * fixed; the volumes are whatever is mounted right now and are read from
 * /proc/mounts, because the filesystem server owns that answer and a browser
 * carrying its own copy would be a second one.
 */
#define MAX_PLACES 24
#define PLACE_H    26
#define PLACES_TOP 12

/* A place in the sidebar. `volume` marks the ones read from /proc/mounts,
 * which are facts about the system rather than choices - they appear and
 * vanish with the disk and cannot be pinned or unpinned. Everything else is
 * the person's own list. */
static struct { char label[32]; char path[192]; int volume; } g_place[MAX_PLACES];
static int g_places;
static int g_volumes_from;      /* where the mounted volumes start */

static void add_place(const char* label, const char* path, int volume)
{
    if (g_places >= MAX_PLACES)
        return;
    snprintf(g_place[g_places].label, sizeof(g_place[0].label), "%s", label);
    snprintf(g_place[g_places].path, sizeof(g_place[0].path), "%s", path);
    g_place[g_places].volume = volume;
    ++g_places;
}

/* The last component of a path, which is what a pinned folder is called. */
static const char* leaf_of(const char* path)
{
    const char* leaf = path;
    for (const char* p = path; *p != '\0'; ++p)
        if (*p == '/' && p[1] != '\0')
            leaf = p + 1;
    return leaf;
}

static void sidebar_path(char* out, int max)
{
    const char* home = getenv("HOME");
    if (home == 0 || home[0] == '\0')
        home = "/root";
    snprintf(out, (unsigned)max, "%s/.sidebar", home);
}

/* What the sidebar holds when nobody has said otherwise.
 *
 * Five, and each earns its place: the applications, where you are, the two
 * folders a desktop system puts things in by default, and the root - which is
 * the way out of all of them. */
static void places_defaults(void)
{
    const char* home = getenv("HOME");
    if (home == 0 || home[0] == '\0')
        home = "/root";
    char sub[224];
    add_place("Applications", PATH_APPS, 0);
    add_place("Home", home, 0);
    snprintf(sub, sizeof(sub), "%s/Desktop", home);   add_place("Desktop", sub, 0);
    snprintf(sub, sizeof(sub), "%s/Documents", home); add_place("Documents", sub, 0);
    add_place("Computer", "/", 0);
}

static void places_save(void)
{
    char path[256];
    sidebar_path(path, sizeof(path));
    FILE* out = fopen(path, "w");
    if (out == 0)
        return;
    /* Only the pinned ones: the volumes are read from the system every time
     * and writing them down would mean remembering a disk that is not there. */
    for (int i = 0; i < g_places; ++i)
        if (!g_place[i].volume)
            fprintf(out, "%s\t%s\n", g_place[i].path, g_place[i].label);
    fclose(out);
}

/* Read the pinned list, or fall back to the defaults.
 *
 * An empty file means "I unpinned everything", which is a choice and is kept.
 * A missing file means nobody has ever chosen, which is when the defaults
 * apply - so the two cases have to be told apart, and that is why this looks
 * at whether the file opened rather than at how many lines it had. */
static void places_load(void)
{
    g_places = 0;
    char path[256];
    sidebar_path(path, sizeof(path));
    FILE* in = fopen(path, "r");
    if (in == 0) {
        places_defaults();
        return;
    }
    char line[256];
    while (g_places < MAX_PLACES && fgets(line, sizeof(line), in) != 0) {
        unsigned n = (unsigned)strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        if (n == 0)
            continue;
        char* tab = strchr(line, '\t');
        if (tab != 0) {
            *tab = '\0';
            add_place(tab + 1, line, 0);
        } else {
            add_place(leaf_of(line), line, 0);
        }
    }
    fclose(in);
}

static void places_build(void)
{
    places_load();
    g_volumes_from = g_places;

    /* And every filesystem that is mounted somewhere other than the root -
     * read from /proc/mounts, because the filesystem server owns that answer
     * and a browser carrying its own copy would be a second one. */
    FILE* in = fopen("/proc/mounts", "r");
    if (in == 0)
        return;
    char line[256];
    while (fgets(line, sizeof(line), in) != 0) {
        char what[96], at[96], kind[32], how[16];
        if (sscanf(line, "%95s %95s %31s %15s", what, at, kind, how) != 4)
            continue;
        if (strcmp(at, "/") == 0 || strcmp(kind, "procfs") == 0)
            continue;
        add_place(at, at, 1);
    }
    fclose(in);
}

/* Pin a folder, unless it is already there. */
static int place_pin(const char* path)
{
    for (int i = 0; i < g_places; ++i)
        if (strcmp(g_place[i].path, path) == 0)
            return 0;               /* already pinned; saying so is not an error */
    if (g_places >= MAX_PLACES)
        return -1;
    /* Inserted before the volumes, so the person's list stays one block and
     * the disks stay another. */
    for (int i = g_places; i > g_volumes_from; --i)
        g_place[i] = g_place[i - 1];
    snprintf(g_place[g_volumes_from].label, sizeof(g_place[0].label),
             "%s", leaf_of(path));
    snprintf(g_place[g_volumes_from].path, sizeof(g_place[0].path), "%s", path);
    g_place[g_volumes_from].volume = 0;
    ++g_volumes_from;
    ++g_places;
    places_save();
    return 0;
}

static void place_unpin(int i)
{
    if (i < 0 || i >= g_places || g_place[i].volume)
        return;                     /* a mounted disk is not a pin */
    for (int k = i; k + 1 < g_places; ++k)
        g_place[k] = g_place[k + 1];
    --g_places;
    if (i < g_volumes_from)
        --g_volumes_from;
    places_save();
}

static int sidebar_row(int y)
{
    const int row = (y - PLACES_TOP) / PLACE_H;
    return (row >= 0 && row < g_places) ? row : -1;
}

static int sidebar_row_y(int i) { return PLACES_TOP + i * PLACE_H; }

/* Which place is being dragged out of the sidebar, and how far. */
static int g_pin_drag = -1;
static int g_pin_out;

static void draw_sidebar(void)
{
    wg_sidebar(0, 0, SIDEBAR_W, (int)g_h);
    for (int i = 0; i < g_places; ++i) {
        const int y = sidebar_row_y(i);
        if (y + PLACE_H > (int)g_h)
            break;
        const int here = strcmp(g_place[i].path, g_path) == 0;
        /* The one being dragged out is shown on its way: dimmed, so releasing
         * it is visibly the thing that removes it rather than a surprise. */
        const int leaving = (i == g_pin_drag && g_pin_out);
        if (here && !leaving)
            wg_row_select(6, y - 2, SIDEBAR_W - 12, PLACE_H - 2);

        /* A folder icon, because that is what every one of these is. The
         * current one is shown open, which is the same distinction the icon
         * set already draws for a directory you are inside. */
        const uint32_t* px = icon_by_name(here ? "folder-opened"
                                               : "folder-populated");
        if (px != 0)
            wg_icon_scaled(10, y, px, ICON_SIZE, ICON_SIZE, 16, 16);
        wg_text_clipped(32, y + 1, g_place[i].label,
                        leaving ? WG_DIM
                                : (here ? wg_ink_colour() : WG_DIM),
                        SIDEBAR_W - 42);
    }
}

static int content_left(void) { return SIDEBAR_W; }

static int content_top(void)  { return TOOLBAR_H + PATH_H; }
static int content_h(void)    { return (int)g_h - content_top() - STATUS_H; }
static int bar_x(void)        { return (int)g_w - 4 - WG_SCROLL_W; }

static int icon_cols(void)
{
    const int c = ((int)g_w - content_left() - 8 - WG_SCROLL_W) / CELL_W;
    return c > 0 ? c : 1;
}

/* The whole content's height in pixels - what the bar's span is measured in
 * now that scrolling is smooth. */
static int content_span(void)
{
    int h;
    if (g_searching)
        h = 2 + g_results_n * ROW_H;
    else if (g_view == VIEW_ICON)
        h = ((g_count + icon_cols() - 1) / icon_cols()) * CELL_H + 12;
    else if (g_view == VIEW_LIST)
        h = ROW_H + 2 + g_count * ROW_H;
    else
        h = 2 + g_row_count * ROW_H;
    return h > 0 ? h : 1;
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

/* --- the toolbar, as components ---------------------------------------------
 *
 * The navigation and the path are drawn: three arrows sharing one pill and a
 * well with a line of text in it are pictures, not controls with state. The
 * search box and the menus are components, because they are exactly the two
 * things that had grown their own copy of what a component already does - a
 * field with a caret and a focus, and a bar of titles with a drop-down under
 * whichever one is down.
 *
 * All four are laid out by the tree, so menu_btn_x() and the hand-measured
 * search box are gone with them.
 */

static struct app g_app;
static struct ui_view* g_v_nav;
static struct ui_view* g_v_search;
static struct ui_view* g_v_menu;
static struct ui_view* g_v_content;

static void draw_nav(struct ui_view* v, void* user)
{
    (void)user;
    const struct ui_rect f = v->frame;
    const int w = f.w / 3;
    const struct box back = { f.x, f.y, w, f.h };
    const struct box fwd  = { f.x + w, f.y, w, f.h };
    const struct box up   = { f.x + 2 * w, f.y, w, f.h };

    static const char* const nav[3] = { "", "", "" };
    wg_pill_group(f.x, f.y, w, f.h, 3, nav, -1);
    /* The arrows go on top of the pill: it is one control, and these say which
     * of its three ends is which. */
    arrow_glyph(&back, 0, g_hist_at > 0);
    arrow_glyph(&fwd,  1, g_hist_at >= 0 && g_hist_at + 1 < g_hist_n);
    arrow_glyph(&up,   2, strcmp(g_path, "/") != 0);
}

static void draw_path(struct ui_view* v, void* user)
{
    (void)user;
    const struct ui_rect f = v->frame;
    if (f.w < 40)
        return;
    /* A display and not a control - it is not editable - so it sits in a well
     * rather than looking like a field. A message, when there is one, in place
     * of the path: it is about what just happened here, which is more use for
     * the moment after it happens than the path you can see in the title bar. */
    wg_container(f.x, f.y, f.w, f.h, 6);
    wg_text_clipped(f.x + 10, f.y + (f.h - WG_GLYPH_H) / 2,
                    g_status[0] != '\0' ? g_status : g_path,
                    g_status[0] != '\0' ? WG_DIM : WG_INK, f.w - 20);
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
        const int cx = content_left() + 8 + (slot % cols) * CELL_W;
        const int cy = top + 6 + (slot / cols) * CELL_H - shift;
        if (cy >= top + content_h())
            break;
        if (cy + CELL_H < top)
            continue;
        if (i == g_selected || g_marked[i])
            wg_row_select(cx - 2, cy - 2, CELL_W - 4, CELL_H - 6);
        /* A bundle is drawn as the application it is, not as the directory it
         * happens to be made of. */
        entry_icon(cx + 26, cy, i, ICON_SIZE);
        wg_text_clipped(cx, cy + 32, g_entries[i].d_name, WG_INK, CELL_W - 8);
    }
}

static void draw_list(void)
{
    const int top = content_top();
    /* Every x here is measured from the content's left edge rather than from
     * the window's. They were window coordinates, so a row began at x=8 -
     * eight pixels into a sidebar a hundred and forty wide - and the sidebar
     * was only drawn afterwards, on top. With the glass on, the sidebar is
     * translucent and the rows showed straight through it. */
    const int L = content_left();
    wg_text(L + 10, top + 2, "Name", WG_DIM);
    wg_text(L + 300, top + 2, "Size", WG_DIM);
    wg_text(L + 400, top + 2, "Kind", WG_DIM);
    wg_fill(L + 8, top + ROW_H, (int)g_w - L - 16, 1, WG_DIM);

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
            wg_row_select(L + 8, y, (int)g_w - L - 16, ROW_H);
        const int dir = (g_entries[i].d_type == S_IFDIR) &&
                        !bundle_is_app(g_entries[i].d_name);
        entry_icon(L + 11, y + 1, i, 16);
        wg_text_clipped(L + 32, y + 1, g_entries[i].d_name, WG_INK, 260);
        char size[24];
        if (dir)
            snprintf(size, sizeof(size), "--");
        else
            snprintf(size, sizeof(size), "%llu",
                     (unsigned long long)g_entries[i].d_size);
        wg_text(L + 300, y + 1, size, WG_INK);
        wg_text(L + 400, y + 1,
                bundle_is_app(g_entries[i].d_name) ? "application"
                    : dir ? "folder"
                    : (S_ISEXEC(g_entries[i].d_mode) ? "program" : "document"),
                WG_INK);
    }
}

static void draw_tree(void)
{
    const int top = content_top();
    const int L = content_left();       /* as in draw_list, and for the same reason */
    const int first = g_scroll / ROW_H;
    for (int i = first; i < g_row_count; ++i) {
        const int y = top + 2 + i * ROW_H - g_scroll;
        if (y >= top + content_h())
            break;
        if (y + ROW_H < top)
            continue;
        const struct row* r = &g_rows[i];
        const int x = L + 10 + r->depth * 16;
        if (i == g_selected)
            wg_row_select(L + 8, y, (int)g_w - L - 16, ROW_H);
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

/* Results are shown as paths rather than as names, because the answer to
 * "where is my file" is the path - a list of bare names would make you open
 * each one to find out which is which. */
static void draw_results(void)
{
    const int top = content_top();
    const int L = content_left();
    const int first = g_scroll / ROW_H;
    for (int i = first; i < g_results_n; ++i) {
        const int y = top + 2 + i * ROW_H - g_scroll;
        if (y >= top + content_h())
            break;
        if (y + ROW_H < top)
            continue;
        const char* path = g_index[g_results[i]];
        if (i == g_selected)
            wg_row_select(L + 8, y, (int)g_w - L - 16, ROW_H);
        /* The name in ink and the folder it is in dimmed after it: two facts
         * at different weights rather than one long line at one weight. */
        const char* name = leaf_of(path);
        wg_text_clipped(L + 12, y + 1, name, wg_ink_colour(), 220);
        wg_text_clipped(L + 240, y + 1, path, WG_DIM, (int)g_w - L - 256);
    }
    if (g_results_n == 0)
        wg_text(L + 12, top + 8, "nothing matched", WG_DIM);
}

/* The window's background. The components are drawn over it by the framework,
 * in the order they sit in the tree - and the content view is clipped to its
 * own frame, which is what lets the toolbar be a sibling above it rather than
 * something painted again afterwards. */
static void draw_background(struct app* a)
{
    /* The buffer and the size, taken every paint: the framework owns them and
     * a resize does not pass through here. */
    g_px = a->px;
    g_w  = a->w;
    g_h  = a->h;
    wg_theme();                 /* whatever settings last chose */
    wg_glass_clear();
}

static void draw_content(struct ui_view* v, void* user)
{
    (void)v; (void)user;
    /* The listing sits in its own well, the way a document area does - four
     * pixels in from the window's edge, so it gets the small corner that goes
     * with that. */
    wg_container(content_left() + 4, content_top(),
                 (int)g_w - content_left() - 8, content_h(), 4);

    if (g_searching)              draw_results();
    else if (g_view == VIEW_ICON) draw_icons();
    else if (g_view == VIEW_LIST) draw_list();
    else                          draw_tree();

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
}

static void draw_side(struct ui_view* v, void* user)
{
    (void)v; (void)user;
    draw_sidebar();
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
    /* A press in the sidebar is the sidebar's, in every view. Rows are drawn
     * from the content's left edge, so they are hit from there too. */
    if (x < content_left())
        return -1;
    if (g_view == VIEW_ICON) {
        const int cols = icon_cols();
        const int col = (x - content_left() - 8) / CELL_W;
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

static const char* const kContextMenu[] = {
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

/* What the menu does. Most of it was on the toolbar; the last three are
 * things this system has been able to do for a while and the file browser has
 * had no way to ask for. */
/* The three menus, dispatched by which one is down. The item numbers are
 * positions in the tables above, separators included, so the two stay in step
 * by being read from the same place. */
/* --- searching ---------------------------------------------------------------
 *
 * Walking the tree for every search would mean reading every directory on the
 * disk each time somebody typed a letter, which on a cold cache is seconds and
 * on a warm one is still hundreds of round trips to vfsd. So the walk happens
 * once and its result is kept: a flat list of paths, searched by substring.
 *
 * The index is written to disk as well, so a second run of the browser does not
 * pay for the first one's walk. That makes it possible for the index to be
 * older than the filesystem, which is a real cost and is why it is stated: a
 * search finds what was there when the index was built. Rebuilding is one item
 * in the View menu, and anything the index claims is checked with stat before
 * it is shown, so a stale entry is dropped rather than offered.
 */
static void index_path(char* out, int max)
{
    const char* home = getenv("HOME");
    if (home == 0 || home[0] == '\0')
        home = "/root";
    snprintf(out, (unsigned)max, "%s/.searchindex", home);
}

/* One directory's worth, recursively. `depth` bounds it: a link loop or a very
 * deep tree should not be able to turn a search into a hang, and eight levels
 * is past anything this filesystem holds. */
static void index_walk(const char* dir, int depth)
{
    if (depth > INDEX_DEPTH || g_index_n >= INDEX_MAX)
        return;
    /* Not the synthetic filesystems: /proc invents a file per process and
     * indexing it would fill the table with names that expire. */
    if (strncmp(dir, "/proc", 5) == 0 || strncmp(dir, "/dev", 4) == 0 ||
        strncmp(dir, "/sys", 4) == 0)
        return;

    /* Its own array per level rather than one shared scratch: recursion with a
     * shared buffer reads the parent's entries after the child has overwritten
     * them, which is a bug that looks like a corrupt disk. Sixty-four at a
     * time keeps the stack cost of eight levels reasonable. */
    struct dirent here[64];
    const int n = getdents(dir, here, 64);
    for (int i = 0; i < n && g_index_n < INDEX_MAX; ++i) {
        if (here[i].d_name[0] == '.')
            continue;
        char full[INDEX_PATH];
        if (strcmp(dir, "/") == 0)
            snprintf(full, sizeof(full), "/%s", here[i].d_name);
        else
            snprintf(full, sizeof(full), "%s/%s", dir, here[i].d_name);
        snprintf(g_index[g_index_n++], INDEX_PATH, "%s", full);
        if (here[i].d_type == S_IFDIR && !bundle_is_app(here[i].d_name))
            index_walk(full, depth + 1);
    }
}

static void index_save(void)
{
    char path[256];
    index_path(path, sizeof(path));
    FILE* out = fopen(path, "w");
    if (out == 0)
        return;
    for (int i = 0; i < g_index_n; ++i)
        fprintf(out, "%s\n", g_index[i]);
    fclose(out);
}

static void index_build(int force)
{
    if (g_index_ready && !force)
        return;
    g_index_n = 0;
    index_walk("/", 0);
    g_index_ready = 1;
    index_save();
    snprintf(g_status, sizeof(g_status), "indexed %d items", g_index_n);
}

/* Read a previous run's index. Cheap enough to do at startup: it is one file
 * and a few thousand short lines, against a walk of the whole disk. */
static void index_load(void)
{
    char path[256];
    index_path(path, sizeof(path));
    FILE* in = fopen(path, "r");
    if (in == 0)
        return;
    char line[INDEX_PATH];
    g_index_n = 0;
    while (g_index_n < INDEX_MAX && fgets(line, sizeof(line), in) != 0) {
        unsigned n = (unsigned)strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        if (n > 0)
            snprintf(g_index[g_index_n++], INDEX_PATH, "%s", line);
    }
    fclose(in);
    g_index_ready = g_index_n > 0;
}

static int fold(char c)
{
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

/* Substring, case-insensitive, against the last component - because a person
 * searching for "notes" means the file called notes, not every file in a
 * folder that happens to have "notes" in its path. */
static int name_contains(const char* path, const char* needle)
{
    const char* name = leaf_of(path);
    for (const char* p = name; *p != '\0'; ++p) {
        int i = 0;
        while (needle[i] != '\0' && fold(p[i]) == fold(needle[i]))
            ++i;
        if (needle[i] == '\0')
            return 1;
    }
    return 0;
}

static void search_run(void)
{
    g_results_n = 0;
    g_scroll = 0;
    g_selected = -1;
    if (g_query[0] == '\0') {
        g_searching = 0;
        return;
    }
    index_build(0);             /* built once, then reused */
    g_searching = 1;
    for (int i = 0; i < g_index_n && g_results_n < INDEX_MAX; ++i) {
        if (!name_contains(g_index[i], g_query))
            continue;
        /* Checked against the disk, so an entry left over from before a file
         * was deleted is dropped rather than offered as a result. */
        struct stat st;
        if (stat(g_index[i], &st) != 0)
            continue;
        g_results[g_results_n++] = i;
    }
    snprintf(g_status, sizeof(g_status), "%d result%s for \"%s\"",
             g_results_n, g_results_n == 1 ? "" : "s", g_query);
}

/* Detach whatever volume we are looking at, if it is one. */
static void unmount_here(void)
{
    int found = -1;
    for (int i = 0; i < g_places; ++i)
        if (g_place[i].volume && strcmp(g_place[i].path, g_path) == 0)
            found = i;
    if (found < 0) {
        snprintf(g_status, sizeof(g_status), "go to a mounted volume first");
        return;
    }
    char at[192];
    snprintf(at, sizeof(at), "%s", g_place[found].path);
    goto_path("/", 1);
    if (fs_umount(at) == 0) {
        places_build();
        snprintf(g_status, sizeof(g_status), "unmounted %s", at);
    } else {
        snprintf(g_status, sizeof(g_status), "%s will not detach", at);
    }
}

static void menu_action(int menu, int item)
{
    char full[512];
    if (menu == M_VIEW) {
        if (item == 0)      { g_view = VIEW_ICON; g_scroll = 0; }
        else if (item == 1) { g_view = VIEW_LIST; g_scroll = 0; }
        else if (item == 2) { g_view = VIEW_TREE; g_scroll = 0; rebuild_tree(); }
        else if (item == 4) { read_dir(); if (g_view == VIEW_TREE) rebuild_tree(); }
        else if (item == 5) index_build(1);
        return;
    }
    if (menu == M_GO) {
        const char* home = getenv("HOME");
        if (home == 0 || home[0] == '\0') home = "/root";
        if (item == 0)      go_history(-1);
        else if (item == 1) go_history(1);
        else if (item == 2) go_up();
        else if (item == 4) goto_path(home, 1);
        else if (item == 5) goto_path(PATH_APPS, 1);
        else if (item == 6) goto_path("/", 1);
        else if (item == 8) unmount_here();
        return;
    }
    switch (item) {
    case 0:
        if (g_selected >= 0) {
            join(g_path, g_entries[g_selected].d_name, full, sizeof(full));
            open_path(full, g_entries[g_selected].d_type == S_IFDIR);
        }
        break;
    case 1:
        if (g_selected >= 0)
            begin_rename();
        break;
    case 3: {
        /* A name that is not taken, so this never fails for a reason the
         * person has to think about. */
        for (int n = 1; n < 100; ++n) {
            char name[32];
            snprintf(name, sizeof(name), n == 1 ? "untitled folder"
                                                : "untitled folder %d", n);
            join(g_path, name, full, sizeof(full));
            struct stat st;
            if (stat(full, &st) == 0)
                continue;
            if (mkdir(full) == 0) {
                read_dir();
                snprintf(g_status, sizeof(g_status), "made %s", name);
            } else {
                snprintf(g_status, sizeof(g_status), "cannot create a folder here");
            }
            break;
        }
        break;
    }
    case 4:
        if (g_selected >= 0) {
            join(g_path, g_entries[g_selected].d_name, full, sizeof(full));
            const int dir = g_entries[g_selected].d_type == S_IFDIR;
            const int ok = (unlink(full) == 0);   /* unlink removes both here */
            snprintf(g_status, sizeof(g_status), ok ? "deleted"
                     : dir ? "the folder is not empty" : "cannot delete that");
            (void)dir;
            if (ok) { g_selected = -1; read_dir(); }
        }
        break;
    case 6:
        if (g_selected >= 0) {
            join(g_path, g_entries[g_selected].d_name, full, sizeof(full));
            struct stat st;
            if (lstat(full, &st) != 0) {
                snprintf(g_status, sizeof(g_status), "cannot read it");
                break;
            }
            /* Everything the filesystem knows, including the parts only this
             * system's own tools could show before: what kind of node it is,
             * which driver a device names, and where a link points. */
            const char* kind = st.st_type == S_IFDIR  ? "folder"
                             : st.st_type == S_IFLNK  ? "symbolic link"
                             : st.st_type == S_IFIFO  ? "fifo"
                             : st.st_type == S_IFCHR  ? "character device"
                             : st.st_type == S_IFBLK  ? "block device"
                                                      : "file";
            if (st.st_type == S_IFCHR || st.st_type == S_IFBLK)
                snprintf(g_status, sizeof(g_status),
                         "%s  device %u,%u  mode %04o  uid %u",
                         kind, major(st.st_rdev), minor(st.st_rdev),
                         st.st_mode & 0777, st.st_uid);
            else if (st.st_type == S_IFLNK) {
                char target[128];
                const long got = readlink(full, target, sizeof(target) - 1);
                target[got > 0 ? got : 0] = '\0';
                snprintf(g_status, sizeof(g_status), "%s -> %s", kind,
                         got > 0 ? target : "(unreadable)");
            } else
                snprintf(g_status, sizeof(g_status),
                         "%s  %lu bytes  mode %04o  uid %u  inode %u",
                         kind, (unsigned long)st.st_size, st.st_mode & 0777,
                         st.st_uid, st.st_ino);
        }
        break;
    }
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

/* --- the menus ------------------------------------------------------------
 *
 * Three of them in the bar, plus the two that come up on a right-click and the
 * one an application bundle declares. The bar is a component; the other three
 * are the pop-up menu the framework already routes for us, which is why they
 * arrive here as a pick rather than as a press to hit-test.
 */

static const char* menu_title(void* user, int i)
{
    (void)user;
    return (i >= 0 && i < M_COUNT) ? kMenus[i].title : "";
}

/* One callback for the whole bar, asked as menu * 100 + item. */
static const char* menu_item(void* user, int code)
{
    (void)user;
    const int m = code / 100, i = code % 100;
    if (m < 0 || m >= M_COUNT || i < 0 || i >= kMenus[m].count)
        return "";
    return kMenus[m].items[i];
}

static void on_menubar(struct ui_view* v, void* user)
{
    (void)user;
    const int m = v->selected / 100, i = v->selected % 100;
    if (m < 0 || m >= M_COUNT || i < 0 || i >= kMenus[m].count)
        return;
    /* A separator is a line, not a choice. */
    if (strcmp(kMenus[m].items[i], "-") == 0)
        return;
    menu_action(m, i);
}

static void on_search(struct ui_view* v, void* user)
{
    (void)user;
    snprintf(g_query, sizeof(g_query), "%s", ui_text(v));
    /* Searched as you type, which the index makes cheap: the walk has already
     * happened and this is a scan of a few thousand strings. */
    search_run();
}

/* Is this point on that view? Used to tell a press on the title band that
 * belongs to a control from one that should move the window. */
static int on_view(const struct ui_view* v, int x, int y)
{
    if (v == 0)
        return 0;
    return x >= v->frame.x && y >= v->frame.y &&
           x < v->frame.x + v->frame.w && y < v->frame.y + v->frame.h;
}

/* The pop-up menu, once something has been chosen from it. */
static int on_menu_pick(struct app* a, int pick)
{
    (void)a;
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
        return 1;
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
        return 1;
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
        return 1;
    return 1;
}

/* A file dialogue is modal: it is drawn over the window and nothing behind it
 * may be pressed while it is up. */
static int on_filter(struct app* a, const struct win_event* e)
{
    (void)a;
    if (!dlg_active())
        return 0;
    /* Once, and once only: the dialogue closes itself on accept, so asking it
     * twice would leave the second caller looking at one that is not there. */
    /* Once, and once only: the dialogue closes itself on accept,
     * so asking it twice would leave the second caller looking at
     * a dialogue that is no longer there. */
    const int answer = dlg_event(e);
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
    return 1;
}

static void on_overlay(struct app* a)
{
    dlg_draw((int)a->w, (int)a->h);
}

/* Everything the components did not deal with: the listing, the sidebar's
 * pins, the drags, the marquee and the keyboard. */
static int on_event(struct app* a, const struct win_event* e)
{
    /* A press a component has already taken is not also a press on the
     * listing. The menus drop over the content, so without this, choosing
     * "as List" from the View menu also selected whatever folder happened to
     * be under the word. */
    if (a->handled && e->type == WIN_EVENT_MOUSE_DOWN)
        return 0;
    if (e->type == WIN_EVENT_MOUSE_DOWN) {
        /* This window owns its title strip, so the server no longer
         * treats a press there as a drag. Anything in that band which
         * is not one of ours is handed back as one - which is how the
         * window still moves when you grab the bar. */
        if (on_view(g_v_nav, e->x, e->y)) {
            /* Three arrows sharing one pill: which third was pressed is
             * which one it was. Measured from the frame rather than from
             * boxes the drawing filled in, so a press arriving between a
             * resize and the paint that follows is still answered against
             * where the arrows actually are. */
            const int third = g_v_nav->frame.w / 3;
            const int at = (e->x - g_v_nav->frame.x) /
                           (third > 0 ? third : 1);
            if (at <= 0)      go_history(-1);
            else if (at == 1) go_history(1);
            else              go_up();
            return 1;
        }
        if (e->y < TOOLBAR_H && e->x >= SIDEBAR_W &&
            !on_view(g_v_search, e->x, e->y) &&
            !on_view(g_v_menu, e->x, e->y)) {
            win_move_begin(a->id);
            return 1;
        }
        if (e->y < TOOLBAR_H && e->x >= SIDEBAR_W)
            return 0;   /* the components have already had it */
        if (e->x < SIDEBAR_W) {
            /* A press in the sidebar might be a click or the start of
             * dragging a pin out; which it was is only known at the
             * release, so both are set up here. */
            const int row = sidebar_row(e->y);
            g_pin_drag = row;
            g_pin_out = 0;
            if (row >= 0) {
                goto_path(g_place[row].path, 1);
                g_scroll = 0;
                g_selected = -1;
                g_searching = 0;
            }
        } else if (e->x >= bar_x() && e->y >= content_top() &&
                   e->y < content_top() + content_h()) {
            /* The bar was drawn but never listened to, which made it
             * look broken rather than absent. */
            if (wg_scroll_on_thumb_v(e->y, content_top(), content_h(),
                                     g_scroll, content_h(),
                                     content_span()))
                g_bar_drag = 1;
            else
                scroll_to(wg_scroll_hit_v(e->x, e->y, bar_x(),
                    content_top(), content_h(), g_scroll,
                    content_h(), content_span()));
        } else if (e->button == 1 && e->y >= content_top() &&
                   hit_test(e->x, e->y) < 0) {
            /* A press on empty space starts a rubber band rather than
             * doing nothing. */
            /* Clicking the empty space around the items lets go of
             * them - which is the only way to end up with nothing
             * selected once something is. */
            g_band = 1;
            g_band_x = g_band_x2 = e->x;
            g_band_y = g_band_y2 = e->y;
            g_selected = -1;
            memset(g_marked, 0, sizeof(g_marked));
            snprintf(g_status, sizeof(g_status), "%d item%s", g_count,
                     g_count == 1 ? "" : "s");
        } else if (e->button == 2) {
            const int hit = hit_test(e->x, e->y);
            if (hit >= 0) {
                if (!g_marked[hit])
                    select_at(hit, 0);
                char full[256];
                join(g_path, g_entries[hit].d_name, full, sizeof(full));
                if (bundle_is_app(full) && build_app_menu(full) > 0) {
                    menu_open(e->x, e->y, g_app_menu_p,
                              g_app_menu_n);
                    g_app_menu_on = 1;
                } else {
                    menu_open(e->x, e->y, kContextMenu, 7);
                }
            } else {
                menu_open(e->x, e->y, kBlankMenu, 7);
                g_blank_menu = 1;
            }
        } else {
            const int hit = hit_test(e->x, e->y);
            if (hit >= 0 && g_view != VIEW_TREE) {
                /* Remember where this began. It becomes a drag only
                 * once the pointer has travelled far enough that it
                 * cannot be a click with an unsteady hand. */
                g_press_item = hit;
                g_press_x = e->x;
                g_press_y = e->y;
            }
            if (hit >= 0) {
                /* Click to select, click again to open - the same
                 * gesture in every view, and no timing to get wrong.
                 * A modified click is always a selection, never an
                 * open: ctrl-clicking a thing twice must not launch it. */
                const uint32_t m = e->modifiers;
                if (hit == g_selected && !(m & (WIN_MOD_CTRL | WIN_MOD_SHIFT)))
                    open_selected();
                else
                    select_at(hit, m);
            }
        }
    } else if (e->type == WIN_EVENT_MOUSE_MOVE && g_pin_drag >= 0) {
        /* Far enough out of the sidebar to mean it. Half the sidebar's
         * width past its edge: far enough that a shaky click cannot
         * unpin anything, close enough that the gesture is one motion. */
        const int out = e->x > SIDEBAR_W + SIDEBAR_W / 2;
        if (out != g_pin_out) {
            g_pin_out = out;
            return 1;
        }
        return 0;
    } else if (e->type == WIN_EVENT_MOUSE_MOVE && g_press_item >= 0 &&
               !win_dragging()) {
        const int dx = e->x - g_press_x, dy = e->y - g_press_y;
        if (dx * dx + dy * dy > 25) {       /* five pixels */
            const int i = g_press_item;
            char full[256];
            join(g_path, g_entries[i].d_name, full, sizeof(full));
            int cx, cy, ox, oy;
            cell_at(i, &cx, &cy);
            win_origin(a->id, &ox, &oy);
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
    } else if (e->type == WIN_EVENT_MOUSE_MOVE && g_bar_drag) {
        scroll_to(wg_scroll_drag_v(e->y, content_top(), content_h(),
                                   content_h(), content_span()));
    } else if (e->type == WIN_EVENT_MOUSE_MOVE && g_band) {
        g_band_x2 = e->x;
        g_band_y2 = e->y;
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
    } else if (e->type == WIN_EVENT_DROP) {
        /* The payload is still in the drag record: the server leaves
         * it there until the next drag begins, precisely so the
         * receiver can read it when the drop arrives. */
        char from[256];
        snprintf(from, sizeof(from), "%s", win_drop_path());
        /* Onto the sidebar is a pin, not a move. There is nowhere for
         * a file to go in a list of places, and "put this where I can
         * reach it" is what dropping it there plainly means. */
        if (e->x < SIDEBAR_W) {
            struct stat st;
            if (stat(from, &st) == 0 && st.st_type == S_IFDIR) {
                place_pin(from);
                win_drop_accept(e->x, e->y);
                snprintf(g_status, sizeof(g_status), "pinned %s",
                         leaf_of(from));
            } else {
                win_drop_reject();
                snprintf(g_status, sizeof(g_status),
                         "only folders can be pinned");
            }
            g_press_item = -1;
            return 1;
        }
        const int hit = hit_test(e->x, e->y);
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
    } else if (e->type == WIN_EVENT_MOUSE_UP) {
        /* A pin dragged clear of the sidebar and let go is unpinned.
         * Dragging out is the gesture people already expect for this,
         * and it needs no menu item and no confirmation - putting it
         * back is one drag the other way. */
        if (g_pin_drag >= 0 && g_pin_out) {
            place_unpin(g_pin_drag);
            snprintf(g_status, sizeof(g_status), "unpinned");
        }
        g_pin_drag = -1;
        g_pin_out = 0;
        g_band = 0;
        g_bar_drag = 0;
        g_press_item = -1;
    } else if (e->type == WIN_EVENT_KEY) {
        /* The search field is a component and takes its own keys.
         * While it has the caret, none of these single-letter
         * shortcuts may fire - or typing "list" into it would change
         * the view three times. */
        if (ui_focused() == g_v_search) {
            if (e->key == 27) {
                ui_set_text(g_v_search, "");
                ui_focus(0);
                g_query[0] = '\0';
                g_searching = 0;
                g_status[0] = '\0';
                return 1;
            }
            return 0;
        }
        /* Arrows move the selection, not the view: moving the view and
         * leaving the selection behind is how you lose your place. The
         * view follows whatever is chosen. */
        if (e->key != '\n' && e->key != '\r')
            g_enter_armed = 0;
        const int limit = g_searching ? g_results_n
                        : (g_view == VIEW_TREE) ? g_row_count : g_count;
        const int per_row = (g_view == VIEW_ICON && !g_searching)
                          ? icon_cols() : 1;
        if (e->key == WIN_KEY_DOWN || e->key == WIN_KEY_UP ||
            (g_view == VIEW_ICON &&
             (e->key == WIN_KEY_LEFT || e->key == WIN_KEY_RIGHT))) {
            int to = g_selected < 0 ? 0 : g_selected;
            if (e->key == WIN_KEY_DOWN)       to += per_row;
            else if (e->key == WIN_KEY_UP)    to -= per_row;
            else if (e->key == WIN_KEY_RIGHT) to += 1;
            else                                 to -= 1;
            if (to >= 0 && to < limit)
                select_at(to, 0);
            reveal_selected();
        } else if (e->key == WIN_KEY_RIGHT) {
            scroll_to(g_scroll + content_h());      /* a page */
        } else if (e->key == WIN_KEY_LEFT) {
            scroll_to(g_scroll - content_h());
        } else if (e->key == 1) {    /* ctrl+a */
            select_all();
        } else if (e->key == 3) {    /* ctrl+c */
            copy_marked();
        } else if (e->key == '\n' || e->key == '\r') {
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
        else if (e->key == 'u')
            go_up();
        else if (e->key == 'i')
            { g_view = VIEW_ICON; g_scroll = 0; }
        else if (e->key == 'l')
            { g_view = VIEW_LIST; g_scroll = 0; }
        else if (e->key == 't')
            { g_view = VIEW_TREE; g_scroll = 0; rebuild_tree(); }
    }
    return 1;
}

/* Something dragged out of this folder into another window leaves no trace
 * here but the drag ending. */
static int on_tick(struct app* a)
{
    (void)a;
    static int was;
    const int now = win_dragging();
    const int ended = (was && !now);
    was = now;
    if (ended)
        read_dir();
    return ended;
}

int main(int argc, char** argv)
{
    for (int i = 0; i < M_COUNT; ++i)
        g_menu_counts[i] = kMenus[i].count;

    places_build();
    /* The previous run's index, if there is one: a walk of the whole disk is
     * what this avoids, and reading a few thousand short lines is not. */
    index_load();

    struct ui_view* root = ui_box(0, UI_STACK_H, 0, 0);

    /* The sidebar is the window's spine, full height and its own drawing: it
     * has pins that can be dragged out of it, which no list component does. */
    ui_grow(ui_size(ui_custom(root, draw_side, 0), SIDEBAR_W, 0), 0);

    struct ui_view* right = ui_box(root, UI_STACK_V, 0, 0);
    ui_grow(right, 1);

    /* One band, and now genuinely one: this window draws its own title strip,
     * so navigation, path, search and menus share the line the title used to
     * have to itself. */
    struct ui_view* bar = ui_box(right, UI_STACK_H, 4, 8);
    ui_grow(ui_size(bar, 0, TOOLBAR_H), 0);
    g_v_nav = ui_grow(ui_size(ui_custom(bar, draw_nav, 0), 84, 22), 0);
    ui_grow(ui_custom(bar, draw_path, 0), 1);
    g_v_search = ui_grow(ui_size(ui_search(bar, "Search"), 140, 22), 0);
    ui_on(g_v_search, on_search, 0);
    g_v_menu = ui_grow(ui_size(ui_menubar(bar, menu_title, M_COUNT, menu_item,
                                          g_menu_counts, 0), 146, 22), 0);
    ui_on(g_v_menu, on_menubar, 0);

    g_v_content = ui_grow(ui_custom(right, draw_content, 0), 1);

    /* Where we start is somewhere we have been, so back can come home to it. */
    remember_place(g_path);
    read_dir();

    g_app.title = "Files";
    g_app.width = g_w; g_app.height = g_h;
    /* Wide enough that the path still has somewhere to be once the sidebar,
     * the arrows, the search box and three menus have had their share of the
     * one band they all live in. Below this it was squeezed to nothing and
     * simply stopped drawing. */
    g_app.min_width = 640; g_app.min_height = 300;
    /* Its pixels carry alpha, so the glass reaches past the title bar - and
     * this window draws that bar itself, which is what puts the controls on
     * the same line as the title rather than in a band beneath it. */
    g_app.sidebar = SIDEBAR_W;
    g_app.client_title = 1;
    g_app.root = root;
    g_app.draw = draw_background;
    g_app.event = on_event;
    g_app.filter = on_filter;
    g_app.overlay = on_overlay;
    g_app.menu_pick = on_menu_pick;
    g_app.tick_ms = 60;
    g_app.tick = on_tick;
    return app_run(&g_app, argc, argv);
}
