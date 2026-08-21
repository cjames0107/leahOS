/* Help - the manual, in a window.
 *
 * `man ls` answers the same question, and answers it into a terminal that the
 * thing you are trying to do is not in. The manual is also the one part of
 * this system that is only discoverable if you already know what to look for:
 * `man` with no argument lists the pages, which means the list and the page
 * are two commands and you cannot see both.
 *
 * The pages are plain text - see the note at the top of user/man.c for why
 * they are not troff - so this is a list, a search, and the file.
 */

#include <app.h>
#include <ui.h>
#include <fcntl.h>
#include <paths.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <widget.h>
#include <window.h>

#define MAN_DIR  PATH_SHARE "/man"
#define MAX_PAGES 256
#define MAX_LINES 2048
#define LINE_MAX  200

static struct app g_app;
static struct ui_view* g_list;
static struct ui_view* g_pane;
static struct ui_view* g_scroller;

static void size_page(void);
static struct ui_view* g_search;
static struct ui_view* g_title;

static char g_page[MAX_PAGES][64];      /* every page there is       */
static int  g_pages;
static int  g_shown[MAX_PAGES];         /* the ones the search left  */
static int  g_shown_n;
static char g_find[64];

static char g_line[MAX_LINES][LINE_MAX];
static int  g_lines;

static void load_pages(void)
{
    struct dirent here[MAX_PAGES];
    const int n = getdents(MAN_DIR, here, MAX_PAGES);
    for (int i = 0; i < n && g_pages < MAX_PAGES; ++i) {
        const char* name = here[i].d_name;
        const unsigned len = (unsigned)strlen(name);
        if (len < 3 || name[len - 2] != '.')
            continue;                   /* pages are <name>.<section> */
        snprintf(g_page[g_pages], sizeof(g_page[0]), "%.*s", (int)len - 2, name);
        ++g_pages;
    }
    /* Alphabetical, because a directory is in whatever order the filesystem
     * put it and a list of commands is looked up by name. */
    for (int i = 1; i < g_pages; ++i) {
        char key[64];
        snprintf(key, sizeof(key), "%s", g_page[i]);
        int j = i - 1;
        while (j >= 0 && strcmp(g_page[j], key) > 0) {
            memcpy(g_page[j + 1], g_page[j], sizeof(g_page[0]));
            --j;
        }
        snprintf(g_page[j + 1], sizeof(g_page[0]), "%s", key);
    }
}

