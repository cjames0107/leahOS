/* Elements - every component the system has, in one window.
 *
 * This used to demonstrate that there was no toolkit: it drew its own bevelled
 * rectangles and hit-tested them by hand, and said so, because that was honest
 * about what a client was. There is a toolkit now, so what is worth showing is
 * each component doing its real job, wired to something that visibly reacts.
 *
 * There are more components than fit in one window, so they are on pages - and
 * the pages are themselves a component doing its job. Every page is built at
 * startup and all but one is hidden, which is what UI_HIDDEN is for: rebuilding
 * a page on every switch would mean the sliders forgot where they were.
 *
 * It is also the smallest complete example of writing an application here.
 * There is no event loop, no hit-testing, no layout arithmetic and no redraw
 * bookkeeping in this file. What is here is what the window contains and what
 * happens when it is used.
 */

#include <app.h>
#include <icon.h>
#include <stdio.h>
#include <string.h>
#include <ui.h>

#define PAGES 5

static char g_said[96] = "nothing yet";
static struct ui_view* g_report;
static struct ui_view* g_progress;
static struct ui_view* g_pages[PAGES];
static int g_page;

static void say(const char* what)
{
    snprintf(g_said, sizeof(g_said), "%s", what);
    ui_set_text(g_report, g_said);
}

/* --- the pages ------------------------------------------------------------- */

static const char* const kPageNames[PAGES] = {
    "Controls", "Text", "Lists", "Layout", "More"
};
static const char* page_name(void* user, int row)
{
    (void)user;
    return (row >= 0 && row < PAGES) ? kPageNames[row] : "";
}

static void on_page(struct ui_view* v, void* user)
{
    (void)user;
    g_page = v->on;
    for (int i = 0; i < PAGES; ++i) {
        if (g_pages[i] == 0)
            continue;
        if (i == g_page) g_pages[i]->flags &= ~UI_HIDDEN;
        else             g_pages[i]->flags |= UI_HIDDEN;
    }
    /* The tree's shape changed - a hidden view takes no room - so it is laid
     * out again before anything is drawn or hit. */
    app_relayout((struct app*)user);
    say(kPageNames[g_page]);
}

/* --- what the controls do -------------------------------------------------- */

static void on_click(struct ui_view* v, void* user)
{
    (void)user;
    static int count;
    ++count;
    char line[96];
    snprintf(line, sizeof(line), "%s pressed, %d time%s", ui_text(v), count,
             count == 1 ? "" : "s");
    say(line);
}

static void on_check(struct ui_view* v, void* user)
{
    (void)user;
    char line[96];
    snprintf(line, sizeof(line), "%s is now %s", ui_text(v),
             v->on ? "on" : "off");
    say(line);
}

static void on_slide(struct ui_view* v, void* user)
{
    (void)user;
    char line[96];
    snprintf(line, sizeof(line), "slider at %d", v->value);
    say(line);
    if (g_progress != 0)
        g_progress->value = v->value;   /* two components wired together */
}

static void on_step(struct ui_view* v, void* user)
{
    (void)user;
    char line[96];
    snprintf(line, sizeof(line), "stepper at %d", v->value);
    say(line);
}

static void on_text(struct ui_view* v, void* user)
{
    (void)user;
    char line[96];
    snprintf(line, sizeof(line), "field says \"%s\"", ui_text(v));
    say(line);
}

static const char* const kSizes[] = { "Small", "Medium", "Large", "Huge" };
static const char* size_row(void* user, int row)
{
    (void)user;
    return (row >= 0 && row < 4) ? kSizes[row] : "";
}

static void on_popup(struct ui_view* v, void* user)
{
    (void)user;
    char line[96];
    snprintf(line, sizeof(line), "chose %s", size_row(0, v->selected));
    say(line);
}

/* A menu bar's items, for every menu, keyed as menu * 100 + item. */
static const char* menu_item(void* user, int key)
{
    (void)user;
    static const char* const kFile[] = { "New", "Open", "Save" };
    static const char* const kEdit[] = { "Cut", "Copy", "Paste", "Undo" };
    const int menu = key / 100, item = key % 100;
    if (menu == 0)
        return (item >= 0 && item < 3) ? kFile[item] : "";
    if (menu == 1)
        return (item >= 0 && item < 4) ? kEdit[item] : "";
    return "";
}
static const int kMenuCounts[2] = { 3, 4 };
static const char* menu_title(void* user, int row)
{
    (void)user;
    return row == 0 ? "File" : row == 1 ? "Edit" : "";
}

