/* vi - a visual editor for a terminal.
 *
 * There was no way to edit a file without the window server. Edit is a window;
 * if the desktop does not come up, or a machine is reached over something that
 * is not a screen, there was nothing - including nothing to fix the file that
 * was stopping the desktop coming up. That is a bad place for a system to be
 * and the reason this exists.
 *
 * It is vi rather than ed because the terminal can address its cursor now, and
 * a screen editor on a terminal that can is worth more than a line editor on
 * one that cannot. It is a subset: the commands somebody actually uses to
 * change a file and get out again, and not the ones that make vi a language.
 *
 * The buffer is one run of bytes with the cursor as an offset into it, rather
 * than an array of lines. Every edit is then a memmove and nothing has to be
 * kept in step - no line lengths, no reflowing, no special case for joining
 * two lines because a newline is a byte like any other. What that costs is
 * that finding line N means counting newlines, which at the size of a file
 * anybody edits by hand is nothing.
 */

#include <cli.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#define TEXT_MAX 262144

static char  g_text[TEXT_MAX];
static long  g_len;
static long  g_cursor;          /* an offset into g_text */
static long  g_top;             /* the offset of the first line on screen */
static char  g_path[256];
static int   g_dirty;
static int   g_rows = 24, g_cols = 80;
static char  g_message[128];
static char  g_command[128];    /* what is being typed after : or / */
static int   g_command_len;
static char  g_search[128];

/* One level, which is the one people use. A second buffer the size of the
 * first is cheap and a full history is a different program. */
static char  g_undo[TEXT_MAX];
static long  g_undo_len;
static long  g_undo_cursor;
static int   g_have_undo;

enum { NORMAL, INSERT, COMMAND, SEARCH };
static int g_mode = NORMAL;

/* --- the buffer ----------------------------------------------------------- */

static long line_start(long at)
{
    while (at > 0 && g_text[at - 1] != '\n')
        --at;
    return at;
}

static long line_end(long at)
{
    while (at < g_len && g_text[at] != '\n')
        ++at;
    return at;
}

static long next_line(long at)
{
    const long end = line_end(at);
    return end < g_len ? end + 1 : end;
}

static long prev_line(long at)
{
    const long start = line_start(at);
    return start > 0 ? line_start(start - 1) : start;
}

static long line_number(long at)
{
    long n = 1;
    for (long i = 0; i < at && i < g_len; ++i)
        if (g_text[i] == '\n')
            ++n;
    return n;
}

/* How many lines there are to look at.
 *
 * A text file ends with a newline, and that newline ends the last line rather
 * than starting an empty one after it - so counting separators and adding one
 * says four for a three-line file. Every editor counts the way `wc -l` does,
 * and a status line that disagreed with wc would be the one wrong. */
static long total_lines(void)
{
    long n = 0;
    for (long i = 0; i < g_len; ++i)
        if (g_text[i] == '\n')
            ++n;
    if (g_len > 0 && g_text[g_len - 1] != '\n')
        ++n;                            /* a last line with no newline on it */
    return n > 0 ? n : 1;               /* an empty buffer is one empty line */
}

static void remember(void)
{
    memcpy(g_undo, g_text, (unsigned long)g_len);
    g_undo_len = g_len;
    g_undo_cursor = g_cursor;
    g_have_undo = 1;
}

static void insert(long at, const char* what, long n)
{
    if (g_len + n > TEXT_MAX || n <= 0)
        return;
    memmove(&g_text[at + n], &g_text[at], (unsigned long)(g_len - at));
    memcpy(&g_text[at], what, (unsigned long)n);
    g_len += n;
    g_dirty = 1;
}

static void erase(long at, long n)
{
    if (n <= 0 || at < 0 || at + n > g_len)
        return;
    memmove(&g_text[at], &g_text[at + n], (unsigned long)(g_len - at - n));
    g_len -= n;
    g_dirty = 1;
}

/* --- the screen ----------------------------------------------------------- */

static void put(const char* s)
{
    write(1, s, strlen(s));
}

static void at(int row, int column)
{
    char code[24];
    snprintf(code, sizeof(code), "\x1b[%d;%dH", row, column);
    put(code);
}

/* Bring the cursor's line onto the screen, by moving the window rather than
 * the cursor: what the person asked for was to be somewhere, not to see
 * somewhere. */
static void follow(void)
{
    const int page = g_rows - 1;
    if (g_cursor < g_top) {
        g_top = line_start(g_cursor);
        return;
    }
    /* Count the lines between the top and the cursor; if there are too many,
     * walk the top down until there are not. */
    for (;;) {
        long n = 0, i = g_top;
        while (i < g_cursor && n < page) {
            i = next_line(i);
            ++n;
        }
        if (n < page || i >= g_cursor)
            break;
        g_top = next_line(g_top);
    }
}

