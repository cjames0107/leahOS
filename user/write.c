/* Write - a rich text editor.
 *
 * Edit is for files that are their characters: source, configuration, notes.
 * This is for documents, where how something is written is part of what it
 * says - and the difference is a file format, because a document that loses
 * its emphasis when it is saved is a plain text file with extra steps.
 *
 * So it reads and writes RTF, and the model behind it is RTF's own: a style
 * per character rather than a tree of runs. See user/libc/rtf.c for why that
 * is the right shape rather than a shortcut.
 *
 * The page is a custom view. Everything around it - the weight, slant and
 * underline toggles, the size, the alignment - is components, because those
 * are ordinary controls and there is nothing to be gained by drawing them by
 * hand.
 */

#include <app.h>
#include <ui.h>
#include <fcntl.h>
#include <rtf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <widget.h>
#include <window.h>

#define MARGIN 22

static struct app g_app;
static struct rtf_doc* g_doc;

/* Where the caret is, and where a selection started. They are equal when there
 * is no selection, which is the same thing a caret is. */
static long g_caret;
static long g_anchor;
static int  g_scroll;           /* in pixels down the document */
static int  g_selecting;

/* The style the next character typed will take. Held rather than read from the
 * document because pressing bold with nothing selected has to mean "what I
 * type next is bold", and there is no character yet to put that on. */
static unsigned char g_typing;

static struct ui_view* g_page;
static struct ui_view* g_bold;
static struct ui_view* g_italic;
static struct ui_view* g_under;
static struct ui_view* g_size;
static struct ui_view* g_align;
static struct ui_view* g_status;

/* --- laying the document out ------------------------------------------------
 *
 * Wrapped to the page's width, which means the line table has to be rebuilt
 * whenever the document or the width changes. It is kept rather than
 * recomputed per frame: drawing walks it, the caret is found through it, and a
 * click is turned into a position with it, so three passes would otherwise
 * measure the same text three times.
 */
struct line {
    long start, end;    /* character range, end exclusive           */
    int  y;             /* top of the line, in document coordinates */
    int  height;
};
static struct line* g_line;
static int g_lines, g_line_cap;
static int g_doc_height;
static int g_laid_for_width = -1;
static long g_laid_for_len = -1;

static int size_px_of(unsigned char style)
{
    /* Points to pixels. The screen is nominally 96 dots to the inch, which
     * makes a point four thirds of a pixel, and rounding it here keeps every
     * measurement of a run in step with every drawing of it. */
    return rtf_size_points[rtf_style_size(style)] * 4 / 3;
}

static unsigned wg_style_of(unsigned char style)
{
    unsigned out = 0;
    if (style & RTF_BOLD)      out |= WG_STYLE_BOLD;
    if (style & RTF_ITALIC)    out |= WG_STYLE_ITALIC;
    if (style & RTF_UNDERLINE) out |= WG_STYLE_UNDERLINE;
    return out;
}

static int add_line(long start, long end, int y, int height)
{
    if (g_lines == g_line_cap) {
        const int want = g_line_cap == 0 ? 128 : g_line_cap * 2;
        struct line* grown = (struct line*)malloc(sizeof(struct line) * (unsigned)want);
        if (grown == 0)
            return 0;
        if (g_line != 0)
            memcpy(grown, g_line, sizeof(struct line) * (unsigned)g_lines);
        free(g_line);
        g_line = grown;
        g_line_cap = want;
    }
    g_line[g_lines].start = start;
    g_line[g_lines].end = end;
    g_line[g_lines].y = y;
    g_line[g_lines].height = height;
    ++g_lines;
    return 1;
}

/* The width of one character, at its own style. Measured one at a time because
 * wrapping needs to know where it stopped fitting, and a run measured whole
 * only says that it did not. */
static int char_width(long i)
{
    if (i < 0 || i >= g_doc->len)
        return 0;
    const unsigned char st = g_doc->style[i];
    if (g_doc->text[i] == '\t')
        return size_px_of(st) * 2;
    return wg_styled_width(&g_doc->text[i], 1, size_px_of(st), wg_style_of(st));
}

