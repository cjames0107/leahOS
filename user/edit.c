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
#include <dialog.h>
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

static uint32_t* g_px;
static unsigned  g_w = 520, g_h = 400;

static char g_text[MAX_TEXT];
static int  g_len;
static int  g_caret;            /* byte offset of the insertion point */
/* A selection is an ordered pair of offsets; anchor is where the drag began, so
 * dragging backwards works without a special case. */
static int  g_anchor = -1;
static int  g_hcol;             /* first visible column, for wide lines */
static int  g_bar_v, g_bar_h;   /* which thumb, if either, is being dragged */
static int  g_dirty;
static int  g_scroll;           /* first visible line */
static char g_file[256];
static char g_status[128] = "";

/* Where each line begins. Recomputed after every edit; see the header comment. */
static int g_line_start[MAX_LINES];
static int g_lines;

struct box { int x, y, w, h; };
static struct box g_save = { 8,   5, 56, 20 };
static struct box g_up   = { 72,  5, 24, 20 };
static struct box g_down = { 100, 5, 24, 20 };

static int inside(const struct box* b, int x, int y)
{
    return x >= b->x && y >= b->y && x < b->x + b->w && y < b->y + b->h;
}

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
static void save(void)
{
    dlg_save("/", g_file[0] == '/' ? g_file + 1 : g_file);
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
    return ((int)g_h - TOOLBAR_H - STATUS_H - 8 - WG_SCROLL_W) / WG_GLYPH_H;
}

static int text_cols(void)
{
    return ((int)g_w - 16 - WG_SCROLL_W) / WG_GLYPH_W;
}

/* The selection, low offset first, or 0 length when there is none. */
static void selection(int* from, int* to)
{
    if (g_anchor < 0) { *from = *to = g_caret; return; }
    *from = g_anchor < g_caret ? g_anchor : g_caret;
    *to   = g_anchor < g_caret ? g_caret : g_anchor;
}

/* The longest line, so the horizontal bar knows how far it can go. */
static int widest(void)
{
    int w = 1;
    for (int i = 0; i < g_lines; ++i) {
        const int n = line_end(i) - g_line_start[i];
        if (n > w) w = n;
    }
    return w;
}

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

static void draw(void)
{
    wg_fill(0, 0, (int)g_w, (int)g_h, WG_FACE);
    wg_button(g_save.x, g_save.y, g_save.w, g_save.h, "Save", 0);
    wg_button(g_up.x, g_up.y, g_up.w, g_up.h, "^", 0);
    wg_button(g_down.x, g_down.y, g_down.w, g_down.h, "v", 0);

    char title[300];
    snprintf(title, sizeof(title), "%s%s", g_file, g_dirty ? " *" : "");
    wg_text_clipped(136, 12 - WG_GLYPH_H / 2 + 5, title, WG_INK, (int)g_w - 146);

    const int top = TOOLBAR_H;
    const int h = (int)g_h - TOOLBAR_H - STATUS_H;
    wg_fill(4, top, (int)g_w - 8, h, WG_PAPER);
    wg_bevel(4, top, (int)g_w - 8, h, 0);

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
                wg_fill(x0, y, x1 - x0, WG_GLYPH_H, 0xB0C4DE);
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
    wg_scrollbar_v((int)g_w - 4 - WG_SCROLL_W, top, h - WG_SCROLL_W,
                   g_scroll, rows, g_lines);
    wg_scrollbar_h(4, top + h - WG_SCROLL_W, (int)g_w - 8 - WG_SCROLL_W,
                   g_hcol, text_cols(), widest());

    wg_fill(0, (int)g_h - STATUS_H, (int)g_w, STATUS_H, WG_FACE);
    char line[160];
    snprintf(line, sizeof(line), "line %d of %d   ctrl+a c v s",
             caret_line + 1, g_lines);
    wg_text_clipped(8, (int)g_h - STATUS_H + 2, line, WG_INK, 260);
    wg_text_clipped(280, (int)g_h - STATUS_H + 2, g_status, WG_DIM,
                    (int)g_w - 290);
}

static const char* const kMenu[] = { "Copy", "Cut", "Paste", "Select all" };