static int fold(char c)
{
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

static int matches(const char* name)
{
    if (g_find[0] == '\0')
        return 1;
    for (const char* p = name; *p != '\0'; ++p) {
        int i = 0;
        while (g_find[i] != '\0' && fold(p[i]) == fold(g_find[i]))
            ++i;
        if (g_find[i] == '\0')
            return 1;
    }
    return 0;
}

static void refilter(void)
{
    g_shown_n = 0;
    for (int i = 0; i < g_pages; ++i)
        if (matches(g_page[i]))
            g_shown[g_shown_n++] = i;
    g_list->rows = g_shown_n;
    if (g_list->selected >= g_shown_n)
        g_list->selected = g_shown_n > 0 ? 0 : -1;
}

static const char* page_row(void* user, int row)
{
    (void)user;
    return (row >= 0 && row < g_shown_n) ? g_page[g_shown[row]] : "";
}

static void open_page(const char* name)
{
    g_lines = 0;

    /* Whichever section it is in. The pages are all section 1 today, and
     * looking for the file rather than assuming the number is what stops that
     * being a fact this has to be told about later. */
    char path[256];
    int fd = -1;
    for (int section = 1; section <= 8 && fd < 0; ++section) {
        snprintf(path, sizeof(path), "%s/%s.%d", MAN_DIR, name, section);
        fd = open(path, O_RDONLY);
    }
    if (fd < 0) {
        snprintf(g_line[g_lines++], LINE_MAX, "no manual entry for %s", name);
        ui_set_text(g_title, name);
        size_page();
        return;
    }

    char buf[4096];
    int at = 0;
    long got;
    while ((got = read(fd, buf, sizeof(buf))) > 0) {
        for (long i = 0; i < got; ++i) {
            if (buf[i] == '\n' || at + 1 >= LINE_MAX) {
                g_line[g_lines][at] = '\0';
                if (++g_lines >= MAX_LINES) { at = 0; goto full; }
                at = 0;
                continue;
            }
            if (buf[i] != '\r')
                g_line[g_lines][at++] = buf[i];
        }
    }
    if (at > 0 && g_lines < MAX_LINES)
        g_line[g_lines++][at] = '\0';
full:
    close(fd);
    ui_set_text(g_title, name);
    size_page();
}

/* --- the page ---------------------------------------------------------------- */

/* The page, drawn at its full height inside a scroll view.
 *
 * It kept its own scroll offset, drew only the lines that fitted, and wired
 * its own bar and its own arrow keys - which is what six other applications
 * were also doing, each slightly differently. Inside a ui_scroll the frame is
 * the whole document and it is shifted for us, so this draws every line at
 * where that line is and the scroll view clips what does not fit. */
static void draw_page(struct ui_view* v, void* user)
{
    (void)user;
    const struct ui_rect f = v->frame;

    const int line_h = wg_text_height() + 2;
    for (int i = 0; i < g_lines; ++i) {
        const char* text = g_line[i];
        /* A heading is a line that starts at the left margin and is not
         * indented, which is the whole of the convention these pages follow. */
        const int heading = text[0] != '\0' && text[0] != ' ' &&
                            text[0] >= 'A' && text[0] <= 'Z';
        wg_styled(f.x + 12, f.y + 10 + i * line_h, text, (int)strlen(text),
                  heading ? wg_ink_colour() : WG_INK,
                  wg_text_size(), heading ? WG_STYLE_BOLD : 0);
    }
}

/* How tall the page is, which is what the scroll view lays it out at. */
static void size_page(void)
{
    if (g_pane == 0)
        return;
    g_pane->want_h = g_lines * (wg_text_height() + 2) + 20;
    if (g_scroller != 0)
        g_scroller->scroll = 0;
}

static void on_pick(struct ui_view* v, void* u)
{
    (void)u;
    if (v->selected >= 0 && v->selected < g_shown_n)
        open_page(g_page[g_shown[v->selected]]);
}

static void on_find(struct ui_view* v, void* u)
{
    (void)u;
    snprintf(g_find, sizeof(g_find), "%s", ui_text(v));
    refilter();
}

int main(int argc, char** argv)
{
    load_pages();

    struct ui_view* root = ui_box(0, UI_STACK_H, 0, 0);

    struct ui_view* left = ui_box(root, UI_STACK_V, 8, 6);
    ui_size(left, 190, 0);
    ui_grow(left, 0);
    g_search = ui_grow(ui_size(ui_search(left, "Search"), 0, 24), 0);
    ui_on(g_search, on_find, 0);
    g_list = ui_grow(ui_sidebar(left, page_row, 0, 0), 1);
    ui_on(g_list, on_pick, 0);

    struct ui_view* right = ui_box(root, UI_STACK_V, 10, 6);
    ui_grow(right, 1);
    g_title = ui_grow(ui_size(ui_label(right, ""), 0, 20), 0);
    g_scroller = ui_scroll(right);
    g_pane = ui_custom(g_scroller, draw_page, 0);

    refilter();
    /* Something on screen at once: a help window that opens empty is a window
     * that has to be worked out before it can help. */
    const char* first = 0;
    for (int i = 1; i < argc; ++i)
        if (argv[i][0] != '\0' && (argv[i][0] < '0' || argv[i][0] > '9'))
            first = argv[i];
    if (first == 0 && g_shown_n > 0)
        first = g_page[g_shown[0]];
    if (first != 0) {
        open_page(first);
        for (int i = 0; i < g_shown_n; ++i)
            if (strcmp(g_page[g_shown[i]], first) == 0)
                g_list->selected = i;
    }

    g_app.title = "Help";
    g_app.width = 720; g_app.height = 500;
    g_app.min_width = 520; g_app.min_height = 320;
    g_app.sidebar = 190;
    g_app.root = root;
    return app_run(&g_app, argc, argv);
}
