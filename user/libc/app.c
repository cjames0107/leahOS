/* app_run - the loop every window program was writing for itself.
 *
 * See <app.h> for why this exists. The rules it enforces, which are the ones
 * the hand-written copies kept getting wrong:
 *
 *  - After a resize the pixel buffer is a different buffer. It has to be
 *    re-mapped and the toolkit re-pointed at it, in that order, before
 *    anything draws. An application that missed either drew into the old
 *    mapping and left the new one full of whatever was there.
 *
 *  - The menu eats events while it is open, except a resize, which nothing may
 *    swallow.
 *
 *  - Drawing happens once per pass through the pending events, not once per
 *    event. A drag delivers a stream of motion, and redrawing per motion event
 *    is how a window server ends up composing the same frame five times.
 */

#include <app.h>
#include <dialog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void app_quit(struct app* a, int status)
{
    a->quit = 1;
    a->status = status;
}

static void paint(struct app* a);

void app_redraw(struct app* a)
{
    if (a->px != 0)
        paint(a);
}

/* Take the new size and make every view of it agree. */
static int resized(struct app* a, int w, int h)
{
    a->w = (unsigned)w;
    a->h = (unsigned)h;
    a->px = win_map(a->id);
    if (a->px == 0)
        return -1;
    wg_target(a->px, a->w, a->h);
    /* The components are laid out against the window, so a new window size is
     * a new layout - and it happens here rather than at the next draw, because
     * an event may arrive first and would otherwise be hit-tested against the
     * old frames. */
    app_relayout(a);
    return 0;
}

/* Paint: the application's own background if it has one, then its components
 * over the top. */
static void paint(struct app* a)
{
    if (a->draw != 0)
        a->draw(a);
    else {
        wg_theme();
        wg_glass_clear();
    }
    if (a->root != 0)
        ui_draw(a->root);
    if (a->overlay != 0)
        a->overlay(a);
    menu_draw();
    win_present(a->id);

    /* Then the sheet, into its own buffer. The toolkit draws wherever it was
     * last pointed, so the two are painted one after the other rather than
     * together - and it is pointed back at the window afterwards, because
     * every other part of this loop assumes that. */
    if (app_sheet_open(a)) {
        wg_target(a->sheet_px, a->sheet_w, a->sheet_h);
        wg_theme();
        wg_glass_clear();
        const struct ui_rect all = { 0, 0, (int)a->sheet_w, (int)a->sheet_h };
        ui_layout(a->sheet, all);
        ui_draw(a->sheet);
        win_present(a->sheet_id);
        wg_target(a->px, a->w, a->h);
    }
}

int app_sheet_open(const struct app* a)
{
    return a != 0 && a->sheet != 0 && a->sheet_id >= 0;
}

struct ui_view* app_sheet(struct app* a, unsigned w, unsigned h)
{
    if (a == 0)
        return 0;
    if (app_sheet_open(a))
        app_sheet_close(a, 0);      /* one at a time; see app.h */

    int px = 0, py = 0;
    win_origin(a->id, &px, &py);
    /* Centred on the window it belongs to, which is what makes it read as
     * that window's question rather than as another window that happens to
     * have appeared. */
    const int x = px + ((int)a->w - (int)w) / 2;
    const int y = py + ((int)a->h - (int)h) / 2;

    a->sheet_id = win_create(x > 0 ? x : 0, y > 0 ? y : 0, w, h, "");
    if (a->sheet_id < 0) {
        a->sheet = 0;
        return 0;
    }
    win_set_sheet(a->sheet_id);
    win_set_alpha(a->sheet_id);
    a->sheet_px = win_map(a->sheet_id);
    if (a->sheet_px == 0) {
        win_destroy(a->sheet_id);
        a->sheet_id = -1;
        a->sheet = 0;
        return 0;
    }
    a->sheet_w = w;
    a->sheet_h = h;

    /* Its own root, and the frames are laid out when it is first painted. */
    a->sheet = ui_box(0, UI_STACK_V, 16, 10);
    return a->sheet;
}

void app_sheet_close(struct app* a, int result)
{
    if (a == 0 || a->sheet_id < 0)
        return;
    win_destroy(a->sheet_id);
    a->sheet_id = -1;
    a->sheet = 0;
    a->sheet_px = 0;
    /* The toolkit is pointed back at the window before anything else draws,
     * or the next paint goes into a buffer that has been destroyed. */
    if (a->px != 0)
        wg_target(a->px, a->w, a->h);
    if (a->sheet_done != 0)
        a->sheet_done(a, result);
}

