#ifndef _UI_H
#define _UI_H

#include <widget.h>
#include <window.h>

/* Components, laid out and routed - rather than pixels and hit-tests.
 *
 * widget.h paints. wg_button draws a button and then forgets it: it returns
 * nothing, remembers nothing, and never says it was clicked. Every application
 * therefore kept its own rectangle for every control, hit-tested it by hand in
 * a chain of if/else, and tracked which one had the keyboard in an integer of
 * its own. Seven applications declared their own `struct box` and their own
 * `inside()`; three kept their own focus flag; every one of them wrote the same
 * backspace-and-insert loop to make a text field editable.
 *
 * Worse than the repetition is what it costs to get right. A control's
 * geometry had to be written twice - once where it is drawn, once where it is
 * hit - and nothing checked that the two agreed. The Files sidebar drew its
 * rows eight pixels from the window's edge and hit-tested them from the
 * content's, and that is not a mistake anybody makes with a layout engine,
 * because there is no second number to disagree with the first.
 *
 * So: a tree of views, a layout pass that gives each one its frame, an event
 * pass that finds the view under the pointer and the view holding the keyboard,
 * and a draw pass. The application says what its interface contains and what to
 * do when something happens in it.
 *
 *     struct ui_view* root = ui_box(UI_STACK_V, 12, 8);
 *     ui_label(root, "Name");
 *     struct ui_view* name = ui_field(root, "");
 *     ui_button(root, "Save", on_save, name);
 *
 * Nothing is allocated: views come from a fixed pool, because free() is a
 * no-op in this libc and an interface built once at startup has no reason to
 * be freed anyway.
 */

#define UI_MAX_VIEWS 192
#define UI_TEXT_MAX  128

struct ui_view;

/* Something happened to this view - a button was clicked, a field committed
 * with Return, a list row was chosen. `user` is whatever was passed in when
 * the view was made. */
typedef void (*ui_action)(struct ui_view* v, void* user);

/* A list's contents, asked for rather than copied: an application already has
 * its rows in an array of its own, and a list that insisted on its own copy
 * would be a second one to keep in step. */
typedef const char* (*ui_row_text)(void* user, int row);

enum {
    UI_BOX,             /* lays its children out; draws nothing itself */
    UI_LABEL,
    UI_BUTTON,
    UI_FIELD,           /* editable text, with a caret */
    UI_CHECK,
    UI_RADIO,
    UI_SEGMENTED,       /* a pill group; one of n */
    UI_LIST,            /* rows, selection, scrolling */
    UI_SLIDER,
    UI_PROGRESS,
    UI_SPACER,          /* nothing, but takes up room */
    UI_CUSTOM,          /* the application draws it */
    UI_SIDEBAR,         /* a list, styled as the window's spine */

    /* --- the second set -----------------------------------------------------
     *
     * Added by asking what the applications already written here had to build
     * for themselves. Every one of these exists in the tree twice or more:
     * Files has a table, a tree and an icon grid; Edit, Console and the browser
     * each hold a page of scrolling text; Settings and the browser both want a
     * bar of menus; three windows are a sidebar beside a pane with a divider
     * between them that none of them can drag.
     */
    UI_SEPARATOR,       /* a rule between sections */
    UI_GROUP,           /* a titled box holding other views */
    UI_TOGGLE,          /* on or off, as a switch rather than a tick */
    UI_STEPPER,         /* a number with a - and a + */
    UI_POPUP,           /* one of a list, chosen from a drop-down */
    UI_TABS,            /* a strip of tabs, the last one selected */
    UI_MENUBAR,         /* titles across a bar, each with a drop-down */
    UI_TABLE,           /* rows and columns, with headings */
    UI_TREE,            /* rows at depths, with twisties */
    UI_ICONGRID,        /* icons with labels, wrapped into rows */
    UI_TEXT,            /* many lines, editable, scrolling */
    UI_SCROLL,          /* a window onto a child taller than itself */
    UI_SPLIT,           /* two children and a divider that can be dragged */
    UI_IMAGE,           /* a picture */
};

