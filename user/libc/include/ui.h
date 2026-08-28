#ifndef _UI_H
#define _UI_H

#include <widget.h>
#include <textedit.h>
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

/* A browser's columns. `chosen` is what has been picked in every column so far,
 * which is what decides the contents of the next one. */
typedef int (*ui_col_count)(void* user, int column, const int* chosen);
typedef const char* (*ui_col_text)(void* user, int column, int row,
                                   const int* chosen);

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

    /* --- the third set ------------------------------------------------------
     *
     * What the second set did not cover. Several of these are variations on
     * something already here - a secure field is a field that will not show
     * you what you typed - and are separate kinds rather than flags because
     * the difference is in what they are *for*, and a caller should be able
     * to say which one it wants by name.
     */
    UI_SECURE,          /* a field that shows bullets, for a password */
    UI_SEARCH,          /* a field with a magnifier and a way to empty it */
    UI_COMBO,           /* a button with a menu on one end */
    UI_COLOUR,          /* a well showing a colour, opening a picker */
    UI_LEVEL,           /* a rating or a level: battery, volume, signal */
    UI_SPINNER,         /* work of unknown length, as motion rather than a bar */
    UI_POPOVER,         /* transient content, anchored to what raised it */
    UI_BROWSER,         /* columns, each listing what was chosen in the last */
    UI_CALENDAR,        /* a month, with a day chosen */
};

/* How a box arranges its children. */
enum { UI_STACK_V, UI_STACK_H, UI_FIXED };

/* Flags. */
#define UI_HIDDEN    1u
#define UI_DISABLED  2u
#define UI_FOCUSABLE 4u
/* As tall as what is in it - see ui_fit. */
#define UI_FIT       8u
/* A row that folds what does not fit into a popover - see ui_overflow. */
#define UI_OVERFLOW  16u

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
    /* The other end of the selection. Equal to the caret when nothing is
     * selected, which is what a caret is. */
    int  sel_anchor;
    /* What has been done to this view's text, so it can be undone. Owned by
     * the text engine; see textedit.h. */
    struct te_history* history;
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

    /* Browsers: which row is chosen in each column. A column shows what the
     * column before it chose, which is the whole idea - the path through a
     * hierarchy is visible all at once rather than one level at a time. */
    int col_sel[4];

    /* Calendars: what is being shown, and what is chosen in it. */
    int year, month, day;

    /* Popovers: the view they are anchored to, so they can be drawn beside it
     * wherever the layout happens to have put it. */
    struct ui_view* anchor;

    /* Browsers ask for their columns rather than holding them. */
    ui_col_count col_count;
    ui_col_text  col_text;

    /* Popups and menu bars: which drop-down is showing, if any. */
    int open;

    ui_action action;
    void*     user;
    void (*draw)(struct ui_view* v, void* user);   /* UI_CUSTOM */
    int  (*measure)(struct ui_view* v, int width, void* user);

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

/* How tall the content is, asked of the view rather than stored on it.
 *
 * A custom view inside a scroll is the case a fixed want_h cannot express: a
 * listing's height is its row count times a row, and an icon grid's is the
 * number of rows the icons wrap into - which is not known until the width is,
 * and the width is not known until the bar has taken its share. So the
 * question is asked during layout, with the width as the answer's argument,
 * and the answer is the document's height in pixels.
 *
 * Without this the height had to be computed by the application before layout,
 * from the width the last layout used - one frame stale, which is a scrollbar
 * that reports the wrong length for the first frame after every resize. */
void ui_measure(struct ui_view* v,
                int (*fn)(struct ui_view* v, int width, void* user));

/* Two children with a divider between them. The first two views added are the
 * panes; `at` is where the divider starts. */
struct ui_view* ui_split(struct ui_view* parent, int layout, int at);

/* --- the third set --------------------------------------------------------- */

/* A password field: the same editing, none of the display. What you typed is
 * in ui_text as usual - the masking is only what is drawn, because a field
 * that would not tell its own program the password would be useless. */
