/* term - a shell in a window.
 *
 * Until this existed the desktop and the console took turns: there is one
 * framebuffer and no virtual consoles, so starting the window server meant
 * giving up the shell until every window was closed. A terminal removes that
 * trade by putting the shell somewhere the desktop can draw it.
 *
 * The shape is the ordinary one. Two pipes stand in for a terminal device - the
 * shell's stdin on one, its stdout and stderr on the other - and this process
 * sits between them and the window, turning key events into bytes the shell can
 * read and bytes the shell wrote into glyphs.
 *
 * The one thing that needs care is that a pipe read blocks. Polling the window
 * and waiting for shell output are both things that have to happen, and neither
 * can be allowed to starve the other, so the read lives on a **thread** of its
 * own: it blocks as long as it likes while the main thread keeps the window
 * responsive. They share the character grid, under a mutex.
 *
 * There is no line discipline in the kernel for a pipe, so this does the
 * editing: characters are echoed here and a line is only handed to the shell
 * when Enter is pressed. That is also why backspace works - nothing else in the
 * path knows what a backspace means.
 */

#include <display.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread.h>
#include <unistd.h>
#include <widget.h>
#include <window.h>

#define GLYPH_W 8
#define GLYPH_H 16

#define MAX_COLS 256
#define MAX_ROWS 128

/* How far back the terminal remembers. The window shows a few dozen lines; a
 * build or a test run says hundreds, and the ones that matter have already
 * gone past by the time anybody looks. */
#define HISTORY_ROWS 1024

#define BG      0x000000
#define FG      0xC0C0C0
#define CURSOR  0x00A000

static uint32_t* g_px;
static unsigned  g_win_w, g_win_h;
static unsigned char g_font[256 * 16];

/* The character grid, and the cursor within it. Written by both threads - the
 * reader as output arrives, the main thread as keys are echoed - so everything
 * that touches it holds the lock.
 *
 * Lines are numbered from the first one the terminal ever wrote and never
 * renumbered, which is what makes a scroll position mean something: the view
 * remembers an absolute line, so output arriving underneath it does not drag
 * it along. The ring is where those numbers actually live - line `a` is at
 * `a % HISTORY_ROWS`, valid while it is still at or after g_first.
 */
static mutex_t g_lock = MUTEX_INIT;
static char g_cells[HISTORY_ROWS][MAX_COLS];
static int  g_cols = 80, g_rows = 24;
static long g_cur_line;         /* the line the cursor is on */
static long g_first;            /* the oldest line still remembered */
static long g_view;             /* the top line of the window */
static int  g_follow = 1;       /* is the view pinned to the bottom? */
static int  g_cur_c;
static volatile int g_dirty = 1;
static volatile int g_shell_done;

static int g_from_shell;        /* read end of the shell's output */
static int g_to_shell;          /* write end of the shell's input */

/* --- the grid ------------------------------------------------------------ */

static char* row_of(long line)
{
    long slot = line % HISTORY_ROWS;
    if (slot < 0)
        slot += HISTORY_ROWS;
    return g_cells[slot];
}

static void clear_line(long line)
{
    char* row = row_of(line);
    for (int c = 0; c < MAX_COLS; ++c)
        row[c] = ' ';
}

/* How many lines there are to look at, and the furthest down the view can go.
 * Both with the lock held. */
static long line_count(void) { return g_cur_line - g_first + 1; }

static long last_view(void)
{
    const long bottom = g_cur_line - g_rows + 1;
    return bottom < g_first ? g_first : bottom;
}

static void term_clear(void)
{
    for (long r = 0; r < HISTORY_ROWS; ++r)
        clear_line(r);
    g_cur_line = 0;
    g_first    = 0;
    g_view     = 0;
    g_follow   = 1;
    g_cur_c    = 0;
}

static void newline(void)
{
    g_cur_c = 0;
    clear_line(++g_cur_line);
    /* The oldest line falls off the back once the ring is full - which is the
     * only place history is ever lost, and the reason HISTORY_ROWS is large. */
    if (line_count() > HISTORY_ROWS)
        g_first = g_cur_line - HISTORY_ROWS + 1;
    if (g_follow)
        g_view = last_view();
    else if (g_view < g_first)
        g_view = g_first;       /* what it was looking at has gone */
}