/* How a box arranges its children. */
enum { UI_STACK_V, UI_STACK_H, UI_FIXED };

/* Flags. */
#define UI_HIDDEN    1u
#define UI_DISABLED  2u
#define UI_FOCUSABLE 4u

struct ui_rect { int x, y, w, h; };

struct ui_view {
    int kind;
    unsigned flags;

    /* Where it ended up. Written by the layout pass; an application reads it
     * (to draw a custom view, say) and does not set it - except in a UI_FIXED
     * parent, where the frame is the point. */
    struct ui_rect frame;

    /* What it asks for. `grow` is a share of whatever room is left over after
     * every child has its minimum: two children with grow 1 split the slack
     * evenly, one with grow 0 keeps exactly its minimum. */
    int want_w, want_h;
    int grow;

    /* Boxes only. */
    int layout, pad, gap;

    char text[UI_TEXT_MAX];
    int  caret;                 /* fields: where the next character goes */
    int  on;                    /* check, radio, segmented-selected index */
    int  value, max;            /* slider, progress */

    /* Lists, and everything that is a list underneath: tables, trees, icon
     * grids, tabs, popups and menu bars all answer "how many" and "what is the
     * text of one", and differ in what they draw around it. */
    ui_row_text row_text;
    int         rows;
    int         selected;
    int         scroll;         /* first visible row, or pixels for UI_TEXT */
    int         row_h;

    /* Tables. Columns are declared rather than measured, because a column that
     * resized itself to its contents would move every time the contents did. */
    const char* (*cell)(void* user, int row, int col);
    int  cols;
    char col_title[6][24];
    int  col_w[6];

    /* Trees. The application keeps the flattened rows - it is the only one that
     * knows what expanding a row reveals - and answers what depth each is at
     * and whether it can be opened. */
    int (*depth_of)(void* user, int row);
    int (*branch_of)(void* user, int row);      /* 0 leaf, 1 shut, 2 open */
    int hit_branch;             /* the twisty was what was clicked, not the row */

    /* Icon grids. */
    const uint32_t* (*icon_of)(void* user, int row);
    int cell_w, cell_h;

    /* Text areas. The buffer belongs to the application: a component that owned
     * its own would mean copying a document in and out of it to do anything
     * with it. */
    char* buffer;
    int   cap;

    /* Splits: where the divider is, as a distance from the leading edge. */
    int divider;

    /* Popups and menu bars: which drop-down is showing, if any. */
    int open;

    ui_action action;
    void*     user;
    void (*draw)(struct ui_view* v, void* user);   /* UI_CUSTOM */

    struct ui_view* child;
    struct ui_view* next;
    struct ui_view* parent;
};

/* --- building -------------------------------------------------------------
 *
 * Every constructor takes the parent it belongs to and returns the view, so an
 * interface reads as the shape it makes. Pass 0 as the parent for a root.
 */
struct ui_view* ui_box(struct ui_view* parent, int layout, int pad, int gap);
struct ui_view* ui_label(struct ui_view* parent, const char* text);
struct ui_view* ui_button(struct ui_view* parent, const char* label,
                          ui_action action, void* user);
struct ui_view* ui_field(struct ui_view* parent, const char* text);
struct ui_view* ui_check(struct ui_view* parent, const char* label, int on);
struct ui_view* ui_radio(struct ui_view* parent, const char* label, int on);
struct ui_view* ui_segmented(struct ui_view* parent, ui_row_text items,
                             int count, void* user);
struct ui_view* ui_list(struct ui_view* parent, ui_row_text rows, int count,
                        void* user);
struct ui_view* ui_sidebar(struct ui_view* parent, ui_row_text rows, int count,
                           void* user);
struct ui_view* ui_slider(struct ui_view* parent, int value, int max);
struct ui_view* ui_progress(struct ui_view* parent, int value, int max);
struct ui_view* ui_spacer(struct ui_view* parent);
struct ui_view* ui_custom(struct ui_view* parent,
                          void (*draw)(struct ui_view*, void*), void* user);

