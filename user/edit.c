/* edit - a text editor, and what the browser hands a document to.
 *
 * The buffer is one flat array of bytes with the newlines left in, rather than
 * an array of lines. That makes insertion and deletion a memmove and makes
 * saving a single write, at the cost of recomputing where the lines start
 * whenever the text changes - which for files this size is nothing, and is far
 * less code to get wrong than keeping two representations agreeing.
 *
 * Ctrl+S saves. Ctrl+Q is the window manager's and never arrives here.
 */

#include <clipboard.h>
#include <app.h>
#include <dialog.h>
#include <ui.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <widget.h>
#include <window.h>

#define TOOLBAR_H 30
#define STATUS_H  20
#define MAX_TEXT  (64 * 1024)
#define MAX_LINES 2048

static char g_text[MAX_TEXT];
static int  g_len;
static int  g_caret;            /* byte offset of the insertion point */
/* A selection is an ordered pair of offsets; anchor is where the drag began, so
 * dragging backwards works without a special case. */
static int  g_anchor = -1;
static int  g_hcol;             /* first visible column, for wide lines */
static int  g_dirty;
static int  g_scroll;           /* first visible line */
static char g_file[256];
static char g_status[128] = "";
/* Where the text is drawn, handed over by the layout. */
static struct ui_rect g_area;

/* Where each line begins. Recomputed after every edit; see the header comment. */
static int g_line_start[MAX_LINES];
static int g_lines;



static void relines(void)
{
    g_lines = 0;
    g_line_start[g_lines++] = 0;
    for (int i = 0; i < g_len && g_lines < MAX_LINES; ++i)
        if (g_text[i] == '\n')
            g_line_start[g_lines++] = i + 1;
}

static int line_of(int offset)
{
    int line = 0;
    for (int i = 1; i < g_lines; ++i) {
        if (g_line_start[i] > offset)
            break;
        line = i;
    }
    return line;
}

static int line_end(int line)
{
    if (line + 1 < g_lines)
        return g_line_start[line + 1] - 1;   /* before the newline */
    return g_len;
}

static void load(const char* path)
{
    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        /* A name that is not there yet is a new file, not an error - that is
         * how every editor lets you create one. */
        g_len = 0;
        snprintf(g_status, sizeof(g_status), "new file");
        relines();
        return;
    }
    g_len = (int)read(fd, g_text, MAX_TEXT - 1);
    if (g_len < 0)
        g_len = 0;
    close(fd);
    snprintf(g_status, sizeof(g_status), "%d bytes", g_len);
    relines();
}

static void save_to(const char* path)
{
    int k = 0;
    while (path[k] != '\0' && k < 255) { g_file[k] = path[k]; ++k; }
    g_file[k] = '\0';
    const int fd = open(g_file, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        snprintf(g_status, sizeof(g_status), "cannot write %s", g_file);
        return;
    }
    const int n = (int)write(fd, g_text, (unsigned long)g_len);
    close(fd);
    if (n == g_len) {
        g_dirty = 0;
        snprintf(g_status, sizeof(g_status), "saved %d bytes", n);
    } else {
        snprintf(g_status, sizeof(g_status), "short write (%d of %d)", n, g_len);
    }
}

/* Always ask. An editor that silently overwrites whatever it was handed is a
 * good way to lose a file you only meant to look at. */
static struct app g_app;
static void sync_labels(void);

/* Where the document goes, once the sheet has been answered. The sheet is a
 * window of its own, so the text underneath is untouched while it is up -
 * which the dialogue this replaces could not manage. */
static void saved(struct app* a, int result)
{
    if (result) {
        save_to(app_sheet_path(a));
        snprintf(g_file, sizeof(g_file), "%s", app_sheet_path(a));
    }
    sync_labels();
}

static void save(void)
{
    g_app.sheet_done = saved;
    app_sheet_save(&g_app, "/", g_file[0] == '/' ? g_file + 1 : g_file);
    snprintf(g_status, sizeof(g_status), "choose where to save");
}

static void insert(char c)
{
    if (g_len + 1 >= MAX_TEXT)
        return;
    for (int i = g_len; i > g_caret; --i)
        g_text[i] = g_text[i - 1];
    g_text[g_caret++] = c;
    ++g_len;
    g_dirty = 1;
    relines();
}

static void backspace(void)
{
    if (g_caret <= 0)
        return;
    for (int i = g_caret - 1; i < g_len - 1; ++i)
        g_text[i] = g_text[i + 1];
    --g_caret;
    --g_len;
    g_dirty = 1;
    relines();
}

static int text_rows(void)
{
    return g_area.h / WG_GLYPH_H;
}


/* The selection, low offset first, or 0 length when there is none. */
static void selection(int* from, int* to)
{
    if (g_anchor < 0) { *from = *to = g_caret; return; }
    *from = g_anchor < g_caret ? g_anchor : g_caret;
    *to   = g_anchor < g_caret ? g_caret : g_anchor;
}

/* The longest line, so the horizontal bar knows how far it can go. */