/* Call with the lock held. */
static void term_putc(char ch)
{
    if (ch == '\n') {
        newline();
        return;
    }
    if (ch == '\r') {
        g_cur_c = 0;
        return;
    }
    if (ch == '\b' || ch == 0x7F) {
        if (g_cur_c > 0)
            --g_cur_c;
        row_of(g_cur_line)[g_cur_c] = ' ';
        return;
    }
    if (ch == '\t') {
        do {
            term_putc(' ');
        } while ((g_cur_c % 8) != 0);
        return;
    }
    if ((unsigned char)ch < 32)
        return;                         /* nothing else is understood */

    row_of(g_cur_line)[g_cur_c] = ch;
    if (++g_cur_c >= g_cols)
        newline();
}

/* --- drawing ------------------------------------------------------------- */

static void draw_glyph(int col, int row, char ch, uint32_t fg, uint32_t bg)
{
    const int x0 = col * GLYPH_W, y0 = row * GLYPH_H;
    if (x0 + GLYPH_W > (int)g_win_w || y0 + GLYPH_H > (int)g_win_h)
        return;
    const unsigned char* glyph = &g_font[(unsigned char)ch * 16];
    for (int r = 0; r < GLYPH_H; ++r) {
        uint32_t* out = &g_px[(unsigned long)(y0 + r) * g_win_w + x0];
        const unsigned char bits = glyph[r];
        for (int c = 0; c < GLYPH_W; ++c)
            out[c] = (bits & (0x80 >> c)) ? fg : bg;
    }
}

/* The text area stops short of the scrollbar. */
static int text_width(void)
{
    const int w = (int)g_win_w - WG_SCROLL_W;
    return w < GLYPH_W ? GLYPH_W : w;
}

static void repaint(int id)
{
    mutex_lock(&g_lock);
    for (int r = 0; r < g_rows; ++r) {
        const char* row = row_of(g_view + r);
        for (int c = 0; c < g_cols; ++c)
            draw_glyph(c, r, row[c], FG, BG);
    }

    /* A block cursor, drawn last so it sits over whatever is under it - and
     * only when the line it is on is one of the ones being shown, because
     * scrolled back is exactly the case where it is not. */
    const long cur_row = g_cur_line - g_view;
    if (cur_row >= 0 && cur_row < g_rows)
        draw_glyph(g_cur_c, (int)cur_row, row_of(g_cur_line)[g_cur_c],
                   BG, CURSOR);

    /* Whatever the glyphs did not cover: the strip left of the bar when the
     * width is not a whole number of characters, and the one below the last
     * row. Without these the window keeps whatever was there before. */
    const int used_w = g_cols * GLYPH_W, used_h = g_rows * GLYPH_H;
    wg_target(g_px, g_win_w, g_win_h);
    if (used_w < text_width())
        wg_fill(used_w, 0, text_width() - used_w, (int)g_win_h, BG);
    if (used_h < (int)g_win_h)
        wg_fill(0, used_h, text_width(), (int)g_win_h - used_h, BG);

    wg_scrollbar_v((int)g_win_w - WG_SCROLL_W, 0, (int)g_win_h,
                   (int)(g_view - g_first), g_rows, (int)line_count());
    mutex_unlock(&g_lock);
    win_present(id);
}

/* Put the view back at the bottom, which is where typing belongs: a key
 * pressed while scrolled back goes to the shell either way, and watching the
 * echo appear somewhere off-screen is not useful. Lock held. */
static void follow_bottom(void)
{
    g_follow = 1;
    g_view = last_view();
}

/* Recompute the grid from the window size. Anything that no longer fits is
 * dropped rather than reflowed: rewrapping needs to know where the shell's
 * lines actually ended, and a pipe does not carry that. */