static void layout(int width)
{
    g_lines = 0;
    g_doc_height = 0;
    if (g_doc == 0 || width <= 0)
        return;

    const int wrap = width - MARGIN * 2;
    long at = 0;
    int y = 0;
    while (at <= g_doc->len) {
        long i = at;
        int used = 0;
        int height = 0;
        /* Where the line could be broken instead of where it ran out: the last
         * space seen. Without it a long word is broken mid-word and, worse, so
         * is every line that happens to end inside one. */
        long space = -1;

        while (i < g_doc->len && g_doc->text[i] != '\n') {
            const int w = char_width(i);
            const int h = wg_styled_height(size_px_of(g_doc->style[i]));
            if (h > height)
                height = h;
            if (used + w > wrap && i > at) {
                if (space > at) {
                    i = space + 1;      /* the break goes after the space */
                    used = 0;
                }
                break;
            }
            if (g_doc->text[i] == ' ')
                space = i;
            used += w;
            ++i;
        }
        if (height == 0) {
            /* An empty line still occupies one: its height is whatever a
             * character typed there would have. */
            const unsigned char st = at < g_doc->len ? g_doc->style[at] : g_typing;
            height = wg_styled_height(size_px_of(st));
        }

        long end = i;
        if (i < g_doc->len && g_doc->text[i] == '\n')
            ++i;                        /* the newline belongs to this line */

        if (!add_line(at, end, y, height))
            break;
        y += height;
        if (i == at)                    /* nothing consumed: stop rather than spin */
            break;
        at = i;
        if (at == g_doc->len && (g_doc->len == 0 || g_doc->text[at - 1] != '\n'))
            break;
    }
    g_doc_height = y;
    g_laid_for_width = width;
    g_laid_for_len = g_doc->len;
}

static void relayout_if_needed(int width)
{
    if (width != g_laid_for_width || g_doc->len != g_laid_for_len)
        layout(width);
}

/* The line a character index is on. */
static int line_of(long at)
{
    for (int i = 0; i < g_lines; ++i)
        if (at >= g_line[i].start && at <= g_line[i].end)
            return i;
    return g_lines > 0 ? g_lines - 1 : 0;
}

/* Where a character index sits on screen, within the page's frame. */
static void caret_at(long at, int* x, int* y, int* h)
{
    *x = MARGIN; *y = 0; *h = 16;
    if (g_lines == 0)
        return;
    const int l = line_of(at);
    *y = g_line[l].y;
    *h = g_line[l].height;
    int px = MARGIN;
    for (long i = g_line[l].start; i < at && i < g_line[l].end; ++i)
        px += char_width(i);
    *x = px;
}

/* --- the page ---------------------------------------------------------------- */

static void selection_range(long* from, long* to)
{
    *from = g_caret < g_anchor ? g_caret : g_anchor;
    *to   = g_caret < g_anchor ? g_anchor : g_caret;
}