/* --- the saving sheet -----------------------------------------------------
 *
 * Provided rather than left to each application, because "where shall I put
 * this" is the same question everywhere and was previously answered by a
 * dialogue drawn over the document it was asking about.
 */
static char g_sheet_dir[192];
static char g_sheet_path[256];
static struct ui_view* g_sheet_field;

const char* app_sheet_path(const struct app* a)
{
    (void)a;
    return g_sheet_path;
}

static void sheet_save_clicked(struct ui_view* v, void* user)
{
    (void)v;
    struct app* a = (struct app*)user;
    const char* name = ui_text(g_sheet_field);
    if (name == 0 || name[0] == '\0') {
        app_sheet_close(a, 0);
        return;
    }
    /* A name with a slash in it is a path and is taken as written; anything
     * else is a name inside the directory being shown. */
    if (name[0] == '/')
        snprintf(g_sheet_path, sizeof(g_sheet_path), "%s", name);
    else if (g_sheet_dir[0] != '\0' && g_sheet_dir[strlen(g_sheet_dir) - 1] == '/')
        snprintf(g_sheet_path, sizeof(g_sheet_path), "%s%s", g_sheet_dir, name);
    else
        snprintf(g_sheet_path, sizeof(g_sheet_path), "%s/%s", g_sheet_dir, name);
    app_sheet_close(a, 1);
}

static void sheet_cancel_clicked(struct ui_view* v, void* user)
{
    (void)v;
    app_sheet_close((struct app*)user, 0);
}

struct ui_view* app_sheet_save(struct app* a, const char* dir,
                               const char* suggested)
{
    snprintf(g_sheet_dir, sizeof(g_sheet_dir), "%s", dir != 0 ? dir : "/");
    g_sheet_path[0] = '\0';

    struct ui_view* root = app_sheet(a, 420, 150);
    if (root == 0)
        return 0;

    ui_grow(ui_label(root, "Save as"), 0);
    char where[220];
    snprintf(where, sizeof(where), "in %s", g_sheet_dir);
    ui_grow(ui_label(root, where), 0);

    g_sheet_field = ui_field(root, suggested != 0 ? suggested : "");
    ui_grow(g_sheet_field, 0);
    /* Return in the field is the same as pressing Save, which is what anyone
     * typing a filename expects to be able to do. */
    ui_on(g_sheet_field, sheet_save_clicked, a);
    ui_focus(g_sheet_field);

    ui_spacer(root);
    struct ui_view* row = ui_box(root, UI_STACK_H, 0, 10);
    ui_size(row, 0, 26);
    ui_grow(row, 0);
    ui_spacer(row);
    ui_grow(ui_button(row, "Cancel", sheet_cancel_clicked, a), 0);
    ui_grow(ui_button(row, "Save", sheet_save_clicked, a), 0);
    return root;
}

/* --- the file panel -------------------------------------------------------
 *
 * A sheet for choosing a file. The listing is taken once when the panel opens
 * and again whenever a directory is entered, rather than being read per row:
 * a row callback that hit the filesystem would read the whole directory once
 * per visible line, per repaint.
 */
#define PANEL_MAX 128
static struct dirent g_panel[PANEL_MAX];
static int  g_panel_n;
static char g_panel_dir[192];
static struct ui_view* g_panel_list;
static struct ui_view* g_panel_where;

static void panel_read(void)
{
    g_panel_n = getdents(g_panel_dir, g_panel, PANEL_MAX);
    if (g_panel_n < 0)
        g_panel_n = 0;
}

static const char* panel_row(void* user, int row)
{
    (void)user;
    if (row < 0 || row >= g_panel_n)
        return "";
    /* Directories are marked, because "enter" and "choose" are different
     * things to do with a row and the person has to know which they will get. */
    static char line[128];
    if (g_panel[row].d_type == S_IFDIR)
        snprintf(line, sizeof(line), "%s/", g_panel[row].d_name);
    else
        snprintf(line, sizeof(line), "%s", g_panel[row].d_name);
    return line;
}