int main(int argc, char** argv)
{
    if (argc > 1) {
        int n = 0;
        while (argv[1][n] != '\0' && n < 255) {
            g_file[n] = argv[1][n];
            ++n;
        }
        g_file[n] = '\0';
    } else {
        snprintf(g_file, sizeof(g_file), "/UNTITLED.TXT");
    }

    if (wg_font() != 0) {
        printf("edit: cannot read the console font\n");
        return 1;
    }
    const int id = win_create(150, 120, g_w, g_h, "Edit");
    if (id < 0) {
        printf("edit: no window server\n");
        return 1;
    }
    g_px = win_map(id);
    if (g_px == 0)
        return 1;
    win_set_min_size(id, 320, 200);
    wg_target(g_px, g_w, g_h);

    load(g_file);
    draw();
    win_present(id);

    for (;;) {
        struct win_event event;
        while (win_poll(id, &event)) {
            if (event.type == WIN_EVENT_CLOSE) {
                win_destroy(id);
                return 0;
            }
            if (menu_active() && event.type != WIN_EVENT_RESIZE) {
                switch (menu_event(&event)) {
                case 0: copy_selection(0); break;
                case 1: copy_selection(1); break;
                case 2: paste(); follow_caret(); break;
                case 3: g_anchor = 0; g_caret = g_len; break;
                }
                draw();
                menu_draw();
                win_present(id);
                continue;
            }
            if (dlg_active() && event.type != WIN_EVENT_RESIZE) {
                if (dlg_event(&event) == DLG_ACCEPT)
                    save_to(dlg_path());
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
                follow_caret();
            } else if (event.type == WIN_EVENT_MOUSE_DOWN) {
                if (inside(&g_save, event.x, event.y)) {
                    save();
                } else if (inside(&g_up, event.x, event.y)) {
                    if (g_scroll > 0) --g_scroll;
                } else if (inside(&g_down, event.x, event.y)) {
                    if (g_scroll + 1 < g_lines) ++g_scroll;
                } else if (event.x >= (int)g_w - 4 - WG_SCROLL_W &&
                           event.y >= TOOLBAR_H) {
                    const int bh = (int)g_h - TOOLBAR_H - STATUS_H - WG_SCROLL_W;
                    if (wg_scroll_on_thumb_v(event.y, TOOLBAR_H, bh, g_scroll,
                                             text_rows(), g_lines))
                        g_bar_v = 1;
                    else
                        g_scroll = wg_scroll_hit_v(event.x, event.y,
                            (int)g_w - 4 - WG_SCROLL_W, TOOLBAR_H, bh,
                            g_scroll, text_rows(), g_lines);
                } else if (event.y >= (int)g_h - STATUS_H - WG_SCROLL_W &&
                           event.y < (int)g_h - STATUS_H) {
                    const int bw = (int)g_w - 8 - WG_SCROLL_W;
                    if (wg_scroll_on_thumb_h(event.x, 4, bw, g_hcol,
                                             text_cols(), widest()))
                        g_bar_h = 1;
                    else
                        g_hcol = wg_scroll_hit_h(event.x, event.y, 4,
                            (int)g_h - STATUS_H - WG_SCROLL_W, bw,
                            g_hcol, text_cols(), widest());
                } else if (event.y >= TOOLBAR_H &&
                           event.y < (int)g_h - STATUS_H) {
                    if (event.button == 2) {
                        menu_open(event.x, event.y, kMenu, 4);
                    } else {
                        /* A click starts a selection; the drag extends it. */
                        g_caret = offset_at(event.x, event.y);
                        g_anchor = g_caret;
                    }
                }
            } else if (event.type == WIN_EVENT_MOUSE_UP) {
                g_bar_v = g_bar_h = 0;
                /* A click that selected nothing means nothing is selected. */
                if (g_anchor == g_caret)
                    g_anchor = -1;
            } else if (event.type == WIN_EVENT_MOUSE_MOVE && g_bar_v) {
                g_scroll = wg_scroll_drag_v(event.y, TOOLBAR_H,
                    (int)g_h - TOOLBAR_H - STATUS_H - WG_SCROLL_W,
                    text_rows(), g_lines);
            } else if (event.type == WIN_EVENT_MOUSE_MOVE && g_bar_h) {
                g_hcol = wg_scroll_drag_h(event.x, 4,
                    (int)g_w - 8 - WG_SCROLL_W, text_cols(), widest());
            } else if (event.type == WIN_EVENT_MOUSE_MOVE) {
                if (g_anchor >= 0 && event.y >= TOOLBAR_H &&
                    event.y < (int)g_h - STATUS_H - WG_SCROLL_W)
                    g_caret = offset_at(event.x, event.y);
            } else if (event.type == WIN_EVENT_KEY) {
                const char c = (char)event.key;
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
            } else {
                continue;
            }
            draw();
            dlg_draw((int)g_w, (int)g_h);
            menu_draw();
            win_present(id);
        }
        msleep(15);
    }
}