static void draw(void)
{
    follow();
    put("\x1b[H");                              /* home, then line by line */

    long at_off = g_top;
    int cursor_row = 1, cursor_col = 1;
    for (int row = 1; row <= g_rows - 1; ++row) {
        at(row, 1);
        put("\x1b[K");                          /* clear what was there */
        if (at_off <= g_len) {
            const long end = line_end(at_off);
            if (g_cursor >= at_off && g_cursor <= end) {
                cursor_row = row;
                cursor_col = (int)(g_cursor - at_off) + 1;
                if (cursor_col > g_cols) cursor_col = g_cols;
            }
            long n = end - at_off;
            if (n > g_cols) n = g_cols;
            if (n > 0)
                write(1, &g_text[at_off], (unsigned long)n);
            at_off = end < g_len ? end + 1 : g_len + 1;
        } else {
            /* Past the end of the file. A tilde, which is how vi says "there
             * is nothing here" rather than leaving a blank that looks like an
             * empty line in the file. */
            put("~");
        }
    }

    /* The status line: what file, whether it has changed, and where we are -
     * or what is being typed, when something is. */
    at(g_rows, 1);
    put("\x1b[K");
    char status[256];
    if (g_mode == COMMAND || g_mode == SEARCH) {
        snprintf(status, sizeof(status), "%c%s", g_mode == COMMAND ? ':' : '/',
                 g_command);
    } else if (g_message[0] != '\0') {
        snprintf(status, sizeof(status), "%s", g_message);
    } else {
        snprintf(status, sizeof(status), "\"%s\"%s %ld/%ld  %s",
                 g_path[0] != '\0' ? g_path : "[no name]",
                 g_dirty ? " [+]" : "",
                 line_number(g_cursor), total_lines(),
                 g_mode == INSERT ? "-- INSERT --" : "");
    }
    write(1, status, strlen(status));

    if (g_mode == COMMAND || g_mode == SEARCH)
        at(g_rows, (int)strlen(status) + 1);
    else
        at(cursor_row, cursor_col);
}

/* --- files ---------------------------------------------------------------- */

static void load(const char* path)
{
    g_len = 0;
    g_cursor = 0;
    g_top = 0;
    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(g_message, sizeof(g_message), "\"%s\" [new file]", path);
        return;
    }
    long n;
    while ((n = read(fd, &g_text[g_len], (unsigned long)(TEXT_MAX - g_len))) > 0)
        g_len += n;
    close(fd);
    snprintf(g_message, sizeof(g_message), "\"%s\" %ld lines, %ld bytes",
             path, total_lines(), g_len);
}

static int save(const char* path)
{
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return -1;
    long written = 0;
    while (written < g_len) {
        const long n = write(fd, &g_text[written], (unsigned long)(g_len - written));
        if (n <= 0)
            break;
        written += n;
    }
    close(fd);
    if (written != g_len)
        return -1;
    g_dirty = 0;
    return 0;
}

/* --- searching ------------------------------------------------------------- */

static long find_from(const char* needle, long from)
{
    const long n = (long)strlen(needle);
    if (n == 0 || g_len == 0)
        return -1;
    for (long i = from; i + n <= g_len; ++i)
        if (memcmp(&g_text[i], needle, (unsigned long)n) == 0)
            return i;
    /* Round the end, which is what a search in an editor does: the thing you
     * are looking for is as likely to be behind you as in front. */
    for (long i = 0; i + n <= from && i + n <= g_len; ++i)
        if (memcmp(&g_text[i], needle, (unsigned long)n) == 0)
            return i;
    return -1;
}

/* --- commands -------------------------------------------------------------- */

static int g_quit;

/* Everything after a colon. Returns 0 normally, 1 to leave. */
static void run_command(const char* line)
{
    g_message[0] = '\0';

    int force = 0;
    char verb[64];
    int n = 0;
    const char* p = line;
    while (*p == ' ') ++p;
    while (*p != '\0' && *p != ' ' && n < (int)sizeof(verb) - 1)
        verb[n++] = *p++;
    verb[n] = '\0';
    while (*p == ' ') ++p;
    if (n > 0 && verb[n - 1] == '!') {
        force = 1;
        verb[--n] = '\0';
    }

    /* A bare number is a line to go to, which is how :42 works everywhere. */
    if (verb[0] >= '0' && verb[0] <= '9') {
        long want = atoi_simple(verb), here = 0;
        g_cursor = 0;
        for (long i = 1; i < want && g_cursor < g_len; ++i)
            g_cursor = next_line(g_cursor);
        (void)here;
        return;
    }

    const char* file = *p != '\0' ? p : g_path;

    if (strcmp(verb, "w") == 0 || strcmp(verb, "wq") == 0 ||
        strcmp(verb, "x") == 0) {
        if (file[0] == '\0') {
            snprintf(g_message, sizeof(g_message), "no file name");
            return;
        }
        if (save(file) != 0) {
            snprintf(g_message, sizeof(g_message), "\"%s\" not written: %s",
                     file, strerror(errno));
            return;
        }
        snprintf(g_path, sizeof(g_path), "%s", file);
        snprintf(g_message, sizeof(g_message), "\"%s\" %ld bytes written",
                 file, g_len);
        if (strcmp(verb, "w") != 0)
            g_quit = 1;
        return;
    }
    if (strcmp(verb, "q") == 0) {
        if (g_dirty && !force) {
            snprintf(g_message, sizeof(g_message),
                     "no write since last change (:q! to leave anyway)");
            return;
        }
        g_quit = 1;
        return;
    }
    snprintf(g_message, sizeof(g_message), "not a command: %s", verb);
}