static void panel_chosen(struct ui_view* v, void* user)
{
    struct app* a = (struct app*)user;
    const int row = v->selected;
    if (row < 0 || row >= g_panel_n)
        return;
    if (g_panel[row].d_type != S_IFDIR) {
        if (g_panel_dir[0] != '\0' &&
            g_panel_dir[strlen(g_panel_dir) - 1] == '/')
            snprintf(g_sheet_path, sizeof(g_sheet_path), "%s%s",
                     g_panel_dir, g_panel[row].d_name);
        else
            snprintf(g_sheet_path, sizeof(g_sheet_path), "%s/%s",
                     g_panel_dir, g_panel[row].d_name);
        app_sheet_close(a, 1);
        return;
    }
    /* A directory is entered rather than answered with. */
    char into[192];
    if (strcmp(g_panel[row].d_name, "..") == 0) {
        snprintf(into, sizeof(into), "%s", g_panel_dir);
        char* slash = strrchr(into, '/');
        if (slash != 0 && slash != into) *slash = '\0';
        else                             into[1] = '\0';
    } else if (strcmp(g_panel_dir, "/") == 0) {
        snprintf(into, sizeof(into), "/%s", g_panel[row].d_name);
    } else {
        snprintf(into, sizeof(into), "%s/%s", g_panel_dir,
                 g_panel[row].d_name);
    }
    snprintf(g_panel_dir, sizeof(g_panel_dir), "%s", into);
    panel_read();
    if (g_panel_list != 0) {
        g_panel_list->rows = g_panel_n;
        g_panel_list->selected = -1;
        g_panel_list->scroll = 0;
    }
    ui_set_text(g_panel_where, g_panel_dir);
}

struct ui_view* app_sheet_file(struct app* a, const char* dir)
{
    snprintf(g_panel_dir, sizeof(g_panel_dir), "%s", dir != 0 ? dir : "/");
    g_sheet_path[0] = '\0';
    panel_read();

    struct ui_view* root = app_sheet(a, 460, 320);
    if (root == 0)
        return 0;
    ui_grow(ui_label(root, "Choose a file"), 0);
    g_panel_where = ui_label(root, g_panel_dir);
    ui_grow(g_panel_where, 0);

    g_panel_list = ui_list(root, panel_row, g_panel_n, 0);
    ui_on(g_panel_list, panel_chosen, a);

    struct ui_view* row = ui_box(root, UI_STACK_H, 0, 10);
    ui_size(row, 0, 26);
    ui_grow(row, 0);
    ui_spacer(row);
    ui_grow(ui_button(row, "Cancel", sheet_cancel_clicked, a), 0);
    return root;
}

/* --- the date panel ------------------------------------------------------- */

static struct ui_view* g_date_cal;

static void date_ok(struct ui_view* v, void* user)
{
    (void)v;
    struct app* a = (struct app*)user;
    /* The answer goes back the way every other sheet's does, as text: a date
     * is a string to whatever asked for one, and inventing a second way to
     * return an answer would mean two of them to remember. */
    if (g_date_cal != 0)
        snprintf(g_sheet_path, sizeof(g_sheet_path), "%04d-%02d-%02d",
                 g_date_cal->year, g_date_cal->month + 1, g_date_cal->day);
    app_sheet_close(a, 1);
}

struct ui_view* app_sheet_date(struct app* a, int year, int month, int day)
{
    g_sheet_path[0] = '\0';
    struct ui_view* root = app_sheet(a, 260, 250);
    if (root == 0)
        return 0;
    ui_grow(ui_label(root, "Choose a date"), 0);
    g_date_cal = ui_calendar(root, year, month, day);
    ui_grow(g_date_cal, 0);

    struct ui_view* row = ui_box(root, UI_STACK_H, 0, 10);
    ui_size(row, 0, 26);
    ui_grow(row, 0);
    ui_spacer(row);
    ui_grow(ui_button(row, "Cancel", sheet_cancel_clicked, a), 0);
    ui_grow(ui_button(row, "Choose", date_ok, a), 0);
    return root;
}

void app_relayout(struct app* a)
{
    if (a == 0 || a->root == 0)
        return;
    const struct ui_rect all = { 0, 0, (int)a->w, (int)a->h };
    ui_layout(a->root, all);
}