static void resize_grid(void)
{
    int cols = text_width() / GLYPH_W;
    int rows = (int)(g_win_h / GLYPH_H);
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (cols > MAX_COLS) cols = MAX_COLS;
    if (rows > MAX_ROWS) rows = MAX_ROWS;

    mutex_lock(&g_lock);
    g_cols = cols;
    g_rows = rows;
    if (g_cur_c >= g_cols) g_cur_c = g_cols - 1;
    /* Nothing has to be thrown away to make the window smaller any more - the
     * lines are all still there, and the view just covers fewer of them. */
    if (g_follow || g_view > last_view())
        g_view = last_view();
    if (g_view < g_first)
        g_view = g_first;
    mutex_unlock(&g_lock);
    g_dirty = 1;
}

/* --- the shell ----------------------------------------------------------- */

/* Blocks on the shell's output for as long as it likes. That is the whole
 * reason it is a thread: the main loop must stay free to service the window. */
static void reader_thread(void* arg)
{
    (void)arg;
    char buffer[256];
    for (;;) {
        const int n = (int)read(g_from_shell, buffer, sizeof(buffer));
        if (n <= 0)
            break;                      /* the shell closed its end, or exited */
        mutex_lock(&g_lock);
        for (int i = 0; i < n; ++i)
            term_putc(buffer[i]);
        mutex_unlock(&g_lock);
        g_dirty = 1;
    }
    g_shell_done = 1;
    g_dirty = 1;
}

static int start_shell(void)
{
    int to_shell[2], from_shell[2];
    if (pipe(to_shell) < 0)
        return -1;
    if (pipe(from_shell) < 0) {
        close(to_shell[0]);
        close(to_shell[1]);
        return -1;
    }

    const int pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        /* The pipes become the shell's console. It reads lines this process
         * assembled and writes output this process renders; it has no idea it
         * is not talking to a terminal. */
        dup2(to_shell[0], 0);
        dup2(from_shell[1], 1);
        dup2(from_shell[1], 2);

        /* A second descriptor onto the same input, marked as the controlling
         * terminal. Standard input is not enough: the shell redirects it for
         * every pipeline, and `something | less` is exactly the case where a
         * program needs the keyboard while its standard input is a pipe from
         * another program. This one is not redirected, is inherited by
         * everything the shell starts, and is what /dev/tty opens. */
        tty_set(dup(0));

        /* What this terminal can do, for anything that asks. Modest and
         * honest: no colour, no cursor addressing, 80 by 24. A program that
         * believes TERM and sends escape codes would print them literally -
         * `clear` already does. */
        setenv("TERM", "leah", 1);
        setenv("COLUMNS", "80", 1);
        setenv("LINES", "24", 1);

        close(to_shell[0]);
        close(to_shell[1]);
        close(from_shell[0]);
        close(from_shell[1]);
        char* argv[] = { "sh", 0 };
        execve("/bin/sh", argv, 0);
        exit(127);
    }

    /* The parent keeps only the ends it uses. Closing the others matters: the
     * reader sees end-of-file when the last writer goes, and if this process
     * held the write end open it would never see the shell exit. */
    close(to_shell[0]);
    close(from_shell[1]);
    g_to_shell = to_shell[1];
    g_from_shell = from_shell[0];
    return pid;
}

