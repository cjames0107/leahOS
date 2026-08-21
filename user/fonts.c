/* Fonts - what the system can draw, and what it looks like.
 *
 * Two questions, and neither has an answer anywhere else. Which typefaces are
 * installed is a directory listing, which is not the same as being able to see
 * them; and whether a particular character exists is something every program
 * finds out by drawing a blank where it wanted a glyph. So: the faces on one
 * side, a specimen at every size the system uses, and the characters
 * themselves - the last of which is also the only way to type one that is not
 * on the keyboard.
 */

#include <app.h>
#include <ui.h>
#include <clipboard.h>
#include <font.h>
#include <paths.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <widget.h>
#include <window.h>

#define MAX_FACES 32

static struct app g_app;
static struct ui_view* g_list;
static struct ui_view* g_tabs;
static struct ui_view* g_pane;
static struct ui_view* g_note;

static char g_face_name[MAX_FACES][64];
static char g_face_path[MAX_FACES][192];
static int  g_faces;
static int  g_chosen;

/* The block of characters on show. Latin-1 and the Latin Extended-A that
 * follows it is what this system's text is; a grid of every codepoint would be
 * a grid of mostly nothing.
 *
 * The thirty-three control codes between the two halves are skipped rather
 * than shown as empty boxes. They are not characters - there is nothing to
 * copy and nothing to look at - and a third of a screen of blanks in the
 * middle of the grid reads as a font that is missing them. */
#define FIRST_CHAR 32
#define LAST_CHAR  0x17F
#define GAP_FIRST  0x7F
#define GAP_LAST   0x9F
static int g_first;      /* an index into the block, not a codepoint */

/* The nth character on show, skipping the gap. */
static int char_at(int n)
{
    const int code = FIRST_CHAR + n;
    return code < GAP_FIRST ? code : code + (GAP_LAST - GAP_FIRST + 1);
}
#define CHAR_COUNT (LAST_CHAR - FIRST_CHAR - (GAP_LAST - GAP_FIRST))
static char g_message[160];

static void say(const char* text)
{
    snprintf(g_message, sizeof(g_message), "%s", text);
    ui_set_text(g_note, g_message);
}

static void find_faces(void)
{
    struct dirent here[64];
    const int n = getdents(PATH_FONTS, here, 64);
    for (int i = 0; i < n && g_faces < MAX_FACES; ++i) {
        const char* name = here[i].d_name;
        const unsigned len = (unsigned)strlen(name);
        if (len < 5 || strcmp(&name[len - 4], ".ttf") != 0)
            continue;
        snprintf(g_face_path[g_faces], sizeof(g_face_path[0]),
                 "%s/%s", PATH_FONTS, name);
        /* The file's name without its extension. A face's own name lives in a
         * table this rasteriser does not read, and inventing one would be a
         * guess presented as a fact. */
        snprintf(g_face_name[g_faces], sizeof(g_face_name[0]), "%.*s",
                 (int)len - 4, name);
        ++g_faces;
    }
}

static const char* face_row(void* user, int row)
{
    (void)user;
    return (row >= 0 && row < g_faces) ? g_face_name[row] : "";
}

static const char* tab_title(void* user, int i)
{
    (void)user;
    return i == 0 ? "Specimen" : "Characters";
}

/* --- the specimen --------------------------------------------------------- */

static const char* const kSpecimen =
    "Sphinx of black quartz, judge my vow.";

static void draw_specimen(struct ui_rect f)
{
    static const int kSizes[] = { 10, 12, 14, 18, 24, 32, 48 };
    int y = f.y + 10;
    for (unsigned i = 0; i < sizeof(kSizes) / sizeof(kSizes[0]); ++i) {
        const int px = kSizes[i] * 4 / 3;
        const int h = wg_styled_height(px);
        if (y + h > f.y + f.h)
            break;
        char label[16];
        snprintf(label, sizeof(label), "%d pt", kSizes[i]);
        wg_text(f.x + 10, y + (h - WG_GLYPH_H) / 2, label, WG_DIM);
        wg_styled(f.x + 64, y, kSpecimen, (int)strlen(kSpecimen),
                  wg_ink_colour(), px, 0);
        y += h + 4;
    }

    /* And the three effects, at one size, because a system with one typeface
     * makes these rather than loading them and it should be plain that it
     * does. */
    if (y + 40 < f.y + f.h) {
        const int px = 18;
        y += 8;
        wg_fill(f.x + 10, y, f.w - 20, 1, WG_DIM);
        y += 10;
        int x = f.x + 64;
        x = wg_styled(x, y, "Regular  ", 9, wg_ink_colour(), px, 0);
        x = wg_styled(x, y, "Bold  ", 6, wg_ink_colour(), px, WG_STYLE_BOLD);
        x = wg_styled(x, y, "Italic  ", 8, wg_ink_colour(), px, WG_STYLE_ITALIC);
        wg_styled(x, y, "Underline", 9, wg_ink_colour(), px, WG_STYLE_UNDERLINE);
        wg_text(f.x + 10, y + 2, "styles", WG_DIM);
    }
}

/* --- the characters -------------------------------------------------------- */

#define CELL 34

static int grid_cols(struct ui_rect f) { return (f.w - 20) / CELL; }
static int grid_rows(struct ui_rect f) { return (f.h - 20) / CELL; }