/* --- the second set --------------------------------------------------------
 *
 * Same shape as the first: a parent, whatever the component needs, and a view
 * back. Anything a component cannot know - what a table's cells say, what a
 * tree's rows are - is asked for through a callback, so the application keeps
 * its own data and there is never a second copy to hold in step.
 */

struct ui_view* ui_separator(struct ui_view* parent);
struct ui_view* ui_group(struct ui_view* parent, const char* title,
                         int layout, int pad, int gap);
struct ui_view* ui_toggle(struct ui_view* parent, const char* label, int on);
struct ui_view* ui_stepper(struct ui_view* parent, int value, int max);
struct ui_view* ui_image(struct ui_view* parent, const uint32_t* px, int size);

/* One of a list, shown as the chosen one until it is opened. */
struct ui_view* ui_popup(struct ui_view* parent, ui_row_text items, int count,
                         void* user);

/* A strip of tabs. `selected` is `on`, as with the segmented control. */
struct ui_view* ui_tabs(struct ui_view* parent, ui_row_text titles, int count,
                        void* user);

/* A bar of menu titles. Each title's items come from `items`, asked for as
 * (menu * 100 + item) so that one callback can answer for the whole bar - the
 * alternative is an array of arrays, which is a second structure to keep in
 * step with the first. `counts` says how many items each menu has. */
struct ui_view* ui_menubar(struct ui_view* parent, ui_row_text titles,
                           int count, ui_row_text items,
                           const int* counts, void* user);

/* Rows and columns. Declare the columns after making it. */
struct ui_view* ui_table(struct ui_view* parent,
                         const char* (*cell)(void* user, int row, int col),
                         int rows, void* user);
void ui_column(struct ui_view* table, const char* title, int width);

struct ui_view* ui_tree(struct ui_view* parent, ui_row_text rows, int count,
                        int (*depth_of)(void*, int),
                        int (*branch_of)(void*, int), void* user);

struct ui_view* ui_icongrid(struct ui_view* parent, ui_row_text labels,
                            int count,
                            const uint32_t* (*icon_of)(void*, int), void* user);

/* Editable text over a buffer the application owns. */
struct ui_view* ui_text_area(struct ui_view* parent, char* buffer, int cap);

/* A window onto a child that does not fit. The child is the one view added to
 * it; anything taller than the frame scrolls. */
struct ui_view* ui_scroll(struct ui_view* parent);

/* Two children with a divider between them. The first two views added are the
 * panes; `at` is where the divider starts. */
struct ui_view* ui_split(struct ui_view* parent, int layout, int at);

/* Sizing, as a hint rather than a command: layout may give a view more when it
 * is set to grow, and never gives it less. */
struct ui_view* ui_size(struct ui_view* v, int w, int h);
struct ui_view* ui_grow(struct ui_view* v, int share);
struct ui_view* ui_on(struct ui_view* v, ui_action action, void* user);

/* Everything goes back to the pool. Called between building one interface and
 * building another; an application that builds once never needs it. */
void ui_reset(void);

/* --- running --------------------------------------------------------------
 *
 * Three passes, in this order, which is the order they depend on each other:
 * layout decides where things are, events need to know that to find what was
 * pressed, and drawing needs to know what the event changed.
 */
void ui_layout(struct ui_view* root, struct ui_rect into);
int  ui_event(struct ui_view* root, const struct win_event* e);  /* 1 = redraw */
void ui_draw(struct ui_view* root);

/* Who has the keyboard, and setting it - so an application can put the caret
 * in the obvious field when a window opens. */
struct ui_view* ui_focused(void);
void            ui_focus(struct ui_view* v);

/* A view's text, for a field an application wants to read. */
const char* ui_text(const struct ui_view* v);
void        ui_set_text(struct ui_view* v, const char* text);

#endif
