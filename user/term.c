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
#include <signal.h>
#include <termios.h>
#include <unistd.h>
#include <app.h>
#include <ui.h>
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

/* Smoked rather than solid when the glass is on: the terminal keeps its dark
 * surface and its colours mean what they meant, but the blur behind it shows
 * through instead of the window being an opaque tile laid on the desktop. */
#define BG_SOLID 0xFF000000u
static uint32_t term_bg(void)
{
    return wg_glass_on() ? 0xA8000000u : BG_SOLID;
}
#define BG      term_bg()
#define FG      0xC0C0C0
#define CURSOR  0xFF00A000

/* The sixteen colours every terminal has had since the VT100's descendants,
 * in the shades the PC text mode used - which is what anything sending these
 * codes was written against. */
static const uint32_t kPalette[16] = {
    0x000000, 0xAA0000, 0x00AA00, 0xAA5500,
    0x0000AA, 0xAA00AA, 0x00AAAA, 0xAAAAAA,
    0x555555, 0xFF5555, 0x55FF55, 0xFFFF55,
    0x5555FF, 0xFF55FF, 0x55FFFF, 0xFFFFFF,
};

/* An attribute is two palette indices and a bright bit, in one byte, because
 * there is one per cell and a cell is otherwise a single char. */
#define ATTR(fg, bg)  ((unsigned char)(((bg) << 4) | (fg)))
#define ATTR_FG(a)    ((a) & 0x0F)
#define ATTR_BG(a)    (((a) >> 4) & 0x0F)
#define ATTR_DEFAULT  ATTR(7, 0)

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
static unsigned char g_attrs[HISTORY_ROWS][MAX_COLS];
static unsigned char g_pen = ATTR_DEFAULT;
static int  g_cols = 80, g_rows = 24;
static long g_cur_line;         /* the line the cursor is on */
static long g_first;            /* the oldest line still remembered */
static long g_view;             /* the top line of the window */
static int  g_follow = 1;       /* is the view pinned to the bottom? */
static int  g_cur_c;
static volatile int g_dirty = 1;
static struct app g_app;

/* The text is a scroll view's content now, so the bar, its thumb, the track
 * and the wheel are the library's. What stays here is the position, because a
 * terminal's position is a line and not a pixel offset: lines fall off the
 * back of the ring as the shell writes, so the same line sits at a different
 * offset a moment later, and it is the line that "scrolled back to here"
 * means. The two are put in step by push_scroll and pull_scroll below. */
static struct ui_view* g_v_scroll;
static struct ui_view* g_v_text;
static int g_pushed = -1;       /* the offset push_scroll last wrote */

/* Take the framework's buffer and size. */
static void adopt_window(void)
{
    extern struct app g_app;
    g_px = g_app.px;
    if (g_app.w != 0) g_win_w = g_app.w;
    if (g_app.h != 0) g_win_h = g_app.h;
}

/* The line being typed, assembled here rather than in the shell: a pipe has no
 * line discipline, so nothing between the keyboard and the shell would
 * otherwise know that backspace means anything. */
static char g_line[512];
static unsigned g_line_len;
static volatile int g_shell_done;

static int g_from_shell;        /* read end of the shell's output */
static int g_to_shell;          /* write end of the shell's input */
static int g_shell_pid;

/* --- the keys that are not text -------------------------------------------
 *
 * Ctrl-C and its two neighbours do not go down the pipe. They are signals, and
 * they go to whichever process group the shell has told us is in front - not to
 * the shell, which is usually sitting in wait() and is not the thing the person
 * wants to interrupt.
 *
 * A real UNIX does this in the tty driver, on the keyboard interrupt. There is
 * no tty driver here: this program *is* the terminal, so this is where it goes.
 */
struct control_key { char ch; int signo; const char* echo; };

static const struct control_key kControlKeys[] = {
    { 0x03, SIGINT,  "^C" },
    { 0x1A, SIGTSTP, "^Z" },
    { 0x1C, SIGQUIT, "^\\" },
};

/* --- the grid ------------------------------------------------------------ */

static long slot_of(long line)
{
    long slot = line % HISTORY_ROWS;
    return slot < 0 ? slot + HISTORY_ROWS : slot;
}