static void on_menu(struct ui_view* v, void* user)
{
    (void)user;
    char line[96];
    snprintf(line, sizeof(line), "menu: %s",
             menu_item(0, v->selected));
    say(line);
}

/* --- data for the list-shaped components ----------------------------------- */

static const char* const kFruit[] = { "apples", "pears", "quinces", "figs",
                                      "medlars", "sloes" };
static const char* fruit_row(void* user, int row)
{
    (void)user;
    return (row >= 0 && row < 6) ? kFruit[row] : "";
}

static void on_pick(struct ui_view* v, void* user)
{
    (void)user;
    char line[96];
    snprintf(line, sizeof(line), "picked %s", fruit_row(0, v->selected));
    say(line);
}

/* A table's cells, by row and column. */
static const char* table_cell(void* user, int row, int col)
{
    (void)user;
    static char scratch[32];
    static const char* const kKind[] = { "fruit", "fruit", "fruit",
                                         "fruit", "fruit", "fruit" };
    if (row < 0 || row >= 6)
        return "";
    if (col == 0) return kFruit[row];
    if (col == 1) { snprintf(scratch, sizeof(scratch), "%d", (row + 1) * 12);
                    return scratch; }
    return kKind[row];
}

/* A tree, flattened - which is the application's job, because only it knows
 * what opening a row reveals. */
static struct { const char* name; int depth; int branch; } g_tree[] = {
    { "Documents", 0, 2 },
    { "notes.txt", 1, 0 },
    { "report.md", 1, 0 },
    { "Pictures",  0, 1 },
    { "Music",     0, 1 },
};
static const char* tree_row(void* user, int row)
{
    (void)user;
    return (row >= 0 && row < 5) ? g_tree[row].name : "";
}
static int tree_depth(void* user, int row)
{
    (void)user;
    return (row >= 0 && row < 5) ? g_tree[row].depth : 0;
}
static int tree_branch(void* user, int row)
{
    (void)user;
    return (row >= 0 && row < 5) ? g_tree[row].branch : 0;
}

static void on_tree(struct ui_view* v, void* user)
{
    (void)user;
    char line[96];
    if (v->hit_branch && v->selected >= 0 && v->selected < 5) {
        /* The twisty rather than the row: the component tells them apart, and
         * this is what an application does with that. */
        g_tree[v->selected].branch = g_tree[v->selected].branch == 2 ? 1 : 2;
        snprintf(line, sizeof(line), "%s %s", tree_row(0, v->selected),
                 g_tree[v->selected].branch == 2 ? "opened" : "closed");
    } else {
        snprintf(line, sizeof(line), "selected %s", tree_row(0, v->selected));
    }
    say(line);
}

static const uint32_t* grid_icon(void* user, int row)
{
    (void)user; (void)row;
    return icon_by_name("folder-populated");
}

static char g_doc[512] =
    "A text area over a buffer the application owns.\n"
    "It has lines, a caret, and arrow keys that move by\n"
    "line and by character.\n\n"
    "Type into it.";

/* A view drawn by the application, for what no component covers. */
static void draw_swatches(struct ui_view* v, void* user)
{
    (void)user;
    static const uint32_t kColours[6] = {
        0xC0392B, 0xD68910, 0xF1C40F, 0x27AE60, 0x2980B9, 0x8E44AD
    };
    const int each = v->frame.w / 6;
    for (int i = 0; i < 6; ++i)
        wg_fill(v->frame.x + i * each, v->frame.y, each - 2, v->frame.h,
                kColours[i]);
}

/* --- building each page ---------------------------------------------------- */