static void draw_page(struct ui_view* v, void* user)
{
    (void)user;
    const struct ui_rect f = v->frame;
    /* The sheet of paper. A document is a page before it is text, and drawing
     * it on the window's own background makes it a text box instead. */
    wg_fill(f.x, f.y, f.w, f.h, WG_PAPER);

    relayout_if_needed(f.w);

    long from, to;
    selection_range(&from, &to);

    for (int l = 0; l < g_lines; ++l) {
        const int y = f.y + g_line[l].y - g_scroll;
        if (y + g_line[l].height < f.y || y > f.y + f.h)
            continue;

        /* Where the line starts, which is what alignment means. */
        int width = 0;
        for (long i = g_line[l].start; i < g_line[l].end; ++i)
            width += char_width(i);
        int x = f.x + MARGIN;
        if (g_doc->align == RTF_CENTRE)
            x = f.x + (f.w - width) / 2;
        else if (g_doc->align == RTF_RIGHT)
            x = f.x + f.w - MARGIN - width;

        /* The selection behind the text, so the text is on top of it. */
        if (to > from && from < g_line[l].end && to > g_line[l].start) {
            int sx = x, sw = 0;
            for (long i = g_line[l].start; i < g_line[l].end; ++i) {
                const int w = char_width(i);
                if (i < from)      sx += w;
                else if (i < to)   sw += w;
            }
            if (to > g_line[l].end && g_line[l].end < g_doc->len)
                sw += 6;        /* the newline, shown as a sliver */
            if (sw > 0)
                wg_fill(sx, y, sw, g_line[l].height, WG_ACCENT);
        }

        /* A run at a time: consecutive characters sharing a style are one call
         * into the toolkit, which is what keeps this from being a glyph lookup
         * per character per frame. */
        long i = g_line[l].start;
        while (i < g_line[l].end) {
            const unsigned char st = g_doc->style[i];
            long j = i;
            while (j < g_line[l].end && g_doc->style[j] == st &&
                   g_doc->text[j] != '\t')
                ++j;
            if (j == i) {                       /* a tab */
                x += char_width(i);
                ++i;
                continue;
            }
            const int px = size_px_of(st);
            /* Sitting on a common baseline, so a big letter and a small one on
             * the same line rest on the same line rather than on their own
             * tops. */
            const int top = y + g_line[l].height - wg_styled_height(px)
                          + (wg_styled_height(px) - px) / 4;
            const int inside = (i >= from && i < to);
            x = wg_styled(x, top, &g_doc->text[i], (int)(j - i),
                          inside ? WG_PAPER : wg_ink_colour(),
                          px, wg_style_of(st));
            i = j;
        }
    }

    /* The caret, when there is no selection to show instead. */
    if (from == to) {
        int cx, cy, ch;
        caret_at(g_caret, &cx, &cy, &ch);
        if (g_doc->align != RTF_LEFT) {
            /* Alignment moves the line, so it moves the caret with it. */
            const int l = line_of(g_caret);
            int width = 0;
            for (long i = g_line[l].start; i < g_line[l].end; ++i)
                width += char_width(i);
            if (g_doc->align == RTF_CENTRE)
                cx += (f.w - width) / 2 - MARGIN;
            else
                cx += f.w - MARGIN - width - MARGIN;
        }
        wg_fill(f.x + cx, f.y + cy - g_scroll + 2, 1, ch - 4, wg_ink_colour());
    }

    wg_scrollbar_v(f.x + f.w - WG_SCROLL_W - 2, f.y, f.h, g_scroll, f.h,
                   g_doc_height);
}

/* Which character a point is nearest. */
static long index_at(int px, int py)
{
    const struct ui_rect f = g_page->frame;
    const int y = py - f.y + g_scroll;
    if (g_lines == 0)
        return 0;
    int l = 0;
    while (l + 1 < g_lines && y >= g_line[l + 1].y)
        ++l;
    int x = f.x + MARGIN;
    for (long i = g_line[l].start; i < g_line[l].end; ++i) {
        const int w = char_width(i);
        if (px < x + w / 2)
            return i;
        x += w;
    }
    return g_line[l].end;
}

/* --- the toolbar ------------------------------------------------------------- */

static void show_state(void)
{
    long from, to;
    selection_range(&from, &to);
    const unsigned char st = (from == to) ? g_typing : rtf_style_at(g_doc, from, to);
    g_bold->on   = (st & RTF_BOLD) != 0;
    g_italic->on = (st & RTF_ITALIC) != 0;
    g_under->on  = (st & RTF_UNDERLINE) != 0;
    g_size->selected = (int)rtf_style_size(st);
    g_align->on = g_doc->align;

    char note[160];
    snprintf(note, sizeof(note), "%s%s - %ld characters",
             g_app.doc_path[0] != '\0' ? g_app.doc_path : "untitled",
             g_app.doc_dirty ? " (edited)" : "", g_doc->len);
    ui_set_text(g_status, note);
}

