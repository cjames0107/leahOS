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

static void save(void)
{
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
    return ((int)g_h - TOOLBAR_H - STATUS_H - 8) / WG_GLYPH_H;
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

        char buf[256];
        int n = 0;
        for (int i = begin; i < end && n < (int)sizeof(buf) - 1; ++i)
            buf[n++] = g_text[i] == '\t' ? ' ' : g_text[i];
        buf[n] = '\0';
        wg_text(8, y, buf, WG_INK);

        /* The caret, drawn as a bar between characters rather than over one, so
         * it is visible at the end of a line as well as inside it. */
        if (line == caret_line) {
            const int col = g_caret - begin;
            wg_fill(8 + col * WG_GLYPH_W, y, 1, WG_GLYPH_H, WG_ACCENT);
        }
    }

    wg_fill(0, (int)g_h - STATUS_H, (int)g_w, STATUS_H, WG_FACE);
    char line[160];
    snprintf(line, sizeof(line), "line %d of %d   ctrl+s saves",
             caret_line + 1, g_lines);
    wg_text_clipped(8, (int)g_h - STATUS_H + 2, line, WG_INK, 260);
    wg_text_clipped(280, (int)g_h - STATUS_H + 2, g_status, WG_DIM,
                    (int)g_w - 290);
}

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
                } else if (event.y >= TOOLBAR_H &&
                           event.y < (int)g_h - STATUS_H) {
                    /* Put the caret where it was clicked, clamped to the end of
                     * that line so a click past the text lands sensibly. */
                    const int line = g_scroll + (event.y - TOOLBAR_H - 4) / WG_GLYPH_H;
                    if (line >= 0 && line < g_lines) {
                        const int col = (event.x - 8) / WG_GLYPH_W;
                        int at = g_line_start[line] + (col < 0 ? 0 : col);
                        if (at > line_end(line))
                            at = line_end(line);
                        g_caret = at;
                    }
                }
            } else if (event.type == WIN_EVENT_KEY) {
                const char c = (char)event.key;
                if (c == 19) {                  /* ctrl+s */
                    save();
                } else if (c == '\b' || c == 0x7F) {
                    backspace();
                    follow_caret();
                } else if (c == '\n' || c == '\r') {
                    insert('\n');
                    follow_caret();
                } else if ((unsigned char)c >= 32) {
                    insert(c);
                    follow_caret();
                }
            } else {
                continue;
            }
            draw();
            win_present(id);
        }
        msleep(15);
    }
}