static void build_controls(struct ui_view* page)
{
    struct ui_view* row = ui_box(page, UI_STACK_H, 0, 8);
    ui_size(row, 0, 26); ui_grow(row, 0);
    ui_button(row, "Press me", on_click, 0);
    ui_button(row, "And me", on_click, 0);
    struct ui_view* pick = ui_popup(row, size_row, 4, 0);
    ui_on(pick, on_popup, 0);
    ui_grow(pick, 0);
    ui_spacer(row);

    ui_grow(ui_separator(page), 0);

    struct ui_view* boxes = ui_box(page, UI_STACK_H, 0, 16);
    ui_size(boxes, 0, 22); ui_grow(boxes, 0);
    ui_on(ui_check(boxes, "Enabled", 1), on_check, 0);
    ui_on(ui_radio(boxes, "First", 1), on_check, 0);
    ui_on(ui_radio(boxes, "Second", 0), on_check, 0);
    ui_spacer(boxes);

    struct ui_view* switches = ui_box(page, UI_STACK_H, 0, 16);
    ui_size(switches, 0, 24); ui_grow(switches, 0);
    ui_on(ui_toggle(switches, "Glass", 1), on_check, 0);
    ui_on(ui_stepper(switches, 3, 10), on_step, 0);
    ui_spacer(switches);

    ui_grow(ui_separator(page), 0);

    struct ui_view* slider = ui_slider(page, 40, 100);
    ui_on(slider, on_slide, 0);
    ui_grow(slider, 0);
    g_progress = ui_progress(page, 40, 100);
    ui_grow(g_progress, 0);

    struct ui_view* pics = ui_box(page, UI_STACK_H, 0, 10);
    ui_size(pics, 0, 32); ui_grow(pics, 0);
    ui_image(pics, icon_by_name("folder-populated"), 32);
    ui_image(pics, icon_by_name("terminal"), 32);
    ui_image(pics, icon_by_name("edit"), 32);
    ui_spacer(pics);

    struct ui_view* sw = ui_custom(page, draw_swatches, 0);
    ui_size(sw, 0, 24); ui_grow(sw, 0);
    ui_spacer(page);
}

static void build_text(struct ui_view* page)
{
    struct ui_view* f = ui_field(page, "an editable field");
    ui_on(f, on_text, 0);
    ui_grow(f, 0);
    ui_grow(ui_label(page, "A text area, over a buffer this program owns:"), 0);
    ui_text_area(page, g_doc, (int)sizeof(g_doc));
}

static void build_lists(struct ui_view* page)
{
    struct ui_view* tabs = ui_tabs(page, size_row, 4, 0);
    ui_grow(tabs, 0);

    struct ui_view* table = ui_table(page, table_cell, 6, 0);
    ui_column(table, "Name", 140);
    ui_column(table, "Size", 80);
    ui_column(table, "Kind", 100);
    ui_on(table, on_pick, 0);

    struct ui_view* side = ui_box(page, UI_STACK_H, 0, 10);
    struct ui_view* tree = ui_tree(side, tree_row, 5, tree_depth, tree_branch, 0);
    ui_on(tree, on_tree, 0);
    struct ui_view* grid = ui_icongrid(side, fruit_row, 6, grid_icon, 0);
    ui_on(grid, on_pick, 0);
}

static void build_layout(struct ui_view* page)
{
    ui_grow(ui_label(page, "A split, whose divider drags:"), 0);
    struct ui_view* split = ui_split(page, UI_STACK_H, 180);
    struct ui_view* left = ui_group(split, "Group", UI_STACK_V, 10, 6);
    ui_grow(ui_label(left, "A titled box"), 0);
    ui_grow(ui_check(left, "with things", 1), 0);
    ui_grow(ui_check(left, "inside it", 0), 0);
    ui_spacer(left);

    struct ui_view* right = ui_scroll(split);
    struct ui_view* tall = ui_box(right, UI_STACK_V, 10, 6);
    ui_size(tall, 0, 400);              /* taller than the frame, so it scrolls */
    for (int i = 0; i < 12; ++i) {
        char line[32];
        snprintf(line, sizeof(line), "scrolling row %d", i + 1);
        ui_grow(ui_label(tall, line), 0);
    }
}

/* --- the third set, shown ------------------------------------------------- */

static struct ui_view* g_spinner;
static struct ui_view* g_over;

static void on_level(struct ui_view* v, void* user)
{
    (void)user;
    char line[96];
    snprintf(line, sizeof(line), "level %d", v->value);
    say(line);
}

static void on_colour(struct ui_view* v, void* user)
{
    (void)user;
    char line[96];
    snprintf(line, sizeof(line), "colour %06x", (unsigned)v->value);
    say(line);
}

static void on_combo(struct ui_view* v, void* user)
{
    (void)user;
    char line[96];
    if (v->selected < 0)
        snprintf(line, sizeof(line), "%s pressed", ui_text(v));
    else
        snprintf(line, sizeof(line), "chose item %d", v->selected);
    say(line);
}

static void on_find(struct ui_view* v, void* user)
{
    (void)user;
    char line[96];
    snprintf(line, sizeof(line), "searching for \"%s\"", ui_text(v));
    say(line);
}

static void on_over(struct ui_view* v, void* user)
{
    (void)v; (void)user;
    ui_popover_show(g_over, 1);
    say("popover shown - press outside it");
}