/* --- keys ------------------------------------------------------------------ */

static void word_forward(void)
{
    while (g_cursor < g_len && g_text[g_cursor] != ' ' &&
           g_text[g_cursor] != '\n')
        ++g_cursor;
    while (g_cursor < g_len && (g_text[g_cursor] == ' ' ||
                                g_text[g_cursor] == '\n'))
        ++g_cursor;
}

static void word_back(void)
{
    if (g_cursor > 0) --g_cursor;
    while (g_cursor > 0 && (g_text[g_cursor] == ' ' || g_text[g_cursor] == '\n'))
        --g_cursor;
    while (g_cursor > 0 && g_text[g_cursor - 1] != ' ' &&
           g_text[g_cursor - 1] != '\n')
        --g_cursor;
}

/* Down or up a line, keeping the column. */
static void vertical(int down)
{
    const long start = line_start(g_cursor);
    const long column = g_cursor - start;
    const long target = down ? next_line(g_cursor) : prev_line(g_cursor);
    if (down && target >= g_len && line_start(g_cursor) == line_start(g_len))
        return;                                 /* already on the last line */
    const long end = line_end(target);
    g_cursor = target + column;
    if (g_cursor > end)
        g_cursor = end;
}

static void normal_key(char c)
{
    static int pending;                         /* the first of a pair: d, g */

    if (pending == 'd') {
        pending = 0;
        if (c == 'd') {
            remember();
            const long start = line_start(g_cursor);
            const long end = line_end(g_cursor);
            erase(start, (end < g_len ? end + 1 : end) - start);
            g_cursor = start > g_len ? g_len : start;
        }
        return;
    }
    if (pending == 'g') {
        pending = 0;
        if (c == 'g')
            g_cursor = 0;
        return;
    }

    switch (c) {
    case 'h': if (g_cursor > line_start(g_cursor)) --g_cursor; break;
    case 'l': if (g_cursor < line_end(g_cursor)) ++g_cursor; break;
    case 'j': vertical(1); break;
    case 'k': vertical(0); break;
    case '0': g_cursor = line_start(g_cursor); break;
    case '$': g_cursor = line_end(g_cursor); break;
    case '^': {
        g_cursor = line_start(g_cursor);
        while (g_cursor < line_end(g_cursor) &&
               (g_text[g_cursor] == ' ' || g_text[g_cursor] == '\t'))
            ++g_cursor;
        break;
    }
    case 'w': word_forward(); break;
    case 'b': word_back(); break;
    case 'G': g_cursor = line_start(g_len); break;
    case 'g': pending = 'g'; break;
    case 'd': pending = 'd'; break;

    case 'i': g_mode = INSERT; break;
    case 'a': if (g_cursor < line_end(g_cursor)) ++g_cursor;
              g_mode = INSERT; break;
    case 'I': g_cursor = line_start(g_cursor); g_mode = INSERT; break;
    case 'A': g_cursor = line_end(g_cursor); g_mode = INSERT; break;
    case 'o': {
        remember();
        g_cursor = line_end(g_cursor);
        insert(g_cursor, "\n", 1);
        ++g_cursor;
        g_mode = INSERT;
        break;
    }
    case 'O': {
        remember();
        g_cursor = line_start(g_cursor);
        insert(g_cursor, "\n", 1);
        g_mode = INSERT;
        break;
    }
    case 'x':
        if (g_cursor < line_end(g_cursor)) {
            remember();
            erase(g_cursor, 1);
        }
        break;
    case 'D':
        remember();
        erase(g_cursor, line_end(g_cursor) - g_cursor);
        break;
    case 'J': {
        /* Two lines into one, with a space where the newline was - which is
         * what makes it useful on wrapped prose rather than a plain delete. */
        const long end = line_end(g_cursor);
        if (end < g_len) {
            remember();
            erase(end, 1);
            insert(end, " ", 1);
            g_cursor = end;
        }
        break;
    }
    case 'u':
        if (g_have_undo) {
            /* The undo is itself undoable, which is one level used twice and
             * is what people reach for immediately after pressing it. */
            static char swap[TEXT_MAX];
            const long swap_len = g_len, swap_cursor = g_cursor;
            memcpy(swap, g_text, (unsigned long)g_len);
            memcpy(g_text, g_undo, (unsigned long)g_undo_len);
            g_len = g_undo_len;
            g_cursor = g_undo_cursor > g_len ? g_len : g_undo_cursor;
            memcpy(g_undo, swap, (unsigned long)swap_len);
            g_undo_len = swap_len;
            g_undo_cursor = swap_cursor;
            g_dirty = 1;
        }
        break;

    case ':': g_mode = COMMAND; g_command[0] = '\0'; g_command_len = 0; break;
    case '/': g_mode = SEARCH;  g_command[0] = '\0'; g_command_len = 0; break;
    case 'n': {
        if (g_search[0] == '\0')
            break;
        const long found = find_from(g_search, g_cursor + 1);
        if (found >= 0) g_cursor = found;
        else snprintf(g_message, sizeof(g_message), "not found: %s", g_search);
        break;
    }
    case 0x06: {                                /* Ctrl-F, a page down */
        for (int i = 0; i < g_rows - 2; ++i) vertical(1);
        break;
    }
    case 0x02: {                                /* Ctrl-B, a page up */
        for (int i = 0; i < g_rows - 2; ++i) vertical(0);
        break;
    }
    default: break;
    }
}