/* Apply a style change to the selection, or to what is typed next. */
static void apply_flag(unsigned flag, int on)
{
    long from, to;
    selection_range(&from, &to);
    if (from == to) {
        g_typing = on ? (unsigned char)(g_typing | flag)
                      : (unsigned char)(g_typing & ~flag);
    } else {
        rtf_restyle(g_doc, from, to, flag, on);
    app_doc_touched(&g_app);
        g_laid_for_width = -1;      /* widths changed, so the wrap did */
    }
    show_state();
}

static void on_bold(struct ui_view* v, void* u)   { (void)u; apply_flag(RTF_BOLD, v->on); }
static void on_italic(struct ui_view* v, void* u) { (void)u; apply_flag(RTF_ITALIC, v->on); }
static void on_under(struct ui_view* v, void* u)  { (void)u; apply_flag(RTF_UNDERLINE, v->on); }

static const char* size_name(void* user, int i)
{
    (void)user;
    static char text[RTF_SIZES][8];
    if (i < 0 || i >= RTF_SIZES)
        return "";
    snprintf(text[i], sizeof(text[i]), "%d pt", rtf_size_points[i]);
    return text[i];
}

static void on_size(struct ui_view* v, void* u)
{
    (void)u;
    if (v->selected < 0 || v->selected >= RTF_SIZES)
        return;
    long from, to;
    selection_range(&from, &to);
    if (from == to) {
        g_typing = rtf_style_with_size(g_typing, (unsigned)v->selected);
    } else {
        rtf_resize(g_doc, from, to, (unsigned)v->selected);
    app_doc_touched(&g_app);
    }
    g_laid_for_width = -1;
    show_state();
}

static const char* align_name(void* user, int i)
{
    (void)user;
    return i == RTF_LEFT ? "Left" : i == RTF_CENTRE ? "Centre" : "Right";
}

static void on_align(struct ui_view* v, void* u)
{
    (void)u;
    g_doc->align = v->on;
    app_doc_touched(&g_app);
    show_state();
}

/* --- files -------------------------------------------------------------------- */

static int doc_load(struct app* a, const char* path);
static int doc_save(struct app* a, const char* path);
static void doc_new(struct app* a);

/* Opening, saving and starting again all go through the framework: see the
 * doc_ fields in main. What is left here is the two ways a person asks for
 * them. */
static void on_save(struct ui_view* v, void* u)
{
    (void)v; (void)u;
    app_doc_save(&g_app);
}

/* --- keys and clicks ----------------------------------------------------------- */

static void reveal_caret(void)
{
    if (g_page == 0 || g_lines == 0)
        return;
    const int l = line_of(g_caret);
    const int top = g_line[l].y, bottom = top + g_line[l].height;
    if (top < g_scroll)
        g_scroll = top;
    else if (bottom > g_scroll + g_page->frame.h)
        g_scroll = bottom - g_page->frame.h;
    if (g_scroll < 0)
        g_scroll = 0;
}

static void delete_selection(void)
{
    long from, to;
    selection_range(&from, &to);
    if (to > from) {
        rtf_delete(g_doc, from, to - from);
        g_caret = g_anchor = from;
    app_doc_touched(&g_app);
    }
}

