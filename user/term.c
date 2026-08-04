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
#include <window.h>

#define GLYPH_W 8
#define GLYPH_H 16

#define MAX_COLS 256
#define MAX_ROWS 128

#define BG      0x000000
#define FG      0xC0C0C0
#define CURSOR  0x00A000

static uint32_t* g_px;
static unsigned  g_win_w, g_win_h;
static unsigned char g_font[256 * 16];

/* The character grid, and the cursor within it. Written by both threads - the
 * reader as output arrives, the main thread as keys are echoed - so everything
 * that touches it holds the lock. */
static mutex_t g_lock = MUTEX_INIT;
static char g_cells[MAX_ROWS][MAX_COLS];
static int  g_cols = 80, g_rows = 24;
static int  g_cur_r, g_cur_c;
static volatile int g_dirty = 1;
static volatile int g_shell_done;

static int g_from_shell;        /* read end of the shell's output */
static int g_to_shell;          /* write end of the shell's input */

/* --- the grid ------------------------------------------------------------ */

static void clear_row(int row)
{
    for (int c = 0; c < MAX_COLS; ++c)
        g_cells[row][c] = ' ';
}

static void term_clear(void)
{
    for (int r = 0; r < MAX_ROWS; ++r)
        clear_row(r);
    g_cur_r = 0;
    g_cur_c = 0;
}

static void scroll_up(void)
{
    for (int r = 0; r + 1 < g_rows; ++r)
        memcpy(g_cells[r], g_cells[r + 1], MAX_COLS);
    clear_row(g_rows - 1);
    g_cur_r = g_rows - 1;
}

static void newline(void)
{
    g_cur_c = 0;
    if (++g_cur_r >= g_rows)
        scroll_up();
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
        g_cells[g_cur_r][g_cur_c] = ' ';
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

    g_cells[g_cur_r][g_cur_c] = ch;
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

static void repaint(int id)
{
    mutex_lock(&g_lock);
    for (int r = 0; r < g_rows; ++r)
        for (int c = 0; c < g_cols; ++c)
            draw_glyph(c, r, g_cells[r][c], FG, BG);
    /* A block cursor, drawn last so it sits over whatever is under it. */
    draw_glyph(g_cur_c, g_cur_r, g_cells[g_cur_r][g_cur_c], BG, CURSOR);
    mutex_unlock(&g_lock);
    win_present(id);
}

/* Recompute the grid from the window size. Anything that no longer fits is
 * dropped rather than reflowed: rewrapping needs to know where the shell's
 * lines actually ended, and a pipe does not carry that. */
static void resize_grid(void)
{
    int cols = (int)(g_win_w / GLYPH_W);
    int rows = (int)(g_win_h / GLYPH_H);
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (cols > MAX_COLS) cols = MAX_COLS;
    if (rows > MAX_ROWS) rows = MAX_ROWS;

    mutex_lock(&g_lock);
    g_cols = cols;
    g_rows = rows;
    if (g_cur_c >= g_cols) g_cur_c = g_cols - 1;
    while (g_cur_r >= g_rows) {
        /* Keep the bottom of the output, which is where the prompt is. */
        for (int r = 0; r + 1 < MAX_ROWS; ++r)
            memcpy(g_cells[r], g_cells[r + 1], MAX_COLS);
        clear_row(MAX_ROWS - 1);
        --g_cur_r;
    }
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

    g_win_w = 80 * GLYPH_W;
    g_win_h = 24 * GLYPH_H;
    const int id = win_create(x, y, g_win_w, g_win_h, "Terminal");
    if (id < 0) {
        printf("term: no window server\n");
        return 1;
    }
    g_px = win_map(id);
    if (g_px == 0)
        return 1;
    /* Below this the shell's prompt has nowhere to go. */
    win_set_min_size(id, 20 * GLYPH_W, 4 * GLYPH_H);

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
            if (event.type != WIN_EVENT_KEY)
                continue;

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