static void on_day(struct ui_view* v, void* user)
{
    (void)user;
    char line[96];
    snprintf(line, sizeof(line), "chose %d/%d/%d", v->day, v->month + 1,
             v->year);
    say(line);
}

/* A little tree for the browser: three groups, each with a few members. */
static int more_cols(void* user, int column, const int* chosen)
{
    (void)user;
    if (column == 0) return 3;
    if (column == 1) return chosen[0] >= 0 ? 4 : 0;
    return chosen[1] >= 0 ? 2 : 0;
}

static const char* more_coltext(void* user, int column, int row,
                                const int* chosen)
{
    (void)user;
    static const char* const kTop[3] = { "Fruit", "Trees", "Birds" };
    static char text[40];
    if (column == 0)
        return (row >= 0 && row < 3) ? kTop[row] : "";
    if (column == 1) {
        snprintf(text, sizeof(text), "%s %d",
                 chosen[0] >= 0 ? kTop[chosen[0]] : "?", row + 1);
        return text;
    }
    snprintf(text, sizeof(text), "detail %d", row + 1);
    return text;
}

static int on_tick(struct app* app)
{
    (void)app;
    /* Only when the page showing it is the one on screen: a spinner turning
     * behind four hidden pages is a repaint per tick for nothing. */
    if (g_page != 4 || g_spinner == 0)
        return 0;
    ui_spin(g_spinner);
    return 1;
}

static void build_more(struct ui_view* page)
{
    struct ui_view* row = ui_box(page, UI_STACK_H, 0, 10);
    ui_size(row, 0, 26); ui_grow(row, 0);
    ui_on(ui_search(row, "Search"), on_find, 0);
    ui_grow(ui_secure(row), 0);
    ui_on(ui_combo(row, "Send", size_row, 4, 0), on_combo, 0);

    struct ui_view* row2 = ui_box(page, UI_STACK_H, 0, 12);
    ui_size(row2, 0, 26); ui_grow(row2, 0);
    ui_on(ui_colour(row2, 0x2C6BED), on_colour, 0);
    ui_on(ui_level(row2, 7, 10, 0), on_level, 0);       /* continuous */
    ui_on(ui_level(row2, 3, 5, 5), on_level, 0);        /* discrete */
    g_spinner = ui_spinner(row2);
    ui_grow(g_spinner, 0);
    struct ui_view* over_button = ui_button(row2, "Popover", on_over, 0);
    ui_grow(over_button, 0);
    ui_spacer(row2);

    /* The popover hangs off the button and takes no room until it is shown. */
    g_over = ui_popover(page, over_button);
    ui_size(g_over, 220, 90);
    ui_label(g_over, "Transient content,");
    ui_label(g_over, "beside what raised it.");

    struct ui_view* split = ui_split(page, UI_STACK_H, 330);
    ui_browser(split, 3, more_cols, more_coltext, 0);
    struct ui_view* right = ui_box(split, UI_STACK_V, 8, 4);
    ui_on(ui_calendar(right, 2026, 7, 20), on_day, 0);
}

int main(int argc, char** argv)
{
    static struct app a;

    /* The whole interface, as the shape it makes. Every frame on screen is
     * computed from this by the layout pass, so no coordinate appears twice
     * and none of them can disagree. */
    struct ui_view* root = ui_box(0, UI_STACK_V, 0, 0);

    struct ui_view* bar = ui_menubar(root, menu_title, 2, menu_item,
                                     kMenuCounts, 0);
    ui_on(bar, on_menu, 0);
    ui_grow(bar, 0);

    struct ui_view* chooser = ui_segmented(root, page_name, PAGES, 0);
    ui_on(chooser, on_page, &a);
    ui_size(chooser, 0, 26);
    ui_grow(chooser, 0);

    for (int i = 0; i < PAGES; ++i) {
        g_pages[i] = ui_box(root, UI_STACK_V, 14, 8);
        if (i != 0)
            g_pages[i]->flags |= UI_HIDDEN;
    }
    build_controls(g_pages[0]);
    build_text(g_pages[1]);
    build_lists(g_pages[2]);
    build_layout(g_pages[3]);
    build_more(g_pages[4]);

    g_report = ui_label(root, g_said);
    ui_size(g_report, 0, 22);
    ui_grow(g_report, 0);

    a.tick_ms = 90;             /* the spinner has to turn to mean anything */
    a.tick = on_tick;
    a.title = "Elements";
    a.width = 700; a.height = 520;
    a.min_width = 560; a.min_height = 420;
    a.root = root;
    return app_run(&a, argc, argv);
}