static int on_key(unsigned key)
{
    long from, to;
    selection_range(&from, &to);

    if (key == WIN_KEY_LEFT)  { if (g_caret > 0) --g_caret; g_anchor = g_caret; }
    else if (key == WIN_KEY_RIGHT) { if (g_caret < g_doc->len) ++g_caret; g_anchor = g_caret; }
    else if (key == WIN_KEY_UP || key == WIN_KEY_DOWN) {
        const int l = line_of(g_caret);
        const int want = l + (key == WIN_KEY_DOWN ? 1 : -1);
        if (want >= 0 && want < g_lines) {
            /* The same distance across, not the same index: lines are not the
             * same length and an index would wander. */
            int cx, cy, ch;
            caret_at(g_caret, &cx, &cy, &ch);
            int x = MARGIN;
            long at = g_line[want].start;
            while (at < g_line[want].end && x + char_width(at) / 2 < cx) {
                x += char_width(at);
                ++at;
            }
            g_caret = g_anchor = at;
        }
    }
    else if (key == '\b' || key == 0x7F) {
        if (to > from) delete_selection();
        else if (g_caret > 0) {
            rtf_delete(g_doc, g_caret - 1, 1);
            --g_caret;
            g_anchor = g_caret;
    app_doc_touched(&g_app);
        }
    }
    else if (key == 2)  { apply_flag(RTF_BOLD, !g_bold->on); return 1; }      /* ctrl+B */
    else if (key == 9)  { apply_flag(RTF_ITALIC, !g_italic->on); return 1; }  /* ctrl+I */
    else if (key == 21) { apply_flag(RTF_UNDERLINE, !g_under->on); return 1; }/* ctrl+U */
    else if (key == 19) { on_save(0, 0); return 1; }                          /* ctrl+S */
    else if (key == 1)  { g_anchor = 0; g_caret = g_doc->len; }               /* ctrl+A */
    else if (key == '\n' || key == '\r' || key == '\t' ||
             (key >= ' ' && key < 127)) {
        delete_selection();
        const char c = (key == '\r') ? '\n' : (char)key;
        if (rtf_insert(g_doc, g_caret, &c, 1, g_typing) == 0) {
            ++g_caret;
            g_anchor = g_caret;
    app_doc_touched(&g_app);
        }
    }
    else return 0;

    if (from == to || g_caret != from)
        g_typing = rtf_style_at(g_doc, g_caret, g_caret);
    g_laid_for_width = -1;
    relayout_if_needed(g_page->frame.w);
    reveal_caret();
    show_state();
    return 1;
}

static int on_event(struct app* a, const struct win_event* e)
{
    if (a->handled && e->type == WIN_EVENT_MOUSE_DOWN)
        return 0;
    const struct ui_rect f = g_page->frame;
    const int inside = e->x >= f.x && e->y >= f.y &&
                       e->x < f.x + f.w && e->y < f.y + f.h;

    if (e->type == WIN_EVENT_MOUSE_DOWN && inside) {
        if (e->x >= f.x + f.w - WG_SCROLL_W - 2) {
            g_scroll = wg_scroll_hit_v(e->x, e->y, f.x + f.w - WG_SCROLL_W - 2,
                                       f.y, f.h, g_scroll, f.h, g_doc_height);
            return 1;
        }
        g_caret = g_anchor = index_at(e->x, e->y);
        g_selecting = 1;
        g_typing = rtf_style_at(g_doc, g_caret, g_caret);
        show_state();
        return 1;
    }
    if (e->type == WIN_EVENT_MOUSE_MOVE && g_selecting) {
        g_caret = index_at(e->x, e->y);
        show_state();
        return 1;
    }
    if (e->type == WIN_EVENT_MOUSE_UP) {
        g_selecting = 0;
        return 0;
    }
    if (e->type == WIN_EVENT_KEY)
        return on_key(e->key);
    return 0;
}

static int on_menu(struct app* a, int pick)
{
    if (pick == 0)      app_doc_new(a);
    else if (pick == 1) app_doc_open(a);
    else if (pick == 2) app_doc_save(a);
    show_state();
    return 1;
}

static const char* const kMenu[] = { "New", "Open...", "Save" };