static void draw_characters(struct ui_rect f)
{
    const int cols = grid_cols(f), rows = grid_rows(f);
    if (cols <= 0 || rows <= 0)
        return;
    struct font* face = font_open(g_face_path[g_chosen]);

    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const int index = g_first + r * cols + c;
            if (index >= CHAR_COUNT)
                break;
            const int code = char_at(index);
            const int x = f.x + 10 + c * CELL, y = f.y + 10 + r * CELL;
            wg_glass_outline(x, y, CELL - 2, CELL - 2, 4, 1,
                             wg_glass_on() ? 0x30FFFFFFu : 0x18000000u);
            /* The codepoint as UTF-8, which is what wg_styled reads. */
            char utf8[4];
            int n = 0;
            if (code < 0x80) {
                utf8[n++] = (char)code;
            } else {
                utf8[n++] = (char)(0xC0 | (code >> 6));
                utf8[n++] = (char)(0x80 | (code & 0x3F));
            }
            const int w = wg_styled_width(utf8, n, 20, 0);
            wg_styled(x + (CELL - 2 - w) / 2, y + 3, utf8, n,
                      wg_ink_colour(), 20, 0);
        }
    }
    if (face != 0)
        font_close(face);

    char note[96];
    const int last = g_first + rows * cols - 1;
    snprintf(note, sizeof(note), "U+%04X to U+%04X - click one to copy it",
             char_at(g_first),
             char_at(last < CHAR_COUNT - 1 ? last : CHAR_COUNT - 1));
    wg_text(f.x + 10, f.y + f.h - WG_GLYPH_H - 4, note, WG_DIM);
}

static void draw_pane(struct ui_view* v, void* user)
{
    (void)user;
    const struct ui_rect f = v->frame;
    wg_container(f.x, f.y, f.w, f.h, WG_RADIUS);
    if (g_faces == 0) {
        wg_text(f.x + 12, f.y + 12, "no typefaces are installed", WG_DIM);
        return;
    }
    if (g_tabs->on == 0)
        draw_specimen(f);
    else
        draw_characters(f);
}

/* --- events ---------------------------------------------------------------- */

static void on_face(struct ui_view* v, void* u)
{
    (void)u;
    if (v->selected >= 0 && v->selected < g_faces) {
        g_chosen = v->selected;
        say(g_face_path[g_chosen]);
    }
}

static void on_tab(struct ui_view* v, void* u)
{
    (void)v; (void)u;
    say(g_tabs->on == 0 ? "every size this system draws at"
                        : "click a character to put it on the clipboard");
}

static int on_event(struct app* a, const struct win_event* e)
{
    if (a->handled && e->type == WIN_EVENT_MOUSE_DOWN)
        return 0;
    const struct ui_rect f = g_pane->frame;

    if (e->type == WIN_EVENT_KEY && g_tabs->on == 1) {
        const int page = grid_cols(f) * grid_rows(f);
        if (e->key == WIN_KEY_DOWN)      g_first += grid_cols(f);
        else if (e->key == WIN_KEY_UP)   g_first -= grid_cols(f);
        else if (e->key == WIN_KEY_RIGHT) g_first += page;
        else if (e->key == WIN_KEY_LEFT)  g_first -= page;
        else return 0;
        if (g_first > CHAR_COUNT - page) g_first = CHAR_COUNT - page;
        if (g_first < 0) g_first = 0;
        return 1;
    }

    if (e->type == WIN_EVENT_MOUSE_DOWN && g_tabs->on == 1 &&
        e->x >= f.x && e->y >= f.y && e->x < f.x + f.w && e->y < f.y + f.h) {
        const int cols = grid_cols(f);
        const int c = (e->x - f.x - 10) / CELL;
        const int r = (e->y - f.y - 10) / CELL;
        if (c < 0 || c >= cols || r < 0)
            return 0;
        const int index = g_first + r * cols + c;
        if (index >= CHAR_COUNT)
            return 0;
        const int code = char_at(index);
        char utf8[4];
        int n = 0;
        if (code < 0x80) utf8[n++] = (char)code;
        else { utf8[n++] = (char)(0xC0 | (code >> 6));
               utf8[n++] = (char)(0x80 | (code & 0x3F)); }
        utf8[n] = '\0';
        clip_put(utf8, (unsigned)n);
        char note[64];
        snprintf(note, sizeof(note), "U+%04X copied", code);
        say(note);
        return 1;
    }
    return 0;
}

int main(int argc, char** argv)
{
    find_faces();

    struct ui_view* root = ui_box(0, UI_STACK_H, 0, 0);

    g_list = ui_sidebar(root, face_row, g_faces, 0);
    ui_size(g_list, 168, 0);
    ui_grow(g_list, 0);
    g_list->selected = 0;
    ui_on(g_list, on_face, 0);

    struct ui_view* right = ui_box(root, UI_STACK_V, 10, 8);
    ui_grow(right, 1);
    g_tabs = ui_grow(ui_size(ui_tabs(right, tab_title, 2, 0), 0, 26), 0);
    ui_on(g_tabs, on_tab, 0);
    g_pane = ui_grow(ui_custom(right, draw_pane, 0), 1);
    g_note = ui_grow(ui_size(ui_label(right, ""), 0, 18), 0);

    g_app.title = "Fonts";
    g_app.width = 700; g_app.height = 480;
    g_app.min_width = 540; g_app.min_height = 340;
    g_app.sidebar = 168;
    g_app.root = root;
    g_app.event = on_event;
    say(g_faces > 0 ? g_face_path[0] : "no typefaces are installed");
    return app_run(&g_app, argc, argv);
}