/* Where in the text a point in the window lands. */
static int offset_at(int x, int y)
{
    int line = g_scroll + (y - TOOLBAR_H - 4) / WG_GLYPH_H;
    if (line < 0) line = 0;
    if (line >= g_lines) line = g_lines - 1;
    int col = g_hcol + (x - 8) / WG_GLYPH_W;
    if (col < 0) col = 0;
    int at = g_line_start[line] + col;
    if (at > line_end(line)) at = line_end(line);
    return at;
}

static void delete_selection(void)
{
    int from, to;
    selection(&from, &to);
    if (from == to)
        return;
    for (int i = from; i < g_len - (to - from); ++i)
        g_text[i] = g_text[i + (to - from)];
    g_len -= (to - from);
    g_caret = from;
    g_anchor = -1;
    g_dirty = 1;
    relines();
}

static void copy_selection(int cut)
{
    int from, to;
    selection(&from, &to);
    if (from == to) {
        snprintf(g_status, sizeof(g_status), "nothing selected");
        return;
    }
    clip_put(&g_text[from], (unsigned)(to - from));
    snprintf(g_status, sizeof(g_status), "%s %d bytes",
             cut ? "cut" : "copied", to - from);
    if (cut)
        delete_selection();
}

static void insert(char c);

static void paste(void)
{
    static char buf[CLIP_MAX];
    const int n = clip_get(buf, sizeof(buf));
    if (n <= 0) {
        snprintf(g_status, sizeof(g_status), "the clipboard is empty");
        return;
    }
    delete_selection();
    for (int i = 0; i < n; ++i)
        insert(buf[i]);
    snprintf(g_status, sizeof(g_status), "pasted %d bytes", n);
}

/* Keep the caret on screen: an editor that types off the bottom is unusable. */
static void follow_caret(void)
{
    const int line = line_of(g_caret);
    if (line < g_scroll)
        g_scroll = line;
    else if (line >= g_scroll + text_rows())
        g_scroll = line - text_rows() + 1;
    if (g_scroll < 0)
        g_scroll = 0;
}

static void draw_text(struct ui_view* view, void* user)
{
    (void)user;
    g_area = view->frame;

    const int top = g_area.y;
    const int h = g_area.h;
    wg_fill(g_area.x, top, g_area.w, h, wg_body_colour());

    const int rows = text_rows();
    const int caret_line = line_of(g_caret);
    for (int r = 0; r < rows; ++r) {
        const int line = g_scroll + r;
        if (line >= g_lines)
            break;
        const int begin = g_line_start[line], end = line_end(line);
        const int y = top + 4 + r * WG_GLYPH_H;

        /* Highlight first, so the glyphs sit on top of it. */
        int from, to;
        selection(&from, &to);
        if (to > begin && from < end) {
            const int a = (from > begin ? from : begin) - begin - g_hcol;
            const int b = (to < end ? to : end) - begin - g_hcol;
            const int x0 = 8 + (a < 0 ? 0 : a) * WG_GLYPH_W;
            const int x1 = 8 + (b < 0 ? 0 : b) * WG_GLYPH_W;
            if (x1 > x0)
                wg_fill(x0, y, x1 - x0, WG_GLYPH_H, wg_sel_colour());
        }

        char buf[256];
        int n = 0;
        for (int i = begin + g_hcol; i < end && n < (int)sizeof(buf) - 1; ++i)
            buf[n++] = g_text[i] == '\t' ? ' ' : g_text[i];
        buf[n] = '\0';
        wg_text(8, y, buf, WG_INK);

        /* The caret, drawn as a bar between characters rather than over one, so
         * it is visible at the end of a line as well as inside it. */
        if (line == caret_line) {
            const int col = g_caret - begin - g_hcol;
            if (col >= 0)
                wg_fill(8 + col * WG_GLYPH_W, y, 1, WG_GLYPH_H, WG_ACCENT);
        }
    }

    /* The bars sit inside the sunken well, along its right and bottom edges. */
    wg_scrollbar_v(g_area.x + g_area.w - WG_SCROLL_W, top, h,
                   g_scroll, rows, g_lines);
}

static const char* const kMenu[] = { "Copy", "Cut", "Paste", "Select all" };

/* --- the interface ---------------------------------------------------------
 *
 * A toolbar of components and the text as a custom view. The text stays
 * hand-drawn: this editor has a selection, a horizontal scroll and a caret
 * that moves by line, and the library's text area has none of those. Putting
 * it on the component would have been a port that quietly removed features.
 */

static struct ui_view* g_title;
static struct ui_view* g_where;
static char g_title_text[300];
static char g_where_text[160];

static void sync_labels(void)
{
    snprintf(g_title_text, sizeof(g_title_text), "%s%s", g_file,
             g_dirty ? " *" : "");
    ui_set_text(g_title, g_title_text);
    snprintf(g_where_text, sizeof(g_where_text), "line %d of %d   %s",
             line_of(g_caret) + 1, g_lines, g_status);
    ui_set_text(g_where, g_where_text);
}

static void on_save(struct ui_view* v, void* u)
{
    (void)v; (void)u;
    save();
    sync_labels();
}

