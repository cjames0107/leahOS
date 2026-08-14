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

    /* Lists. */
    ui_row_text row_text;
    int         rows;
    int         selected;
    int         scroll;         /* first visible row */
    int         row_h;

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