int app_run(struct app* a, int argc, char** argv)
{
    /* One or the other has to exist, or there is nothing to show. */
    if (a == 0 || (a->draw == 0 && a->root == 0))
        return 1;
    if (wg_font() != 0)
        return 1;

    const int x = argc > 1 ? atoi_simple(argv[1]) : 160;
    const int y = argc > 2 ? atoi_simple(argv[2]) : 120;
    if (a->width == 0)  a->width = 640;
    if (a->height == 0) a->height = 420;

    a->id = win_create(x, y, a->width, a->height,
                       a->title != 0 ? a->title : "Window");
    if (a->id < 0) {
        /* Either there is no server or it has no free slot, and from here the
         * two look the same - so say what happened rather than guessing why. */
        printf("%s: could not open a window\n",
               a->title != 0 ? a->title : "app");
        return 1;
    }
    /* Alpha unless the application asks to be opaque, because the glass
     * reaching into a window is the default look and forgetting to ask for it
     * was the single most common way a new window came out wrong. */
    if (!a->opaque)
        win_set_alpha(a->id);
    if (a->client_title)
        win_set_client_title(a->id);
    if (a->sidebar > 0)
        win_set_sidebar(a->id, a->sidebar);
    if (a->min_width > 0 || a->min_height > 0)
        win_set_min_size(a->id, a->min_width, a->min_height);

    a->w = a->width;
    a->h = a->height;
    a->px = win_map(a->id);
    if (a->px == 0)
        return 1;
    wg_target(a->px, a->w, a->h);

    app_relayout(a);
    paint(a);

    /* The tick is counted in fifteen-millisecond passes rather than measured,
     * which is what the hand-written loops did and is accurate enough for
     * something whose purpose is "about twice a second". */
    const unsigned every = a->tick_ms > 0 ? (a->tick_ms / 15 + 1) : 0;
    unsigned since = 0;

    while (!a->quit) {
        int dirty = 0;
        struct win_event e;

        /* The sheet's events first, and while one is up the window's are read
         * and dropped. That is what modal means here: the question has to be
         * answered before the thing that asked it can be used again. */
        while (app_sheet_open(a) && win_poll(a->sheet_id, &e)) {
            if (e.type == WIN_EVENT_CLOSE) {
                app_sheet_close(a, 0);
                dirty = 1;
                break;
            }
            wg_target(a->sheet_px, a->sheet_w, a->sheet_h);
            dirty |= ui_event(a->sheet, &e);
            wg_target(a->px, a->w, a->h);
        }
        if (app_sheet_open(a)) {
            struct win_event ignored;
            while (win_poll(a->id, &ignored)) {
                /* Except the ones that cannot wait: a resize has already
                 * happened by the time it is delivered, and a close is the
                 * person deciding the whole window should go. */
                if (ignored.type == WIN_EVENT_RESIZE) {
                    if (resized(a, ignored.x, ignored.y) != 0)
                        return 1;
                    dirty = 1;
                } else if (ignored.type == WIN_EVENT_CLOSE) {
                    app_sheet_close(a, 0);
                    win_destroy(a->id);
                    return a->status;
                }
            }
            if (dirty)
                paint(a);
            msleep(15);
            continue;
        }

        while (win_poll(a->id, &e)) {
            if (e.type == WIN_EVENT_CLOSE) {
                win_destroy(a->id);
                return a->status;
            }
            if (e.type == WIN_EVENT_RESIZE) {
                if (resized(a, e.x, e.y) != 0)
                    return 1;
                dirty = 1;
                continue;
            }
            if (menu_active()) {
                const int pick = menu_event(&e);
                if (pick >= 0 && a->menu_pick != 0)
                    dirty |= a->menu_pick(a, pick);
                dirty = 1;              /* the menu itself has to be redrawn */
                continue;
            }
            /* Components first: they are the interface, and an application's
             * own handler is for what is left over. A view that took the event
             * says so by asking for a redraw, and the handler still sees it -
             * an application may want to know about a click its list already
             * dealt with. */
            if (a->filter != 0 && a->filter(a, &e) != 0) {
                dirty = 1;
                continue;
            }
            a->handled = 0;
            if (a->root != 0) {
                a->handled = ui_event(a->root, &e);
                dirty |= a->handled;
            }
            if (a->event != 0)
                dirty |= a->event(a, &e);
            if (e.type == WIN_EVENT_MOUSE_DOWN && e.button == 2 &&
                a->menu != 0 && a->menu_count > 0 && !menu_active()) {
                menu_open(e.x, e.y, a->menu, a->menu_count);
                dirty = 1;
            }
        }

        if (every > 0 && ++since >= every) {
            since = 0;
            if (a->tick != 0)
                dirty |= a->tick(a);
        }

        if (dirty && !a->quit)
            paint(a);
        msleep(15);
    }

    win_destroy(a->id);
    return a->status;
}