int main(int argc, char** argv)
{
    const int x = argc > 1 ? atoi_simple(argv[1]) : 60;
    const int y = argc > 2 ? atoi_simple(argv[2]) : 60;

    if (fb_font(g_font) != 0) {
        printf("term: cannot read the console font\n");
        return 1;
    }

    /* Eighty columns of text plus the bar, so the terminal is still the
     * eighty columns everything assumes and the scrollbar is not taken out of
     * them. */
    g_win_w = 80 * GLYPH_W + WG_SCROLL_W;
    g_win_h = 24 * GLYPH_H;
    const int id = win_create(x, y, g_win_w, g_win_h, "Terminal");
    if (id < 0) {
        printf("term: no window server\n");
        return 1;
    }
    g_px = win_map(id);
    if (g_px == 0)
        return 1;
    wg_theme();                 /* the bar is drawn in the desktop's colours */
    /* Below this the shell's prompt has nowhere to go. */
    win_set_min_size(id, 20 * GLYPH_W + WG_SCROLL_W, 4 * GLYPH_H);

    term_clear();
    resize_grid();

    if (start_shell() < 0) {
        printf("term: cannot start a shell\n");
        win_destroy(id);
        return 1;
    }
    thread_create(reader_thread, 0);

    /* Assembled here rather than in the shell, because a pipe has no line
     * discipline: nothing between the keyboard and the shell would otherwise
     * know that backspace means anything. */
    char line[512];
    unsigned len = 0;
    int dragging = 0;           /* is the scrollbar's thumb being held? */

    for (;;) {
        struct win_event event;
        while (win_poll(id, &event)) {
            if (event.type == WIN_EVENT_RESIZE) {
                g_win_w = (unsigned)event.x;
                g_win_h = (unsigned)event.y;
                g_px = win_map(id);
                if (g_px == 0)
                    return 1;
                resize_grid();
                continue;
            }
            if (event.type == WIN_EVENT_CLOSE)
                goto done;

            /* The scrollbar. The position is this program's, as the widget
             * expects: the bar is told where the view is and asked where a
             * click wants it, and nothing about the history belongs to it. */
            if (event.type == WIN_EVENT_MOUSE_DOWN ||
                event.type == WIN_EVENT_MOUSE_MOVE ||
                event.type == WIN_EVENT_MOUSE_UP) {
                const int bar_x = (int)g_win_w - WG_SCROLL_W;

                if (event.type == WIN_EVENT_MOUSE_UP) {
                    dragging = 0;
                    continue;
                }
                if (event.type == WIN_EVENT_MOUSE_MOVE) {
                    if (!dragging)
                        continue;
                    mutex_lock(&g_lock);
                    g_view = g_first + wg_scroll_drag_v(event.y, 0,
                                                        (int)g_win_h, g_rows,
                                                        (int)line_count());
                    if (g_view < g_first)      g_view = g_first;
                    if (g_view > last_view())  g_view = last_view();
                    g_follow = g_view == last_view();
                    mutex_unlock(&g_lock);
                    g_dirty = 1;
                    continue;
                }
                if (event.x < bar_x)
                    continue;               /* a click in the text, not the bar */

                mutex_lock(&g_lock);
                const int first = (int)(g_view - g_first);
                const int span  = (int)line_count();
                if (wg_scroll_on_thumb_v(event.y, 0, (int)g_win_h,
                                         first, g_rows, span)) {
                    dragging = 1;
                } else {
                    g_view = g_first + wg_scroll_hit_v(event.x, event.y, bar_x,
                                                       0, (int)g_win_h,
                                                       first, g_rows, span);
                    if (g_view < g_first)     g_view = g_first;
                    if (g_view > last_view()) g_view = last_view();
                    g_follow = g_view == last_view();
                }
                mutex_unlock(&g_lock);
                g_dirty = 1;
                continue;
            }

            if (event.type != WIN_EVENT_KEY)
                continue;

            /* Typing goes to the bottom, wherever the view happened to be. */
            mutex_lock(&g_lock);
            follow_bottom();
            mutex_unlock(&g_lock);

            const char ch = (char)event.key;
            if (ch == '\n' || ch == '\r') {
                mutex_lock(&g_lock);
                term_putc('\n');
                mutex_unlock(&g_lock);
                line[len] = '\n';
                write(g_to_shell, line, len + 1);
                len = 0;
                g_dirty = 1;
            } else if (ch == '\b' || ch == 0x7F) {
                if (len > 0) {
                    --len;
                    mutex_lock(&g_lock);
                    term_putc('\b');
                    mutex_unlock(&g_lock);
                    g_dirty = 1;
                }
            } else if ((unsigned char)ch >= 32 && len + 1 < sizeof(line)) {
                line[len++] = ch;
                mutex_lock(&g_lock);
                term_putc(ch);
                mutex_unlock(&g_lock);
                g_dirty = 1;
            }
        }

        if (g_dirty) {
            g_dirty = 0;
            repaint(id);
        }

        /* The shell has gone; so should the window. */
        if (g_shell_done)
            break;

        msleep(15);
    }

done:
    /* Closing this end is what tells the shell to stop, if it has not already:
     * its next read returns end-of-file. */
    close(g_to_shell);
    win_destroy(id);
    return 0;
}