static char*          row_of(long line)  { return g_cells[slot_of(line)]; }
static unsigned char* attr_of(long line) { return g_attrs[slot_of(line)]; }

static void clear_line(long line)
{
    char* row = row_of(line);
    unsigned char* attr = attr_of(line);
    for (int c = 0; c < MAX_COLS; ++c) {
        row[c] = ' ';
        /* Cleared in the current colours, not the default ones: a program that
         * sets a background and then clears the screen means that background,
         * and every terminal does it this way. */
        attr[c] = g_pen;
    }
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

/* --- escape sequences -------------------------------------------------------
 *
 * `clear` printed [2J[H for as long as this terminal has existed, because it
 * sends what every terminal since the VT100 understands and this one
 * understood nothing. The escapes that matter are few: move the cursor, erase
 * something, change the colour. What is not here - scrolling regions,
 * alternate screens, character sets - is not here because nothing sends it,
 * and a half-implemented one is worse than an ignored one.
 *
 * Parsed as a small state machine rather than by scanning ahead, because the
 * bytes arrive from a pipe and a sequence can be split across two reads.
 */
enum esc_state { ESC_NONE, ESC_SEEN, ESC_CSI };

static enum esc_state g_esc;
static int g_params[8];
static int g_param_count;
static int g_param_digits;

/* Clear from `from` to `to` on one line, inclusive, in the current colours. */
static void erase_span(long line, int from, int to)
{
    char* row = row_of(line);
    unsigned char* attr = attr_of(line);
    if (from < 0) from = 0;
    if (to >= g_cols) to = g_cols - 1;
    for (int c = from; c <= to; ++c) {
        row[c] = ' ';
        attr[c] = g_pen;
    }
}

static int param(int index, int fallback)
{
    if (index >= g_param_count)
        return fallback;
    return g_params[index] > 0 ? g_params[index] : fallback;
}

/* SGR - the colour and attribute codes. */
static void set_graphics(void)
{
    if (g_param_count == 0) {
        g_pen = ATTR_DEFAULT;
        return;
    }
    for (int i = 0; i < g_param_count; ++i) {
        const int p = g_params[i];
        if (p == 0)
            g_pen = ATTR_DEFAULT;
        else if (p == 1)
            g_pen = ATTR(ATTR_FG(g_pen) | 8, ATTR_BG(g_pen));   /* bold: bright */
        else if (p == 22)
            g_pen = ATTR(ATTR_FG(g_pen) & 7, ATTR_BG(g_pen));
        else if (p == 7)
            g_pen = ATTR(ATTR_BG(g_pen), ATTR_FG(g_pen));       /* reversed */
        else if (p >= 30 && p <= 37)
            g_pen = ATTR((ATTR_FG(g_pen) & 8) | (p - 30), ATTR_BG(g_pen));
        else if (p == 39)
            g_pen = ATTR(7, ATTR_BG(g_pen));
        else if (p >= 40 && p <= 47)
            g_pen = ATTR(ATTR_FG(g_pen), p - 40);
        else if (p == 49)
            g_pen = ATTR(ATTR_FG(g_pen), 0);
        else if (p >= 90 && p <= 97)
            g_pen = ATTR((p - 90) | 8, ATTR_BG(g_pen));
        else if (p >= 100 && p <= 107)
            g_pen = ATTR(ATTR_FG(g_pen), (p - 100) | 8);
    }
}

/* The top line of the window, which is where a cursor address of 1;1 lands.
 * Addressing is relative to the window and not to the history: a program that
 * says "go to the top" means the top of what it can see. */
static long window_top(void) { return g_view; }

static void csi_final(char ch)
{
    switch (ch) {
    case 'A': g_cur_line -= param(0, 1); break;         /* up */
    case 'B': g_cur_line += param(0, 1); break;         /* down */
    case 'C': g_cur_c += param(0, 1); break;            /* right */
    case 'D': g_cur_c -= param(0, 1); break;            /* left */
    case 'G': g_cur_c = param(0, 1) - 1; break;         /* to a column */
    case 'H':
    case 'f':
        g_cur_line = window_top() + param(0, 1) - 1;
        g_cur_c = param(1, 1) - 1;
        break;
    case 'J': {                                          /* erase display */
        const int what = g_param_count > 0 ? g_params[0] : 0;
        const long top = window_top();
        if (what == 0) {
            erase_span(g_cur_line, g_cur_c, g_cols - 1);
            for (long r = g_cur_line + 1; r < top + g_rows; ++r)
                erase_span(r, 0, g_cols - 1);
        } else if (what == 1) {
            for (long r = top; r < g_cur_line; ++r)
                erase_span(r, 0, g_cols - 1);
            erase_span(g_cur_line, 0, g_cur_c);
        } else {
            /* Erase everything: the screen is scrolled away rather than
             * painted over.
             *
             * A terminal without scrollback has nowhere to put the old screen,
             * so it overwrites it and homes the cursor. Here there is
             * somewhere, and overwriting would be worse in both directions:
             * what was on screen would be lost even though the history it
             * belongs to is still there, and the cursor would be left pointing
             * into the middle of that history with everything printed
             * afterwards landing on top of it.
             *
             * So a fresh window's worth of blank lines is appended and the
             * view moves down to them. What was there scrolls up, exactly
             * where a person expects to find it. */
            for (int r = 0; r < g_rows; ++r)
                newline();
            g_cur_c = 0;
            g_follow = 1;
            g_view = last_view();
        }
        break;
    }
    case 'K': {                                          /* erase line */
        const int what = g_param_count > 0 ? g_params[0] : 0;
        if (what == 0)      erase_span(g_cur_line, g_cur_c, g_cols - 1);
        else if (what == 1) erase_span(g_cur_line, 0, g_cur_c);
        else                erase_span(g_cur_line, 0, g_cols - 1);
        break;
    }
    case 'm': set_graphics(); break;
    default: break;                     /* anything else is ignored, not shown */
    }

    if (g_cur_c < 0) g_cur_c = 0;
    if (g_cur_c >= g_cols) g_cur_c = g_cols - 1;
    if (g_cur_line < g_first) g_cur_line = g_first;
    /* Never past the bottom of the window: a cursor address is about the
     * window, and letting one run off it would scroll by arithmetic. */
    if (g_cur_line > window_top() + g_rows - 1)
        g_cur_line = window_top() + g_rows - 1;
}

/* Feed one byte to the parser. Returns 1 when it was consumed as part of a
 * sequence and must not be printed. */
static int escape_byte(char ch)
{
    switch (g_esc) {
    case ESC_NONE:
        if (ch != 0x1B)
            return 0;
        g_esc = ESC_SEEN;
        return 1;

    case ESC_SEEN:
        if (ch == '[') {
            g_esc = ESC_CSI;
            g_param_count = 0;
            g_param_digits = 0;
            g_params[0] = 0;
        } else {
            /* A two-character escape. None of them are understood, and
             * swallowing one is better than printing half of it. */
            g_esc = ESC_NONE;
        }
        return 1;

    case ESC_CSI:
        if (ch >= '0' && ch <= '9') {
            if (g_param_count == 0)
                g_param_count = 1;
            if (g_param_count <= 8) {
                g_params[g_param_count - 1] =
                    g_params[g_param_count - 1] * 10 + (ch - '0');
                g_param_digits = 1;
            }
            return 1;
        }
        if (ch == ';') {
            if (g_param_count < 8)
                g_params[g_param_count++] = 0;
            g_param_digits = 0;
            return 1;
        }
        if (ch == '?' || ch == '>' || ch == '!') {
            /* A private-use introducer. The sequence is still swallowed
             * whole; it just does nothing. */
            return 1;
        }
        (void)g_param_digits;
        csi_final(ch);
        g_esc = ESC_NONE;
        return 1;
    }
    return 0;
}

/* Call with the lock held. */
static void term_putc(char ch)
{
    if (escape_byte(ch))
        return;

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
        attr_of(g_cur_line)[g_cur_c] = g_pen;
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
    attr_of(g_cur_line)[g_cur_c] = g_pen;
    if (++g_cur_c >= g_cols)
        newline();
}

/* --- drawing ------------------------------------------------------------- */

static void draw_glyph(int x0, int y0, char ch, uint32_t fg, uint32_t bg)
{
    if (x0 < 0 || y0 < 0 ||
        x0 + GLYPH_W > (int)g_win_w || y0 + GLYPH_H > (int)g_win_h)
        return;
    const unsigned char* glyph = &g_font[(unsigned char)ch * 16];
    for (int r = 0; r < GLYPH_H; ++r) {
        uint32_t* out = &g_px[(unsigned long)(y0 + r) * g_win_w + x0];
        const unsigned char bits = glyph[r];
        for (int c = 0; c < GLYPH_W; ++c)
            out[c] = (bits & (0x80 >> c)) ? (0xFF000000u | fg) : bg;
    }
}

/* The text area stops short of the scrollbar. */
static int text_width(void)
{
    const int w = (int)g_win_w - WG_SCROLL_W;
    return w < GLYPH_W ? GLYPH_W : w;
}

/* How tall the whole history is. The scroll view asks during layout, and it is
 * the one thing it cannot work out about a view that draws its own content. */
static int measure_text(struct ui_view* v, int width, void* user)
{
    (void)v; (void)width; (void)user;
    mutex_lock(&g_lock);
    const long lines = line_count();
    mutex_unlock(&g_lock);
    return (int)lines * GLYPH_H;
}

static void push_scroll(void);
static void pull_scroll(void);

static void repaint(struct ui_view* view, void* user)
{
    (void)user;
    /* The window's pixels and size, taken every time rather than remembered.
     * The old main assigned these once after win_map and again on every
     * resize; the framework owns the buffer now, and a terminal holding a
     * stale pointer writes its glyphs into a mapping nobody is showing - or
     * past the end of the one that is, which is what it did. */
    adopt_window();
    /* The frame is the whole history, shifted by the scroll, so line `a` is at
     * a fixed place in it and the rows that are on screen are the ones the
     * shift brought there. Only those are drawn: the clip would hide the rest,
     * but a history of five thousand lines should not draw five thousand rows
     * to show twenty-four. */
    const struct ui_rect f = view->frame;
    mutex_lock(&g_lock);
    const int left = f.x, top = f.y + (int)(g_view - g_first) * GLYPH_H;
    for (int r = 0; r < g_rows; ++r) {
        const char* row = row_of(g_view + r);
        const unsigned char* attr = attr_of(g_view + r);
        for (int c = 0; c < g_cols; ++c)
            draw_glyph(left + c * GLYPH_W, top + r * GLYPH_H, row[c],
                       kPalette[ATTR_FG(attr[c])],
                       ATTR_BG(attr[c]) == 0 ? BG
                                             : 0xFF000000u | kPalette[ATTR_BG(attr[c])]);
    }

    /* A block cursor, drawn last so it sits over whatever is under it - and
     * only when the line it is on is one of the ones being shown, because
     * scrolled back is exactly the case where it is not. */
    const long cur_row = g_cur_line - g_view;
    if (cur_row >= 0 && cur_row < g_rows)
        draw_glyph(left + g_cur_c * GLYPH_W, top + (int)cur_row * GLYPH_H,
                   row_of(g_cur_line)[g_cur_c], BG, CURSOR);

    /* Whatever the glyphs did not cover: the strip left of the bar when the
     * width is not a whole number of characters, and the one below the last
     * row. Without these the window keeps whatever was there before. */
    const int used_w = g_cols * GLYPH_W, used_h = g_rows * GLYPH_H;
    wg_target(g_px, g_win_w, g_win_h);
    {
        /* Written directly: wg_fill forces an opaque byte, which is right for
         * a control and wrong for the terminal's own surface. */
        const uint32_t bg = BG;
        for (unsigned yy = 0; yy < g_win_h; ++yy) {
            uint32_t* r = &g_px[(unsigned long)yy * g_win_w];
            if (used_w < text_width())
                for (int xx = used_w; xx < text_width(); ++xx)
                    r[xx] = bg;
            if ((int)yy >= used_h)
                for (int xx = 0; xx < text_width(); ++xx)
                    r[xx] = bg;
        }
    }
    mutex_unlock(&g_lock);
}

/* Put the scroll view where the terminal is looking.
 *
 * The lock is taken for the two numbers and let go before the layout: laying
 * out asks the content how tall it is, and that asks for the lock too. Holding
 * it across the call would be a deadlock against ourselves. */
static void push_scroll(void)
{
    if (g_v_scroll == 0)
        return;
    mutex_lock(&g_lock);
    const int want = (int)(g_view - g_first) * GLYPH_H;
    mutex_unlock(&g_lock);
    if (want == g_v_scroll->scroll && want == g_pushed)
        return;
    g_v_scroll->scroll = want;
    /* The child is positioned from the offset, so it is laid out again at once
     * rather than a frame later. */
    ui_layout(g_v_scroll, g_v_scroll->frame);
    g_pushed = g_v_scroll->scroll;      /* it may have been clamped */
}

/* And take back a move the library made - a wheel notch, a drag of the thumb,
 * a press on the track. Only when the offset is not the one push_scroll left
 * there, so that nothing else is mistaken for a move. */
static void pull_scroll(void)
{
    if (g_v_scroll == 0 || g_v_scroll->scroll == g_pushed)
        return;
    mutex_lock(&g_lock);
    /* Down to a whole line: a character grid means nothing between two rows,
     * and push_scroll puts the offset back on the boundary next tick. */
    long line = g_first + g_v_scroll->scroll / GLYPH_H;
    if (line < g_first)     line = g_first;
    if (line > last_view()) line = last_view();
    g_view = line;
    g_follow = g_view == last_view();
    mutex_unlock(&g_lock);
    g_pushed = g_v_scroll->scroll;
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

/* The line settings, as the program at the other end last left them. Read on
 * every key rather than cached, because it is another process that changes
 * them and it does not tell anybody when it does. */
static unsigned line_flags(void)
{
    struct termios t;
    if (tcgetattr(0, &t) != 0)
        return ISIG | ICANON | ECHO;    /* no control block: behave normally */
    return t.c_lflag;
}

/* Send one to the foreground job, and say so on the screen. True if the key
 * was one of these and has been dealt with. */
static int control_key(char ch)
{
    if ((line_flags() & ISIG) == 0)
        return 0;                       /* raw mode: it is just a byte */

    for (unsigned i = 0; i < sizeof(kControlKeys) / sizeof(kControlKeys[0]); ++i) {
        if (kControlKeys[i].ch != ch)
            continue;

        /* Echoed whether or not anything is listening, because a person who
         * pressed Ctrl-C wants to see that they did - and if the job has
         * already finished, seeing nothing at all reads as a stuck terminal. */
        mutex_lock(&g_lock);
        follow_bottom();
        for (const char* e = kControlKeys[i].echo; *e != '\0'; ++e)
            term_putc(*e);
        term_putc('\n');
        mutex_unlock(&g_lock);
        g_dirty = 1;

        const int fg = (int)tcgetpgrp(0);
        /* Nothing in front means the shell itself is what is running, and it
         * is the shell's own group that should hear about it. */
        kill(-(fg > 0 ? fg : g_shell_pid), kControlKeys[i].signo);
        return 1;
    }
    return 0;
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

    /* Made before the fork, so the shell inherits the key rather than having
     * to be told it afterwards - by which time it may already have started
     * something. */
    tty_control_create();

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

        /* Its own session, with this terminal. The shell is the session
         * leader; every job it starts is a group within that session, and
         * tcsetpgrp names which of them is in front. */
        setsid();

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
    g_shell_pid = pid;
    return pid;
}

/* --- the interface ---------------------------------------------------------
 *
 * The grid is a custom view: it is a character cell array with its own
 * scrollback, colours and cursor, and nothing in the component library is
 * shaped like that. What the framework takes over is the window, the loop and
 * the repainting - including the part this program got wrong nowhere else but
 * had to write anyway.
 */

static int on_tick(struct app* a)
{
    (void)a;
    /* The reader thread sets g_dirty when output arrives. Repainting is the
     * framework's, so this only answers whether anything changed. */
    if (g_shell_done) {
        app_quit(a, 0);
        return 0;
    }
    if (!g_dirty)
        return 0;
    g_dirty = 0;
    /* The history grew, so the document did: laying out again is what tells
     * the bar its new length, and putting the offset back is what keeps the
     * text where the terminal is looking while it does. */
    app_relayout(a);
    push_scroll();
    return 1;
}

static int on_event(struct app* a, const struct win_event* event)
{
    if (event->type == WIN_EVENT_RESIZE) {
        (void)a;
        adopt_window();
        resize_grid();
        return 1;
    }

    /* The bar, the thumb, the track and the wheel have already been dealt
     * with by the components; what is left is to notice that they moved. */
    if (event->type == WIN_EVENT_SCROLL || event->type == WIN_EVENT_MOUSE_DOWN ||
        event->type == WIN_EVENT_MOUSE_MOVE || event->type == WIN_EVENT_MOUSE_UP) {
        pull_scroll();
        g_dirty = 1;
        return 0;
    }
    if (event->type != WIN_EVENT_KEY)
        return 0;


            /* Typing goes to the bottom, wherever the view happened to be. */
            mutex_lock(&g_lock);
            follow_bottom();
            mutex_unlock(&g_lock);

            const char ch = (char)event->key;

            /* Before the line editor, because these are not text and must not
             * be assembled into a line: Ctrl-C during a half-typed command
             * interrupts the job and throws the half away, which is what
             * every terminal does and what a person expects. */
            if (control_key(ch)) {
                g_line_len = 0;
                return 1;
            }

            /* Raw mode: the byte goes down as it is, with no line to wait for
             * and nothing shown unless the program at the other end decides to
             * show it. This is what an editor asks for, and the whole reason
             * termios exists. */
            const unsigned flags = line_flags();
            if ((flags & ICANON) == 0) {
                if ((flags & ECHO) != 0) {
                    mutex_lock(&g_lock);
                    follow_bottom();
                    term_putc(ch);
                    mutex_unlock(&g_lock);
                    g_dirty = 1;
                }
                write(g_to_shell, &ch, 1);
                g_line_len = 0;
                return 1;
            }

            if (ch == '\n' || ch == '\r') {
                mutex_lock(&g_lock);
                term_putc('\n');
                mutex_unlock(&g_lock);
                g_line[g_line_len] = '\n';
                write(g_to_shell, g_line, g_line_len + 1);
                g_line_len = 0;
                g_dirty = 1;
            } else if (ch == '\b' || ch == 0x7F) {
                if (g_line_len > 0) {
                    --g_line_len;
                    mutex_lock(&g_lock);
                    term_putc('\b');
                    mutex_unlock(&g_lock);
                    g_dirty = 1;
                }
            } else if ((unsigned char)ch >= 32 && g_line_len + 1 < sizeof(g_line)) {
                g_line[g_line_len++] = ch;
                if ((flags & ECHO) != 0) {
                    mutex_lock(&g_lock);
                    term_putc(ch);
                    mutex_unlock(&g_lock);
                    g_dirty = 1;
                }
            }
    return 1;
}

int main(int argc, char** argv)
{
    if (fb_font(g_font) != 0) {
        printf("term: cannot read the console font\n");
        return 1;
    }

    struct ui_view* root = ui_box(0, UI_STACK_V, 0, 0);
    /* The history scrolls, and the scrolling is the library's. This window
     * drew a bar of its own, hit-tested it and dragged its thumb by hand, and
     * had no wheel at all - there was nothing here listening for one. */
    g_v_scroll = ui_grow(ui_scroll(root), 1);
    g_v_text = ui_custom(g_v_scroll, repaint, 0);
    ui_measure(g_v_text, measure_text);

    /* Eighty columns of text plus the bar, so the terminal is still the eighty
     * columns everything assumes and the scrollbar is not taken out of them. */
    g_app.title = "Terminal";
    g_app.width = 80 * GLYPH_W + WG_SCROLL_W;
    g_app.height = 24 * GLYPH_H;
    /* Below this the shell's prompt has nowhere to go. */
    g_app.min_width = 20 * GLYPH_W + WG_SCROLL_W;
    g_app.min_height = 4 * GLYPH_H;
    g_app.tick_ms = 15;
    g_app.tick = on_tick;
    g_app.event = on_event;
    g_app.root = root;

    /* An empty grid before the shell writes into it: main used to do this
     * between creating the window and starting the shell. */
    term_clear();

    if (start_shell() < 0) {
        printf("term: cannot start a shell\n");
        return 1;
    }
    thread_create(reader_thread, 0);

    return app_run(&g_app, argc, argv);
}