static int on_menu(struct app* a, int pick)
{
    (void)a;
    if (pick == 0)      copy_selection(0);
    else if (pick == 1) copy_selection(1);
    else if (pick == 2) paste();
    else if (pick == 3) { g_anchor = 0; g_caret = g_len; }
    sync_labels();
    return 1;
}

static int on_event(struct app* a, const struct win_event* event)
{
    (void)a;
    if (event->type == WIN_EVENT_MOUSE_DOWN) {
        if (event->y < g_area.y || event->y >= g_area.y + g_area.h)
            return 0;                   /* the toolbar's, not the text's */
        if (event->x >= g_area.x + g_area.w - WG_SCROLL_W) {
            g_scroll = wg_scroll_hit_v(event->x, event->y,
                g_area.x + g_area.w - WG_SCROLL_W, g_area.y, g_area.h,
                g_scroll, text_rows(), g_lines);
            return 1;
        }
        /* A click starts a selection; the drag extends it. */
        g_caret = offset_at(event->x, event->y);
        g_anchor = g_caret;
        sync_labels();
        return 1;
    }
    if (event->type == WIN_EVENT_MOUSE_UP) {
        /* A click that selected nothing means nothing is selected. */
        if (g_anchor == g_caret)
            g_anchor = -1;
        return 1;
    }
    if (event->type == WIN_EVENT_MOUSE_MOVE) {
        if (g_anchor >= 0 && event->y >= g_area.y &&
            event->y < g_area.y + g_area.h) {
            g_caret = offset_at(event->x, event->y);
            sync_labels();
            return 1;
        }
        return 0;
    }
    if (event->type == WIN_EVENT_KEY) {
                const char c = (char)event->key;
                if (c == 19) {                  /* ctrl+s */
                    save();
                } else if (c == WIN_KEY_UP || c == WIN_KEY_DOWN ||
                           c == WIN_KEY_LEFT || c == WIN_KEY_RIGHT) {
                    /* Arrows move the caret and drop any selection, which is
                     * what an unshifted arrow means everywhere. */
                    const int line = line_of(g_caret);
                    if (c == WIN_KEY_LEFT && g_caret > 0) --g_caret;
                    else if (c == WIN_KEY_RIGHT && g_caret < g_len) ++g_caret;
                    else if (c == WIN_KEY_UP && line > 0) {
                        const int col = g_caret - g_line_start[line];
                        const int end = line_end(line - 1);
                        g_caret = g_line_start[line - 1] + col;
                        if (g_caret > end) g_caret = end;
                    } else if (c == WIN_KEY_DOWN && line + 1 < g_lines) {
                        const int col = g_caret - g_line_start[line];
                        const int end = line_end(line + 1);
                        g_caret = g_line_start[line + 1] + col;
                        if (g_caret > end) g_caret = end;
                    }
                    g_anchor = -1;
                    follow_caret();
                } else if (c == 1) {            /* ctrl+a */
                    g_anchor = 0;
                    g_caret = g_len;
                    snprintf(g_status, sizeof(g_status), "selected all");
                } else if (c == 3) {            /* ctrl+c */
                    copy_selection(0);
                } else if (c == 24) {           /* ctrl+x */
                    copy_selection(1);
                } else if (c == 22) {           /* ctrl+v */
                    paste();
                    follow_caret();
                } else if (c == '\b' || c == 0x7F) {
                    if (g_anchor >= 0) { delete_selection(); follow_caret(); }
                    else {
                    backspace();
                    follow_caret();
                    }
                } else if (c == '\n' || c == '\r') {
                    insert('\n');
                    follow_caret();
                } else if ((unsigned char)c >= 32) {
                    /* Typing over a selection replaces it, as it should. */
                    if (g_anchor >= 0 && g_anchor != g_caret)
                        delete_selection();
                    insert(c);
                    g_anchor = -1;
                    follow_caret();
                }
        sync_labels();
        return 1;
    }
    return 0;
}

int main(int argc, char** argv)
{
    if (argc > 1 && argv[1][0] != '\0' && argv[1][0] != '-')
        load(argv[1]);
    else
        relines();

    struct ui_view* root = ui_box(0, UI_STACK_V, 6, 4);

    struct ui_view* bar = ui_box(root, UI_STACK_H, 0, 6);
    ui_size(bar, 0, 24);
    ui_grow(bar, 0);
    ui_grow(ui_button(bar, "Save", on_save, 0), 0);
    g_title = ui_label(bar, "");
    ui_grow(g_title, 1);

    ui_custom(root, draw_text, 0);
    g_where = ui_label(root, "");
    ui_grow(g_where, 0);
    sync_labels();

    g_app.title = "Edit";
    g_app.width = 520; g_app.height = 400;
    g_app.min_width = 380; g_app.min_height = 260;
    g_app.event = on_event;
    g_app.menu = kMenu;
    g_app.menu_count = 4;
    g_app.menu_pick = on_menu;
    g_app.root = root;
    return app_run(&g_app, argc, argv);
}