static void insert_key(char c)
{
    if (c == 0x1B) {                            /* escape: back to normal */
        g_mode = NORMAL;
        if (g_cursor > line_start(g_cursor))
            --g_cursor;
        return;
    }
    if (c == '\b' || c == 0x7F) {
        if (g_cursor > 0) {
            erase(g_cursor - 1, 1);
            --g_cursor;
        }
        return;
    }
    if (c == '\r')
        c = '\n';
    if (c != '\n' && (unsigned char)c < 32)
        return;
    if (!g_dirty)
        remember();
    insert(g_cursor, &c, 1);
    ++g_cursor;
}

static void command_key(char c)
{
    if (c == 0x1B) {
        g_mode = NORMAL;
        return;
    }
    if (c == '\r' || c == '\n') {
        const int was = g_mode;
        g_mode = NORMAL;
        if (was == COMMAND) {
            run_command(g_command);
        } else {
            snprintf(g_search, sizeof(g_search), "%s", g_command);
            const long found = find_from(g_search, g_cursor + 1);
            if (found >= 0)
                g_cursor = found;
            else
                snprintf(g_message, sizeof(g_message), "not found: %s",
                         g_search);
        }
        return;
    }
    if (c == '\b' || c == 0x7F) {
        if (g_command_len > 0)
            g_command[--g_command_len] = '\0';
        else
            g_mode = NORMAL;
        return;
    }
    if ((unsigned char)c >= 32 && g_command_len < (int)sizeof(g_command) - 1) {
        g_command[g_command_len++] = c;
        g_command[g_command_len] = '\0';
    }
}

int main(int argc, char** argv)
{
    cli_begin(argc, argv, "[file]", "");

    if (cli_argc() > 0) {
        snprintf(g_path, sizeof(g_path), "%s", cli_arg(0));
        load(g_path);
    }

    /* The terminal, as it is rather than as the environment remembers it. A
     * window that has been resized since this program started would otherwise
     * be drawn at the size it was. */
    unsigned rows = 24, cols = 80;
    if (tty_size(0, &rows, &cols) == 0 && rows > 2 && cols > 8) {
        g_rows = (int)rows;
        g_cols = (int)cols;
    }

    /* Keys as they are pressed, not lines: an editor answers to `j` without
     * waiting for a newline, and shows what it decides to rather than what was
     * typed. This is what termios is for, and it is the terminal driver that
     * makes it mean anything. */
    struct termios saved, raw;
    const int have_tty = tcgetattr(0, &saved) == 0;
    if (have_tty) {
        raw = saved;
        raw.c_lflag &= ~(unsigned)(ICANON | ECHO);
        tcsetattr(0, TCSANOW, &raw);
    }

    put("\x1b[2J");
    draw();

    while (!g_quit) {
        char c;
        if (read(0, &c, 1) != 1)
            break;
        g_message[0] = '\0';
        if (g_mode == NORMAL)      normal_key(c);
        else if (g_mode == INSERT) insert_key(c);
        else                       command_key(c);
        draw();
    }

    if (have_tty)
        tcsetattr(0, TCSANOW, &saved);
    put("\x1b[2J\x1b[H");
    return 0;
}