int main(int argc, char** argv)
{
    g_doc = rtf_new();
    if (g_doc == 0) {
        printf("write: out of memory\n");
        return 1;
    }
    g_typing = (unsigned char)(RTF_SIZE_DEFAULT << RTF_SIZE_SHIFT);

    struct ui_view* root = ui_box(0, UI_STACK_V, 0, 0);

    struct ui_view* bar = ui_box(root, UI_STACK_H, 8, 8);
    ui_grow(ui_size(bar, 0, 38), 0);
    /* Wide enough for the switch and the whole word. They were sized by eye
     * and came out as "Bo.." and "Und..", which is a label that has to be
     * guessed at. */
    g_bold   = ui_grow(ui_size(ui_toggle(bar, "Bold", 0), 92, 24), 0);
    g_italic = ui_grow(ui_size(ui_toggle(bar, "Italic", 0), 92, 24), 0);
    g_under  = ui_grow(ui_size(ui_toggle(bar, "Underline", 0), 124, 24), 0);
    ui_on(g_bold, on_bold, 0);
    ui_on(g_italic, on_italic, 0);
    ui_on(g_under, on_under, 0);

    g_size = ui_grow(ui_size(ui_popup(bar, size_name, RTF_SIZES, 0), 84, 24), 0);
    /* Showing what a new document is actually set to. A popup starts at its
     * first item, which here is the smallest size there is - so an empty
     * document claimed to be eight point while typing came out at twelve. */
    g_size->selected = RTF_SIZE_DEFAULT;
    ui_on(g_size, on_size, 0);

    g_align = ui_grow(ui_size(ui_segmented(bar, align_name, 3, 0), 168, 24), 0);
    ui_on(g_align, on_align, 0);

    ui_spacer(bar);
    ui_grow(ui_size(ui_button(bar, "Save", on_save, 0), 64, 24), 0);

    g_page = ui_grow(ui_custom(root, draw_page, 0), 1);

    struct ui_view* foot = ui_box(root, UI_STACK_H, 6, 0);
    ui_grow(ui_size(foot, 0, 26), 0);
    g_status = ui_grow(ui_label(foot, ""), 1);

    /* A document named on the command line, after the position - which is how
     * every application here is launched with something to open. */
    const char* open_this = 0;
    for (int i = 1; i < argc; ++i)
        if (argv[i][0] != '\0' && (argv[i][0] < '0' || argv[i][0] > '9'))
            open_this = argv[i];

    g_app.title = "Write";
    g_app.width = 720; g_app.height = 520;
    /* Wide enough for the toolbar to fit on one line. */
    g_app.min_width = 620; g_app.min_height = 320;
    g_app.root = root;
    g_app.event = on_event;
    g_app.menu = kMenu;
    g_app.menu_count = (int)(sizeof(kMenu) / sizeof(kMenu[0]));
    g_app.menu_pick = on_menu;
    g_app.doc_kind = "document";
    g_app.doc_dir = "/root/Documents";
    g_app.doc_suggested = "document.rtf";
    g_app.doc_save = doc_save;
    g_app.doc_load = doc_load;
    g_app.doc_new = doc_new;

    if (open_this != 0 && doc_load(&g_app, open_this) == 0)
        snprintf(g_app.doc_path, sizeof(g_app.doc_path), "%s", open_this);
    show_state();
    return app_run(&g_app, argc, argv);
}

/* The document, as the framework sees it. Write kept its own path and its own
 * edited flag and its own save; all three are the framework's now, which is
 * what gets it the question before work is thrown away. */
static int doc_load(struct app* a, const char* path)
{
    (void)a;
    struct rtf_doc* fresh = rtf_read(path);
    if (fresh == 0)
        return -1;
    rtf_free(g_doc);
    g_doc = fresh;
    g_caret = g_anchor = 0;
    g_scroll = 0;
    g_laid_for_width = -1;
    g_typing = rtf_style_at(g_doc, 0, 0);
    show_state();
    return 0;
}

static int doc_save(struct app* a, const char* path)
{
    (void)a;
    return rtf_write(path, g_doc);
}

static void doc_new(struct app* a)
{
    (void)a;
    rtf_free(g_doc);
    g_doc = rtf_new();
    g_caret = g_anchor = 0;
    g_scroll = 0;
    g_laid_for_width = -1;
    show_state();
}