struct ui_view* ui_secure(struct ui_view* parent);

/* A search field. `action` fires on every keystroke rather than on Return,
 * because searching as you type is the point; clicking the cross empties it
 * and fires once more so the caller can put the unfiltered list back. */
struct ui_view* ui_search(struct ui_view* parent, const char* placeholder);

/* A button with a menu on one end: the button does the usual thing, the arrow
 * offers the variations.
 *
 * One action serves both, and `selected` says which happened: -1 when the
 * button itself was pressed, otherwise the item chosen. A second callback
 * would mean two places to write the same handler in the common case where
 * the button is just the first item. */
struct ui_view* ui_combo(struct ui_view* parent, const char* label,
                         ui_row_text items, int count, void* user);

/* A colour, shown as a well and edited in a picker that opens under it.
 * `value` is 0xRRGGBB and is what the action should read. */
struct ui_view* ui_colour(struct ui_view* parent, uint32_t rgb);

/* A level: how full, how strong, how loud. `segments` of 0 draws it as one
 * continuous bar; anything else draws that many discrete blocks, which is what
 * a rating or a signal strength wants. */
struct ui_view* ui_level(struct ui_view* parent, int value, int max,
                         int segments);

/* Work of unknown length. Unlike a progress bar this says nothing about how
 * far along it is, because that is exactly what is not known - it only says
 * that something is still happening. Needs a tick to turn. */
struct ui_view* ui_spinner(struct ui_view* parent);
void ui_spin(struct ui_view* v);        /* one step, from the application's tick */

/* Transient content beside the thing that raised it. Returns the popover,
 * which is a box - put components in it as usual. It is shown until something
 * outside it is pressed. */
struct ui_view* ui_popover(struct ui_view* parent, struct ui_view* anchor);
void ui_popover_show(struct ui_view* popover, int shown);

/* Columns, each listing what the column before it chose. `count` answers how
 * many rows a column has given the path so far, and `text` names one; both are
 * asked for with the column index, so the application keeps its own tree and
 * this keeps only which row is chosen where. */
struct ui_view* ui_browser(struct ui_view* parent, int columns,
                           ui_col_count count, ui_col_text text, void* user);

/* A month, with a day chosen. Returns the view; the chosen date is in `year`,
 * `month` (0..11) and `day`. */
struct ui_view* ui_calendar(struct ui_view* parent, int year, int month,
                            int day);

/* Sizing, as a hint rather than a command: layout may give a view more when it
 * is set to grow, and never gives it less. */
struct ui_view* ui_size(struct ui_view* v, int w, int h);
struct ui_view* ui_grow(struct ui_view* v, int share);

/* As tall as what is in it, worked out rather than written down.
 *
 * A box's height was a number the caller computed - `3 * 24 + 28` for three
 * rows of twenty-four in a group with twelve of padding - and every one of
 * those is a sum that is wrong the moment a row is added, a padding changes,
 * or a control's natural height does. Several of them were already wrong, and
 * the symptom is the last thing in the box hanging out of the bottom of it.
 *
 * A box that fits asks its children instead. */
struct ui_view* ui_fit(struct ui_view* v);

/* What a view needs, for a caller that wants the number for itself. */
int ui_natural_h(struct ui_view* v);

/* A row of controls that folds when the window is too narrow for it.
 *
 * A toolbar laid out left to right in a window that can be made narrower
 * eventually runs out of room, and what happened then was that the controls at
 * the end were drawn off the edge - present, unreachable, and with no sign
 * that anything was missing.
 *
 * Now the ones that do not fit move into a popover behind a button at the end
 * of the row, and move back when the window is widened. They are the same
 * controls, not copies of them: a toggle folded away and switched on in the
 * popover is on when it comes back.
 *
 * Call it on a horizontal box after its children are added. */
struct ui_view* ui_overflow(struct ui_view* box);
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
