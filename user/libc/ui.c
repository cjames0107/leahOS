/* The component layer: layout, routing, drawing. See <ui.h> for why.
 *
 * The three passes never disagree about geometry because only one of them
 * decides it. ui_layout writes every frame; ui_event and ui_draw read them.
 * That is the whole reason a control can no longer be drawn in one place and
 * hit in another, which is the bug this exists to make unwritable.
 */

#include <ui.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

/* An icon is 32x32 here as it is everywhere else; named rather than
 * repeated so a change to the icon set is one line. */
#define ICON_SIZE_DEFAULT 32
#define DIVIDER_W 6

/* Asked for when the first view is made, rather than reserved in every
 * program that links libc.
 *
 * As a static array this was ninety kilobytes of BSS in every binary on the
 * system - including audiod, which never draws anything. That is not merely
 * wasteful: audiod is restarted by init when it dies, and with every process
 * on the machine carrying the extra, a restart began failing at startup with a
 * null dereference inside libc. The pool costs nothing now unless something
 * builds an interface out of it. */
static struct ui_view* g_pool;
static int g_used;
static struct ui_view* g_focus;
static struct ui_view* g_pressed;
/* The view whose drop-down is showing. There is at most one: a second menu
 * opening closes the first, which is what a menu bar does. */
/* A menu row that is a rule rather than a choice. */
static int ui_is_separator(const char* label)
{
    return label != 0 && label[0] == '-' && label[1] == '\0';
}

static struct ui_view* g_dropped;
/* Which channel of the colour mixer is being dragged, or -1. */
static int g_channel = -1;
static void colour_panel(const struct ui_view* v, int* x, int* y, int* w,
                         int* h);
/* The popover that is showing. One at a time, like the drop-downs: a second
 * one over the first is not something a person can answer. */
static struct ui_view* g_popover;

void ui_reset(void)
{
    g_used = 0;
    g_focus = 0;
    g_pressed = 0;
    g_dropped = 0;
    g_channel = -1;
}

/* Where a view's drop-down is, and how many rows it has. Written once and read
 * by both the drawing and the hit-testing, for the same reason every other
 * frame is: two copies of a rectangle are two chances to disagree. */
static int dropdown_box(struct ui_view* v, int* x, int* y, int* w, int* n)
{
    if (v == 0)
        return 0;
    if ((v->kind == UI_POPUP || v->kind == UI_COMBO) && v->open) {
        *x = v->frame.x; *y = v->frame.y + v->frame.h;
        *w = v->frame.w; *n = v->rows;
        return 1;
    }
    if (v->kind == UI_MENUBAR && v->open >= 0) {
        int at = v->frame.x;
        for (int i = 0; i < v->open; ++i) {
            const char* t = v->row_text != 0 ? v->row_text(v->user, i) : "";
            at += (int)strlen(t != 0 ? t : "") * WG_GLYPH_W + 20;
        }
        const int* counts = (const int*)(void*)v->depth_of;
        *x = at; *y = v->frame.y + v->frame.h; *w = 170;
        *n = counts != 0 ? counts[v->open] : 0;
        /* Pulled left when it would run off the window. A menu bar sits at the
         * right-hand end of a toolbar as often as not, and dropping its items
         * straight down from there put half of every label past the edge. */
        const struct ui_view* root = v;
        while (root->parent != 0)
            root = root->parent;
        const int right = root->frame.x + root->frame.w;
        if (*x + *w > right - 4)
            *x = right - 4 - *w;
        if (*x < root->frame.x + 4)
            *x = root->frame.x + 4;
        return *n > 0;
    }
    return 0;
}

static struct ui_view* alloc_view(struct ui_view* parent, int kind)
{
    if (g_pool == 0) {
        g_pool = (struct ui_view*)malloc(sizeof(struct ui_view) * UI_MAX_VIEWS);
        if (g_pool == 0)
            return 0;
    }
    if (g_used >= UI_MAX_VIEWS)
        return 0;
    struct ui_view* v = &g_pool[g_used++];
    memset(v, 0, sizeof(*v));
    v->kind = kind;
    v->row_h = WG_GLYPH_H + 8;
    v->selected = -1;
    v->parent = parent;
    if (parent != 0) {
        /* Appended, so children are laid out in the order they were written.
         * A list that reversed itself would make the code read backwards. */
        struct ui_view** at = &parent->child;
        while (*at != 0)
            at = &(*at)->next;
        *at = v;
    }
    return v;
}

static void set_text(struct ui_view* v, const char* text)
{
    if (text == 0) { v->text[0] = '\0'; v->caret = 0; return; }
    snprintf(v->text, sizeof(v->text), "%s", text);
    v->caret = (int)strlen(v->text);
}

/* --- building ------------------------------------------------------------- */

struct ui_view* ui_box(struct ui_view* parent, int layout, int pad, int gap)
{
    struct ui_view* v = alloc_view(parent, UI_BOX);
    if (v == 0) return 0;
    v->layout = layout;
    v->pad = pad;
    v->gap = gap;
    v->grow = 1;                /* a box usually is the room it is given */
    return v;
}

struct ui_view* ui_label(struct ui_view* parent, const char* text)
{
    struct ui_view* v = alloc_view(parent, UI_LABEL);
    if (v == 0) return 0;
    set_text(v, text);
    v->want_h = WG_GLYPH_H + 4;
    v->want_w = (int)strlen(v->text) * WG_GLYPH_W;
    return v;
}

struct ui_view* ui_button(struct ui_view* parent, const char* label,
                          ui_action action, void* user)
{
    struct ui_view* v = alloc_view(parent, UI_BUTTON);
    if (v == 0) return 0;
    set_text(v, label);
    v->action = action;
    v->user = user;
    v->want_h = 26;
    v->want_w = (int)strlen(v->text) * WG_GLYPH_W + 28;
    v->flags |= UI_FOCUSABLE;
    return v;
}

struct ui_view* ui_field(struct ui_view* parent, const char* text)
{
    struct ui_view* v = alloc_view(parent, UI_FIELD);
    if (v == 0) return 0;
    set_text(v, text);
    v->want_h = 26;
    v->want_w = 160;
    v->flags |= UI_FOCUSABLE;
    return v;
}

struct ui_view* ui_check(struct ui_view* parent, const char* label, int on)
{
    struct ui_view* v = alloc_view(parent, UI_CHECK);
    if (v == 0) return 0;
    set_text(v, label);
    v->on = on;
    v->want_h = 20;
    v->want_w = (int)strlen(v->text) * WG_GLYPH_W + 26;
    v->flags |= UI_FOCUSABLE;
    return v;
}

struct ui_view* ui_radio(struct ui_view* parent, const char* label, int on)
{
    struct ui_view* v = ui_check(parent, label, on);
    if (v != 0) v->kind = UI_RADIO;
    return v;
}

struct ui_view* ui_segmented(struct ui_view* parent, ui_row_text items,
                             int count, void* user)
{
    struct ui_view* v = alloc_view(parent, UI_SEGMENTED);
    if (v == 0) return 0;
    v->row_text = items;
    v->rows = count;
    v->user = user;
    v->on = 0;
    v->want_h = 26;
    v->want_w = count * 70;
    v->flags |= UI_FOCUSABLE;
    return v;
}

struct ui_view* ui_list(struct ui_view* parent, ui_row_text rows, int count,
                        void* user)
{
    struct ui_view* v = alloc_view(parent, UI_LIST);
    if (v == 0) return 0;
    v->row_text = rows;
    v->rows = count;
    v->user = user;
    v->grow = 1;
    v->want_h = 80;
    v->flags |= UI_FOCUSABLE;
    return v;
}

struct ui_view* ui_sidebar(struct ui_view* parent, ui_row_text rows, int count,
                           void* user)
{
    struct ui_view* v = ui_list(parent, rows, count, user);
    if (v == 0) return 0;
    v->kind = UI_SIDEBAR;
    v->grow = 0;
    v->want_w = 160;
    return v;
}

struct ui_view* ui_slider(struct ui_view* parent, int value, int max)
{
    struct ui_view* v = alloc_view(parent, UI_SLIDER);
    if (v == 0) return 0;
    v->value = value;
    v->max = max > 0 ? max : 100;
    v->want_h = WG_SLIDER_H;
    v->want_w = 160;
    v->flags |= UI_FOCUSABLE;
    return v;
}

struct ui_view* ui_progress(struct ui_view* parent, int value, int max)
{
    struct ui_view* v = alloc_view(parent, UI_PROGRESS);
    if (v == 0) return 0;
    v->value = value;
    v->max = max > 0 ? max : 100;
    v->want_h = 12;
    v->want_w = 160;
    return v;
}

struct ui_view* ui_spacer(struct ui_view* parent)
{
    struct ui_view* v = alloc_view(parent, UI_SPACER);
    if (v == 0) return 0;
    v->grow = 1;
    return v;
}

struct ui_view* ui_custom(struct ui_view* parent,
                          void (*draw)(struct ui_view*, void*), void* user)
{
    struct ui_view* v = alloc_view(parent, UI_CUSTOM);
    if (v == 0) return 0;
    v->draw = draw;
    v->user = user;
    v->grow = 1;
    return v;
}

/* --- the second set -------------------------------------------------------- */

struct ui_view* ui_separator(struct ui_view* parent)
{
    struct ui_view* v = alloc_view(parent, UI_SEPARATOR);
    if (v == 0) return 0;
    v->want_h = 9;
    v->want_w = 9;
    return v;
}

struct ui_view* ui_group(struct ui_view* parent, const char* title,
                         int layout, int pad, int gap)
{
    /* A box that draws a heading and a frame. Its children lay out inside the
     * padding as any box's do, with room left at the top for the title. */
    struct ui_view* v = alloc_view(parent, UI_GROUP);
    if (v == 0) return 0;
    set_text(v, title);
    v->layout = layout;
    v->pad = pad;
    v->gap = gap;
    v->grow = 1;
    return v;
}

struct ui_view* ui_toggle(struct ui_view* parent, const char* label, int on)
{
    struct ui_view* v = alloc_view(parent, UI_TOGGLE);
    if (v == 0) return 0;
    set_text(v, label);
    v->on = on;
    v->want_h = 22;
    v->want_w = (int)strlen(v->text) * WG_GLYPH_W + 52;
    v->flags |= UI_FOCUSABLE;
    return v;
}

struct ui_view* ui_stepper(struct ui_view* parent, int value, int max)
{
    struct ui_view* v = alloc_view(parent, UI_STEPPER);
    if (v == 0) return 0;
    v->value = value;
    v->max = max > 0 ? max : 100;
    v->want_h = 24;
    v->want_w = 96;
    v->flags |= UI_FOCUSABLE;
    return v;
}

struct ui_view* ui_image(struct ui_view* parent, const uint32_t* px, int size)
{
    struct ui_view* v = alloc_view(parent, UI_IMAGE);
    if (v == 0) return 0;
    v->icon_of = 0;
    v->user = (void*)px;
    v->want_w = size > 0 ? size : ICON_SIZE_DEFAULT;
    v->want_h = v->want_w;
    return v;
}

struct ui_view* ui_popup(struct ui_view* parent, ui_row_text items, int count,
                         void* user)
{
    struct ui_view* v = alloc_view(parent, UI_POPUP);
    if (v == 0) return 0;
    v->row_text = items;
    v->rows = count;
    v->user = user;
    v->selected = 0;
    v->want_h = 26;
    v->want_w = 160;
    v->flags |= UI_FOCUSABLE;
    return v;
}

struct ui_view* ui_tabs(struct ui_view* parent, ui_row_text titles, int count,
                        void* user)
{
    struct ui_view* v = alloc_view(parent, UI_TABS);
    if (v == 0) return 0;
    v->row_text = titles;
    v->rows = count;
    v->user = user;
    v->want_h = 26;
    v->flags |= UI_FOCUSABLE;
    return v;
}

struct ui_view* ui_menubar(struct ui_view* parent, ui_row_text titles,
                           int count, ui_row_text items,
                           const int* counts, void* user)
{
    struct ui_view* v = alloc_view(parent, UI_MENUBAR);
    if (v == 0) return 0;
    v->row_text = titles;
    v->rows = count;
    v->cell = 0;
    v->user = user;
    /* The items callback and the per-menu counts ride in the fields a menu bar
     * does not otherwise use, rather than growing the struct for one kind. */
    v->icon_of = (const uint32_t* (*)(void*, int))items;
    v->depth_of = (int (*)(void*, int))(void*)counts;
    v->open = -1;
    v->want_h = 24;
    v->flags |= UI_FOCUSABLE;
    return v;
}

struct ui_view* ui_table(struct ui_view* parent,
                         const char* (*cell)(void* user, int row, int col),
                         int rows, void* user)
{
    struct ui_view* v = alloc_view(parent, UI_TABLE);
    if (v == 0) return 0;
    v->cell = cell;
    v->rows = rows;
    v->user = user;
    v->grow = 1;
    v->row_h = WG_GLYPH_H + 4;
    v->flags |= UI_FOCUSABLE;
    return v;
}

void ui_column(struct ui_view* t, const char* title, int width)
{
    if (t == 0 || t->cols >= 6)
        return;
    snprintf(t->col_title[t->cols], sizeof(t->col_title[0]), "%s",
             title != 0 ? title : "");
    t->col_w[t->cols] = width > 0 ? width : 100;
    ++t->cols;
}

struct ui_view* ui_tree(struct ui_view* parent, ui_row_text rows, int count,
                        int (*depth_of)(void*, int),
                        int (*branch_of)(void*, int), void* user)
{
    struct ui_view* v = alloc_view(parent, UI_TREE);
    if (v == 0) return 0;
    v->row_text = rows;
    v->rows = count;
    v->depth_of = depth_of;
    v->branch_of = branch_of;
    v->user = user;
    v->grow = 1;
    v->row_h = WG_GLYPH_H + 4;
    v->flags |= UI_FOCUSABLE;
    return v;
}

struct ui_view* ui_icongrid(struct ui_view* parent, ui_row_text labels,
                            int count,
                            const uint32_t* (*icon_of)(void*, int), void* user)
{
    struct ui_view* v = alloc_view(parent, UI_ICONGRID);
    if (v == 0) return 0;
    v->row_text = labels;
    v->rows = count;
    v->icon_of = icon_of;
    v->user = user;
    v->grow = 1;
    v->cell_w = 92;
    v->cell_h = 64;
    v->flags |= UI_FOCUSABLE;
    return v;
}

struct ui_view* ui_text_area(struct ui_view* parent, char* buffer, int cap)
{
    struct ui_view* v = alloc_view(parent, UI_TEXT);
    if (v == 0) return 0;
    v->buffer = buffer;
    v->cap = cap;
    v->grow = 1;
    v->row_h = WG_GLYPH_H;
    v->caret = buffer != 0 ? (int)strlen(buffer) : 0;
    v->flags |= UI_FOCUSABLE;
    return v;
}

struct ui_view* ui_scroll(struct ui_view* parent)
{
    struct ui_view* v = alloc_view(parent, UI_SCROLL);
    if (v == 0) return 0;
    v->grow = 1;
    v->flags |= UI_FOCUSABLE;
    return v;
}

struct ui_view* ui_split(struct ui_view* parent, int layout, int at)
{
    struct ui_view* v = alloc_view(parent, UI_SPLIT);
    if (v == 0) return 0;
    v->layout = layout;
    v->divider = at;
    v->grow = 1;
    return v;
}

/* --- the third set -------------------------------------------------------- */

struct ui_view* ui_secure(struct ui_view* parent)
{
    struct ui_view* v = ui_field(parent, "");
    if (v != 0) v->kind = UI_SECURE;
    return v;
}

struct ui_view* ui_search(struct ui_view* parent, const char* placeholder)
{
    struct ui_view* v = ui_field(parent, "");
    if (v == 0) return 0;
    v->kind = UI_SEARCH;
    /* The placeholder lives where a label's text would: it is drawn when the
     * field is empty and is never part of what the field contains. */
    snprintf(v->col_title[0], sizeof(v->col_title[0]), "%s",
             placeholder != 0 ? placeholder : "Search");
    return v;
}

struct ui_view* ui_combo(struct ui_view* parent, const char* label,
                         ui_row_text items, int count, void* user)
{
    struct ui_view* v = alloc_view(parent, UI_COMBO);
    if (v == 0) return 0;
    set_text(v, label);
    v->row_text = items;
    v->rows = count;
    v->user = user;
    v->open = 0;
    v->selected = -1;
    v->want_h = 26;
    v->want_w = (int)strlen(v->text) * WG_GLYPH_W + 56;
    v->flags |= UI_FOCUSABLE;
    return v;
}

struct ui_view* ui_colour(struct ui_view* parent, uint32_t rgb)
{
    struct ui_view* v = alloc_view(parent, UI_COLOUR);
    if (v == 0) return 0;
    v->value = (int)(rgb & 0xFFFFFF);
    v->want_h = 24;
    v->want_w = 54;
    v->flags |= UI_FOCUSABLE;
    return v;
}

struct ui_view* ui_level(struct ui_view* parent, int value, int max,
                         int segments)
{
    struct ui_view* v = alloc_view(parent, UI_LEVEL);
    if (v == 0) return 0;
    v->value = value;
    v->max = max > 0 ? max : 100;
    v->on = segments > 0 ? segments : 0;    /* 0 means one continuous bar */
    v->want_h = 14;
    v->want_w = 140;
    return v;
}

struct ui_view* ui_spinner(struct ui_view* parent)
{
    struct ui_view* v = alloc_view(parent, UI_SPINNER);
    if (v == 0) return 0;
    v->want_h = 20;
    v->want_w = 20;
    v->max = 12;                /* twelve spokes, one lit at a time */
    return v;
}

void ui_spin(struct ui_view* v)
{
    if (v != 0 && v->kind == UI_SPINNER)
        v->value = (v->value + 1) % (v->max > 0 ? v->max : 12);
}

struct ui_view* ui_popover(struct ui_view* parent, struct ui_view* anchor)
{
    struct ui_view* v = ui_box(parent, UI_STACK_V, 10, 6);
    if (v == 0) return 0;
    v->kind = UI_POPOVER;
    v->anchor = anchor;
    v->flags |= UI_HIDDEN;      /* until something asks for it */
    v->grow = 0;
    v->want_w = 200;
    v->want_h = 120;
    return v;
}

void ui_popover_show(struct ui_view* v, int shown)
{
    if (v == 0 || v->kind != UI_POPOVER)
        return;
    if (shown) {
        if (g_popover != 0 && g_popover != v)
            g_popover->flags |= UI_HIDDEN;
        v->flags &= ~UI_HIDDEN;
        g_popover = v;
    } else {
        v->flags |= UI_HIDDEN;
        if (g_popover == v)
            g_popover = 0;
    }
}

struct ui_view* ui_browser(struct ui_view* parent, int columns,
                           ui_col_count count, ui_col_text text, void* user)
{
    struct ui_view* v = alloc_view(parent, UI_BROWSER);
    if (v == 0) return 0;
    if (columns < 1) columns = 1;
    if (columns > 4) columns = 4;
    v->cols = columns;
    v->col_count = count;
    v->col_text = text;
    v->user = user;
    v->grow = 1;
    v->row_h = WG_GLYPH_H + 4;
    for (int i = 0; i < 4; ++i)
        v->col_sel[i] = -1;
    v->flags |= UI_FOCUSABLE;
    return v;
}

struct ui_view* ui_calendar(struct ui_view* parent, int year, int month,
                            int day)
{
    struct ui_view* v = alloc_view(parent, UI_CALENDAR);
    if (v == 0) return 0;
    v->year = year;
    v->month = month;
    v->day = day;
    v->want_w = 210;
    v->want_h = 170;
    v->flags |= UI_FOCUSABLE;
    return v;
}

struct ui_view* ui_size(struct ui_view* v, int w, int h)
{
    if (v != 0) { v->want_w = w; v->want_h = h; }
    return v;
}

struct ui_view* ui_grow(struct ui_view* v, int share)
{
    if (v != 0) v->grow = share;
    return v;
}

struct ui_view* ui_on(struct ui_view* v, ui_action action, void* user)
{
    if (v != 0) { v->action = action; v->user = user; }
    return v;
}

const char* ui_text(const struct ui_view* v) { return v != 0 ? v->text : ""; }
void ui_set_text(struct ui_view* v, const char* text) { if (v) set_text(v, text); }
struct ui_view* ui_focused(void) { return g_focus; }
void ui_focus(struct ui_view* v) { g_focus = v; }

/* --- layout ---------------------------------------------------------------
 *
 * One pass, top down. A box hands each child its minimum and then shares what
 * is left over by weight, which is the whole of the model: no constraints to
 * solve and nothing to iterate. It is enough for every window in this system,
 * and the interesting property is not what it can express but that a control's
 * position exists in exactly one place.
 */
void ui_layout(struct ui_view* v, struct ui_rect into)
{
    if (v == 0 || (v->flags & UI_HIDDEN) != 0)
        return;
    v->frame = into;

    if (v->kind == UI_SPLIT) {
        /* Two panes and a divider. The first child gets `divider` across the
         * split's direction, the second gets the rest - so dragging the
         * divider is one number changing and everything following from it. */
        struct ui_view* first = v->child;
        struct ui_view* second = first != 0 ? first->next : 0;
        const int across = (v->layout == UI_STACK_H);
        int at = v->divider;
        const int span = across ? into.w : into.h;
        if (at < 40) at = 40;
        if (at > span - 40) at = span - 40 > 40 ? span - 40 : 40;
        v->divider = at;
        if (first != 0) {
            struct ui_rect r = across ? (struct ui_rect){ into.x, into.y, at, into.h }
                                      : (struct ui_rect){ into.x, into.y, into.w, at };
            ui_layout(first, r);
        }
        if (second != 0) {
            struct ui_rect r = across
                ? (struct ui_rect){ into.x + at + DIVIDER_W, into.y,
                                    into.w - at - DIVIDER_W, into.h }
                : (struct ui_rect){ into.x, into.y + at + DIVIDER_W,
                                    into.w, into.h - at - DIVIDER_W };
            ui_layout(second, r);
        }
        return;
    }

    if (v->kind == UI_SCROLL) {
        /* The child is laid out at its own height rather than the frame's, and
         * shifted by the scroll. Anything past the bottom is simply not drawn -
         * which is why the child's own layout has to be done at full size. */
        struct ui_view* c = v->child;
        if (c != 0) {
            const int tall = c->want_h > into.h ? c->want_h : into.h;
            /* Room for the bar taken out of the width before the child is laid
             * out, so its contents stop short of it instead of being drawn
             * underneath it. */
            int w = into.w - WG_SCROLL_W - 4;
            if (w < 0) w = 0;
            struct ui_rect r = { into.x, into.y - v->scroll, w, tall };
            ui_layout(c, r);
        }
        return;
    }

    if (v->kind != UI_BOX && v->kind != UI_GROUP)
        return;

    /* A group keeps room at the top for its title; a plain box does not. */
    if (v->kind == UI_GROUP) {
        into.y += WG_GLYPH_H + 6;
        into.h -= WG_GLYPH_H + 6;
        v->frame.h = v->frame.h;    /* the frame stays the whole group */
    }

    const int horizontal = (v->layout == UI_STACK_H);
    struct ui_rect inner = { into.x + v->pad, into.y + v->pad,
                             into.w - 2 * v->pad, into.h - 2 * v->pad };

    if (v->layout == UI_FIXED) {
        /* The children keep the frames they were given; only the offset moves.
         * This is the escape hatch for a view whose arrangement is its own
         * business - a canvas, a chart, a desktop. */
        for (struct ui_view* c = v->child; c != 0; c = c->next) {
            struct ui_rect r = { inner.x + c->frame.x, inner.y + c->frame.y,
                                 c->frame.w, c->frame.h };
            ui_layout(c, r);
        }
        return;
    }

    /* Popovers are placed against their anchor rather than stacked, so they
     * are laid out after the others and take none of the room. */
    for (struct ui_view* c = v->child; c != 0; c = c->next) {
        if (c->kind != UI_POPOVER || (c->flags & UI_HIDDEN) != 0)
            continue;
        const struct ui_view* at = c->anchor != 0 ? c->anchor : v;
        struct ui_rect r = { at->frame.x, at->frame.y + at->frame.h + 6,
                             c->want_w, c->want_h };
        /* Kept inside the window: a popover that hangs off the edge is a
         * popover with its content cut off. */
        if (r.x + r.w > into.x + into.w) r.x = into.x + into.w - r.w;
        if (r.x < into.x) r.x = into.x;
        if (r.y + r.h > into.y + into.h) r.y = at->frame.y - r.h - 6;
        ui_layout(c, r);
    }

    int visible = 0, fixed = 0, weight = 0;
    for (struct ui_view* c = v->child; c != 0; c = c->next) {
        if ((c->flags & UI_HIDDEN) != 0 || c->kind == UI_POPOVER)
            continue;
        ++visible;
        fixed += horizontal ? c->want_w : c->want_h;
        weight += c->grow;
    }
    if (visible == 0)
        return;

    const int gaps = v->gap * (visible - 1);
    int slack = (horizontal ? inner.w : inner.h) - fixed - gaps;
    if (slack < 0)
        slack = 0;

    int at = horizontal ? inner.x : inner.y;
    int given = 0, seen = 0;
    for (struct ui_view* c = v->child; c != 0; c = c->next) {
        if ((c->flags & UI_HIDDEN) != 0 || c->kind == UI_POPOVER)
            continue;
        ++seen;
        int extra = 0;
        if (weight > 0 && c->grow > 0) {
            /* The last one to grow takes the remainder, so a division that
             * does not come out exactly leaves no gap at the end. */
            extra = (seen == visible) ? slack - given : slack * c->grow / weight;
            given += extra;
        }
        struct ui_rect r;
        if (horizontal) {
            r.x = at; r.y = inner.y;
            r.w = c->want_w + extra;
            r.h = c->want_h > 0 && c->grow == 0 ? c->want_h : inner.h;
            at += r.w + v->gap;
        } else {
            r.x = inner.x; r.y = at;
            r.w = c->want_w > 0 && c->grow == 0 ? c->want_w : inner.w;
            r.h = c->want_h + extra;
            at += r.h + v->gap;
        }
        /* Never wider or taller than the room it is being put in.
         *
         * A view that does not grow keeps the size it asked for, and what it
         * asked for was measured when it was made - a label's width is the
         * width of its text. Narrow the window, or drag a divider left, and
         * that width stopped fitting: the label kept drawing at its full size,
         * out through the side of the pane holding it and across whatever was
         * beside it. The pane is the limit, so it is applied here rather than
         * hoped for by each component. */
        if (r.w > (horizontal ? inner.w : inner.w))
            r.w = inner.w;
        if (r.h > inner.h)
            r.h = inner.h;
        if (r.w < 0) r.w = 0;
        if (r.h < 0) r.h = 0;
        ui_layout(c, r);
    }
}

/* --- events --------------------------------------------------------------- */

static int in_rect(const struct ui_rect* r, int x, int y)
{
    return x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h;
}

/* The deepest view under the point that can do something with it. A box is
 * scenery: it holds children and takes no events of its own. */
static struct ui_view* hit(struct ui_view* v, int x, int y)
{
    if (v == 0 || (v->flags & (UI_HIDDEN | UI_DISABLED)) != 0 ||
        !in_rect(&v->frame, x, y))
        return 0;
    for (struct ui_view* c = v->child; c != 0; c = c->next) {
        struct ui_view* deeper = hit(c, x, y);
        if (deeper != 0)
            return deeper;
    }
    return v->kind == UI_BOX ? 0 : v;
}

/* --- dates, for the calendar ----------------------------------------------
 *
 * The weekday is asked of the library rather than computed: timegm turns a
 * broken-down date back into a time_t and gmtime_r fills in tm_wday on the way
 * out, which is shorter than Zeller and impossible to get wrong by one. Noon
 * rather than midnight so no arithmetic can land the moment on the day before.
 */
static int ui_days_in(int year, int month)
{
    static const int kLength[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (month < 0 || month > 11)
        return 30;
    const int leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return (month == 1 && leap) ? 29 : kLength[month];
}

static int ui_first_weekday(int year, int month)
{
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = year - 1900;
    t.tm_mon = month;
    t.tm_mday = 1;
    t.tm_hour = 12;
    const time_t when = timegm(&t);
    struct tm back;
    if (gmtime_r(&when, &back) == 0)
        return 0;
    return back.tm_wday;
}

/* How far along a line the caret sits.
 *
 * WG_GLYPH_W is the width of a cell in the console font, and the interface is
 * not drawn in the console font - wg_text measures proportionally. Multiplying
 * the caret index by a fixed width put the mark a long way from the letter it
 * was in front of, further with every character typed. Measuring the text up
 * to the caret is the only thing that agrees with what was drawn, because it
 * asks the same function that drew it. */
static int caret_x(const char* text, int caret)
{
    char before[UI_TEXT_MAX];
    int n = caret;
    if (n < 0) n = 0;
    if (n > (int)sizeof(before) - 1) n = (int)sizeof(before) - 1;
    memcpy(before, text, (unsigned)n);
    before[n] = '\0';
    return wg_text_width(before);
}

static int list_rows_visible(const struct ui_view* v)
{
    const int n = v->frame.h / (v->row_h > 0 ? v->row_h : 1);
    return n > 0 ? n : 1;
}

static void list_show(struct ui_view* v, int row)
{
    const int page = list_rows_visible(v);
    if (row < v->scroll)                 v->scroll = row;
    else if (row >= v->scroll + page)    v->scroll = row - page + 1;
    if (v->scroll < 0) v->scroll = 0;
}

/* A key into a field. Returns 1 when the text changed or the caret moved. */
static int field_key(struct ui_view* v, unsigned key)
{
    const int n = (int)strlen(v->text);
    if (v->caret > n) v->caret = n;
    if (key == '\b') {
        if (v->caret <= 0)
            return 0;
        memmove(&v->text[v->caret - 1], &v->text[v->caret],
                (unsigned)(n - v->caret) + 1);
        --v->caret;
        return 1;
    }
    if (key == WIN_KEY_LEFT)  { if (v->caret > 0) { --v->caret; return 1; } return 0; }
    if (key == WIN_KEY_RIGHT) { if (v->caret < n) { ++v->caret; return 1; } return 0; }
    if (key == '\n' || key == '\r') {
        if (v->action != 0)
            v->action(v, v->user);
        return 1;
    }
    if (key >= ' ' && key < 127 && n + 1 < UI_TEXT_MAX) {
        /* Inserted at the caret rather than appended: a field that can only be
         * typed at the end is a field you have to retype to correct. */
        memmove(&v->text[v->caret + 1], &v->text[v->caret],
                (unsigned)(n - v->caret) + 1);
        v->text[v->caret++] = (char)key;
        return 1;
    }
    return 0;
}

int ui_event(struct ui_view* root, const struct win_event* e)
{
    if (root == 0 || e == 0)
        return 0;

    if (e->type == WIN_EVENT_MOUSE_DOWN) {
        /* A popover answers before anything beneath it, and a press outside it
         * puts it away - which is what "transient" means. */
        if (g_popover != 0 && (g_popover->flags & UI_HIDDEN) == 0) {
            struct ui_view* inside = hit(g_popover, e->x, e->y);
            if (inside == 0 && !in_rect(&g_popover->frame, e->x, e->y)) {
                ui_popover_show(g_popover, 0);
                return 1;
            }
        }
        /* An open drop-down answers first, wherever it is over. It is drawn
         * above its neighbours, so it has to be hit above them too - otherwise
         * choosing an item activates whatever the item happens to cover. */
        if (g_dropped != 0 && g_dropped->kind == UI_COLOUR) {
            struct ui_view* d = g_dropped;
            int px, py, pw, ph;
            colour_panel(d, &px, &py, &pw, &ph);
            if (e->x >= px && e->x < px + pw &&
                e->y >= py && e->y < py + ph) {
                g_channel = wg_rgb_hit(px + 8, py + 8, pw - 16, e->x, e->y);
                if (g_channel >= 0) {
                    d->value = (int)wg_rgb_move((uint32_t)d->value, g_channel,
                                                px + 8, pw - 16, e->x);
                    if (d->action) d->action(d, d->user);
                }
                return 1;
            }
            /* Anywhere else shuts it, and the press goes on to whatever it
             * landed on. */
            d->open = 0;
            g_dropped = 0;
        }
        if (g_dropped != 0) {
            int dx, dy, dw, dn;
            struct ui_view* d = g_dropped;
            const int rh = WG_GLYPH_H + 8;
            if (dropdown_box(d, &dx, &dy, &dw, &dn) &&
                e->x >= dx && e->x < dx + dw &&
                e->y >= dy && e->y < dy + dn * rh) {
                const int item = (e->y - dy) / rh;
                {
                    ui_row_text items = d->kind == UI_MENUBAR
                                      ? (ui_row_text)d->icon_of : d->row_text;
                    const char* t = items != 0
                        ? items(d->user, d->kind == UI_MENUBAR
                                       ? d->open * 100 + item : item)
                        : "";
                    if (ui_is_separator(t != 0 ? t : ""))
                        return 1;   /* a line: it takes the press and does nothing */
                }
                if (d->kind == UI_POPUP || d->kind == UI_COMBO) {
                    d->selected = item;
                    d->open = 0;
                } else {
                    /* Which menu and which item, in one number: the two are
                     * always wanted together, and an application reading
                     * `selected / 100` and `% 100` needs no second field. */
                    d->selected = d->open * 100 + item;
                    d->open = -1;
                }
                g_dropped = 0;
                if (d->action) d->action(d, d->user);
                return 1;
            }
            /* Anywhere else shuts it, and the press goes on to whatever it
             * landed on - which is what makes a menu dismiss without eating
             * the click that dismissed it. */
            if (d->kind == UI_POPUP || d->kind == UI_COMBO) d->open = 0;
            else                                            d->open = -1;
            g_dropped = 0;
        }
        struct ui_view* v = hit(root, e->x, e->y);
        g_pressed = v;
        if (v == 0) {
            /* A press on nothing takes the keyboard away from whatever had it,
             * which is what makes clicking the background dismiss a caret. */
            const int had = g_focus != 0;
            g_focus = 0;
            return had;
        }
        if ((v->flags & UI_FOCUSABLE) != 0)
            g_focus = v;

        if (v->kind == UI_CHECK) {
            v->on = !v->on;
            if (v->action) v->action(v, v->user);
        } else if (v->kind == UI_RADIO) {
            /* One of a group: its siblings of the same kind go off. The group
             * is "the radios sharing a parent", which is the arrangement that
             * already says they belong together. */
            if (v->parent != 0)
                for (struct ui_view* s = v->parent->child; s != 0; s = s->next)
                    if (s->kind == UI_RADIO)
                        s->on = (s == v);
            if (v->action) v->action(v, v->user);
        } else if (v->kind == UI_SEGMENTED) {
            const int seg = v->rows > 0
                ? (e->x - v->frame.x) / (v->frame.w / v->rows) : 0;
            if (seg >= 0 && seg < v->rows) {
                v->on = seg;
                if (v->action) v->action(v, v->user);
            }
        } else if (v->kind == UI_LIST || v->kind == UI_SIDEBAR) {
            const int row = v->scroll + (e->y - v->frame.y) / v->row_h;
            if (row >= 0 && row < v->rows) {
                v->selected = row;
                if (v->action) v->action(v, v->user);
            }
        } else if (v->kind == UI_SLIDER) {
            const int span = v->frame.w > 1 ? v->frame.w - 1 : 1;
            int val = (e->x - v->frame.x) * v->max / span;
            if (val < 0) val = 0;
            if (val > v->max) val = v->max;
            v->value = val;
            if (v->action) v->action(v, v->user);
        } else if (v->kind == UI_TOGGLE) {
            v->on = !v->on;
            if (v->action) v->action(v, v->user);
        } else if (v->kind == UI_STEPPER) {
            /* The two ends step, the middle does nothing: a stepper whose
             * number was also a button would fire on every glance at it. */
            const int third = v->frame.w / 3;
            if (e->x < v->frame.x + third && v->value > 0) --v->value;
            else if (e->x > v->frame.x + v->frame.w - third &&
                     v->value < v->max) ++v->value;
            else return 1;
            if (v->action) v->action(v, v->user);
        } else if (v->kind == UI_POPUP) {
            v->open = !v->open;
            g_dropped = v->open ? v : 0;
        } else if (v->kind == UI_TABS) {
            const int seg = v->rows > 0
                ? (e->x - v->frame.x) / (v->frame.w / v->rows) : 0;
            if (seg >= 0 && seg < v->rows) {
                v->on = seg;
                if (v->action) v->action(v, v->user);
            }
        } else if (v->kind == UI_MENUBAR) {
            int at = v->frame.x, picked = -1;
            for (int i = 0; i < v->rows; ++i) {
                const char* t = v->row_text != 0 ? v->row_text(v->user, i) : "";
                const int w = (int)strlen(t) * WG_GLYPH_W + 20;
                if (e->x >= at && e->x < at + w) { picked = i; break; }
                at += w;
            }
            /* Clicking the open one shuts it, which is what a menu bar does
             * everywhere and what stops a title being a one-way switch. */
            v->open = (picked >= 0 && picked == v->open) ? -1 : picked;
            g_dropped = v->open >= 0 ? v : 0;
        } else if (v->kind == UI_TABLE || v->kind == UI_TREE) {
            const int head = (v->kind == UI_TABLE) ? v->row_h : 0;
            const int row = v->scroll + (e->y - v->frame.y - head) / v->row_h;
            if (row >= 0 && row < v->rows) {
                v->selected = row;
                v->hit_branch = 0;
                if (v->kind == UI_TREE && v->branch_of != 0 &&
                    v->branch_of(v->user, row) != 0) {
                    /* The twisty is a target of its own: opening a folder and
                     * selecting it are different intentions, and a tree that
                     * cannot tell them apart opens something every time you
                     * try to look at it. */
                    const int depth = v->depth_of != 0
                                    ? v->depth_of(v->user, row) : 0;
                    const int tx = v->frame.x + 8 + depth * 16;
                    if (e->x >= tx && e->x < tx + 12)
                        v->hit_branch = 1;
                }
                if (v->action) v->action(v, v->user);
            }
        } else if (v->kind == UI_ICONGRID) {
            const int cols = v->frame.w / (v->cell_w > 0 ? v->cell_w : 1);
            if (cols > 0) {
                const int col = (e->x - v->frame.x) / v->cell_w;
                const int row = (e->y - v->frame.y) / v->cell_h + v->scroll;
                const int i = row * cols + col;
                if (col >= 0 && col < cols && i >= 0 && i < v->rows) {
                    v->selected = i;
                    if (v->action) v->action(v, v->user);
                }
            }
        } else if (v->kind == UI_TEXT) {
            /* The caret goes to the line and column that were clicked. */
            if (v->buffer != 0) {
                const int line = v->scroll + (e->y - v->frame.y - 4) / v->row_h;
                const int col = (e->x - v->frame.x - 8) / WG_GLYPH_W;
                int at = 0, l = 0;
                while (v->buffer[at] != '\0' && l < line) {
                    if (v->buffer[at] == '\n') ++l;
                    ++at;
                }
                int c = 0;
                while (v->buffer[at] != '\0' && v->buffer[at] != '\n' && c < col) {
                    ++at; ++c;
                }
                v->caret = at;
            }
        } else if (v->kind == UI_SPLIT) {
            /* Only the divider itself: the panes are children and were hit
             * first, so reaching here means the gap between them. */
            g_pressed = v;
        } else if (v->kind == UI_COMBO) {
            /* The arrow end opens the menu; the rest is the button. Split at
             * the arrow's width so a press near the edge does what it looks
             * like it will do. */
            if (e->x >= v->frame.x + v->frame.w - 24) {
                v->open = !v->open;
                g_dropped = v->open ? v : 0;
            } else {
                v->selected = -1;       /* -1: the button, not an item */
                if (v->action) v->action(v, v->user);
            }
        } else if (v->kind == UI_COLOUR) {
            /* The well opens a mixer under it. Three sliders rather than a
             * tray of swatches: twelve fixed colours are twelve pictures you
             * can paint, and three channels are all of them. */
            v->open = !v->open;
            g_dropped = v->open ? v : 0;
        } else if (v->kind == UI_BROWSER) {
            const int each = v->cols > 0 ? v->frame.w / v->cols : v->frame.w;
            const int col = each > 0 ? (e->x - v->frame.x) / each : 0;
            if (col >= 0 && col < v->cols) {
                const int row = (e->y - v->frame.y) / v->row_h;
                const int n = v->col_count != 0
                            ? v->col_count(v->user, col, v->col_sel) : 0;
                if (row >= 0 && row < n) {
                    v->col_sel[col] = row;
                    /* Everything to the right described the old choice and
                     * describes nothing now. Clearing it is what makes the
                     * columns a path rather than four independent lists. */
                    for (int k = col + 1; k < 4; ++k)
                        v->col_sel[k] = -1;
                    v->selected = col;
                    if (v->action) v->action(v, v->user);
                }
            }
        } else if (v->kind == UI_CALENDAR) {
            /* Which cell, in the grid drawn below the weekday initials. */
            const int cw = v->frame.w / 7;
            const int ch = (v->frame.h - WG_GLYPH_H - 22) / 6;
            if (cw > 0 && ch > 0) {
                const int col = (e->x - v->frame.x) / cw;
                const int row = (e->y - v->frame.y - WG_GLYPH_H - 22) / ch;
                if (col >= 0 && col < 7 && row >= 0 && row < 6) {
                    const int first = ui_first_weekday(v->year, v->month);
                    const int day = row * 7 + col - first + 1;
                    if (day >= 1 && day <= ui_days_in(v->year, v->month)) {
                        v->day = day;
                        if (v->action) v->action(v, v->user);
                    }
                }
            }
        } else if (v->kind == UI_FIELD || v->kind == UI_SECURE ||
                   v->kind == UI_SEARCH) {
            /* The cross at the end of a search field empties it, and says so,
             * because whatever was filtered has to be put back. */
            if (v->kind == UI_SEARCH && v->text[0] != '\0' &&
                e->x >= v->frame.x + v->frame.w - 22) {
                v->text[0] = '\0';
                v->caret = 0;
                if (v->action) v->action(v, v->user);
                return 1;
            }
            /* The caret lands where it was clicked, as near as the glyph width
             * allows. */
            /* Measured the same way it is drawn: walk the string until the
             * text is wider than the click. Dividing by a fixed width put the
             * caret somewhere else entirely on a proportional font. */
            const int want = e->x - v->frame.x - 8;
            const int n = (int)strlen(v->text);
            int at = 0;
            while (at < n && caret_x(v->text, at + 1) <= want)
                ++at;
            v->caret = at;
        }
        /* A view with nothing to do did not take the press. The distinction
         * matters to the application's own handler: a custom view is where it
         * draws its own content and does its own hit-testing, and treating a
         * press there as "a component dealt with it" makes the content dead. */
        switch (v->kind) {
        case UI_CUSTOM:
        case UI_LABEL:
        case UI_SPACER:
        case UI_SEPARATOR:
        case UI_IMAGE:
        case UI_PROGRESS:
        case UI_LEVEL:
        case UI_GROUP:
        case UI_SPINNER:
            return 0;
        default:
            return 1;
        }
    }

    if (e->type == WIN_EVENT_MOUSE_UP) {
        struct ui_view* v = hit(root, e->x, e->y);
        /* A button fires on release over the same button it was pressed on,
         * which is what lets a press be taken back by sliding off it. */
        if (v != 0 && v == g_pressed && v->kind == UI_BUTTON && v->action != 0)
            v->action(v, v->user);
        g_pressed = 0;
        g_channel = -1;
        return 1;
    }

    if (e->type == WIN_EVENT_MOUSE_MOVE && g_channel >= 0 &&
        g_dropped != 0 && g_dropped->kind == UI_COLOUR) {
        struct ui_view* d = g_dropped;
        int px, py, pw, ph;
        colour_panel(d, &px, &py, &pw, &ph);
        const uint32_t was = (uint32_t)d->value;
        d->value = (int)wg_rgb_move(was, g_channel, px + 8, pw - 16, e->x);
        if ((uint32_t)d->value == was)
            return 0;
        if (d->action) d->action(d, d->user);
        return 1;
    }

    if (e->type == WIN_EVENT_MOUSE_MOVE && g_pressed != 0 &&
        g_pressed->kind == UI_SPLIT) {
        struct ui_view* v = g_pressed;
        const int at = (v->layout == UI_STACK_H) ? e->x - v->frame.x
                                                 : e->y - v->frame.y;
        if (at != v->divider) {
            v->divider = at;
            /* Laid out again at once: the panes' frames are what the next
             * event will be hit-tested against, and leaving them stale for a
             * frame is how a drag comes out one step behind the pointer. */
            ui_layout(v, v->frame);
            return 1;
        }
        return 0;
    }

    if (e->type == WIN_EVENT_MOUSE_MOVE && g_pressed != 0 &&
        g_pressed->kind == UI_SLIDER) {
        const struct ui_view* v = g_pressed;
        const int span = v->frame.w > 1 ? v->frame.w - 1 : 1;
        int val = (e->x - v->frame.x) * v->max / span;
        if (val < 0) val = 0;
        if (val > v->max) val = v->max;
        if (val != g_pressed->value) {
            g_pressed->value = val;
            if (g_pressed->action) g_pressed->action(g_pressed, g_pressed->user);
            return 1;
        }
        return 0;
    }

    if (e->type == WIN_EVENT_KEY && g_focus != 0) {
        struct ui_view* v = g_focus;
        if (v->kind == UI_FIELD || v->kind == UI_SECURE ||
            v->kind == UI_SEARCH) {
            const int changed = field_key(v, e->key);
            /* A search reports every keystroke: filtering as you type is the
             * whole difference between it and a field you press Return in. */
            if (changed && v->kind == UI_SEARCH && v->action != 0 &&
                e->key != '\n' && e->key != '\r')
                v->action(v, v->user);
            return changed;
        }
        if (v->kind == UI_TEXT && v->buffer != 0) {
            const int n = (int)strlen(v->buffer);
            if (v->caret > n) v->caret = n;
            if (e->key == '\b') {
                if (v->caret <= 0) return 0;
                memmove(&v->buffer[v->caret - 1], &v->buffer[v->caret],
                        (unsigned)(n - v->caret) + 1);
                --v->caret;
            } else if (e->key == WIN_KEY_LEFT) {
                if (v->caret > 0) --v->caret; else return 0;
            } else if (e->key == WIN_KEY_RIGHT) {
                if (v->caret < n) ++v->caret; else return 0;
            } else if (e->key == WIN_KEY_UP || e->key == WIN_KEY_DOWN) {
                /* By line, keeping the column where it can. Walking the buffer
                 * rather than keeping a line table: a table is a second
                 * structure to invalidate on every keystroke. */
                int line_start = v->caret;
                while (line_start > 0 && v->buffer[line_start - 1] != '\n')
                    --line_start;
                const int col = v->caret - line_start;
                if (e->key == WIN_KEY_UP) {
                    if (line_start == 0) return 0;
                    int prev = line_start - 1;
                    while (prev > 0 && v->buffer[prev - 1] != '\n') --prev;
                    int at = prev;
                    while (at < line_start - 1 && at - prev < col) ++at;
                    v->caret = at;
                } else {
                    int next = v->caret;
                    while (v->buffer[next] != '\0' && v->buffer[next] != '\n')
                        ++next;
                    if (v->buffer[next] == '\0') return 0;
                    ++next;
                    int at = next;
                    while (v->buffer[at] != '\0' && v->buffer[at] != '\n' &&
                           at - next < col) ++at;
                    v->caret = at;
                }
            } else if ((e->key >= ' ' && e->key < 127) || e->key == '\n' ||
                       e->key == '\r') {
                if (n + 1 >= v->cap) return 0;
                const char c = (e->key == '\r') ? '\n' : (char)e->key;
                memmove(&v->buffer[v->caret + 1], &v->buffer[v->caret],
                        (unsigned)(n - v->caret) + 1);
                v->buffer[v->caret++] = c;
            } else return 0;
            return 1;
        }
        if (v->kind == UI_LIST || v->kind == UI_SIDEBAR ||
            v->kind == UI_TABLE || v->kind == UI_TREE ||
            v->kind == UI_ICONGRID) {
            int to = v->selected;
            if (e->key == WIN_KEY_DOWN)      ++to;
            else if (e->key == WIN_KEY_UP)   --to;
            else if (e->key == '\n' || e->key == '\r') {
                if (v->action) v->action(v, v->user);
                return 1;
            } else return 0;
            if (to < 0) to = 0;
            if (to >= v->rows) to = v->rows - 1;
            if (to != v->selected) {
                v->selected = to;
                list_show(v, to);
                if (v->action) v->action(v, v->user);
                return 1;
            }
            return 0;
        }
        if (v->kind == UI_BUTTON && (e->key == '\n' || e->key == '\r')) {
            if (v->action) v->action(v, v->user);
            return 1;
        }
        if (v->kind == UI_CHECK && e->key == ' ') {
            v->on = !v->on;
            if (v->action) v->action(v, v->user);
            return 1;
        }
        if (v->kind == UI_SLIDER &&
            (e->key == WIN_KEY_LEFT || e->key == WIN_KEY_RIGHT)) {
            v->value += (e->key == WIN_KEY_RIGHT) ? 1 : -1;
            if (v->value < 0) v->value = 0;
            if (v->value > v->max) v->value = v->max;
            if (v->action) v->action(v, v->user);
            return 1;
        }
    }
    return 0;
}

/* --- drawing --------------------------------------------------------------- */

/* A colour that suits whichever mode is on.
 *
 * The components were written with flat colours - WG_SHADOW for a switch's
 * track, WG_PAPER behind a progress bar - and a flat grey laid on the glass is
 * a grey rectangle: it covers the blur instead of sitting on it, which is what
 * "some elements turn dark grey" was. On the glass they want a translucent
 * white; opaque, they want the solid colour they always had. */
static uint32_t glass_tint(uint32_t on_glass, uint32_t when_opaque)
{
    return wg_glass_on() ? on_glass : when_opaque;
}

/* Which menu of a bar is being drawn, so the shared item callback knows which
 * one it is answering for. */
static int g_menu_of;

static void draw_dropdown(struct ui_view* v, int x, int y, int w,
                          ui_row_text items, int count, int chosen);

/* The colour well's mixer: under the well, pulled left when that would run it
 * off the window's edge. In one place because the drawing and the dragging
 * both need it and a second copy is a second thing to get wrong. */
#define UI_MIXER_W 236
static void colour_panel(const struct ui_view* v, int* x, int* y, int* w,
                         int* h)
{
    *w = UI_MIXER_W;
    *h = WG_RGB_H + 16;
    *x = v->frame.x;
    *y = v->frame.y + v->frame.h + 4;
    /* The window, which is whatever the root was laid out over. */
    const struct ui_view* root = v;
    while (root->parent != 0)
        root = root->parent;
    const int right = root->frame.x + root->frame.w;
    if (*x + *w > right - 4)
        *x = right - 4 - *w;
    if (*x < root->frame.x + 4)
        *x = root->frame.x + 4;
}

/* Whatever has to be above everything, drawn after everything. One menu at
 * most, which is why this needs no list. */
static void draw_overlay(void)
{
    struct ui_view* v = g_dropped;
    if (v == 0)
        return;
    if (v->kind == UI_COLOUR) {
        int px, py, pw, ph;
        colour_panel(v, &px, &py, &pw, &ph);
        wg_container(px, py, pw, ph, 10);
        wg_rgb_draw(px + 8, py + 8, pw - 16, (uint32_t)v->value);
        return;
    }
    int x, y, w, n;
    if (!dropdown_box(v, &x, &y, &w, &n))
        return;
    if (v->kind == UI_MENUBAR)
        g_menu_of = v->open;
    draw_dropdown(v, x, y, w,
                  v->kind == UI_MENUBAR ? (ui_row_text)v->icon_of : v->row_text,
                  n, v->kind == UI_POPUP ? v->selected : -1);
    if (g_popover != 0 && (g_popover->flags & UI_HIDDEN) == 0)
        ui_draw(g_popover);
}

/* One drop-down: a popup's list of choices, or a menu bar's items. They are the
 * same picture, and were the same picture written twice until this. */
static void draw_dropdown(struct ui_view* v, int x, int y, int w,
                          ui_row_text items, int count, int chosen)
{
    if (count <= 0)
        return;
    const int rh = WG_GLYPH_H + 8;
    wg_container(x - 4, y - 2, w + 8, count * rh + 8, 10);
    for (int i = 0; i < count; ++i) {
        const int ry = y + 2 + i * rh;
        const char* t = items != 0
            ? items(v->user, v->kind == UI_MENUBAR ? g_menu_of * 100 + i : i)
            : "";
        if (t == 0)
            t = "";
        /* A row whose label is "-" is a separator: a line, and not something
         * that can be chosen. It is how a menu groups without needing a second
         * level, and it was being drawn as a row with a hyphen in it. */
        if (ui_is_separator(t)) {
            wg_glass_fill(x + 6, ry + rh / 2, w - 12, 1, 0,
                          glass_tint(0x40FFFFFFu, WG_DIM));
            continue;
        }
        if (i == chosen)
            wg_row_select(x, ry, w, rh);
        wg_text(x + 10, ry + 4, t, wg_ink_colour());
    }
}

/* The background of something you look *into* - a table, a tree, a page of
 * text - as opposed to a panel that floats above the window.
 *
 * wg_container is the floating kind: a wash and a bright line just inside its
 * edge. Used for content it draws that bright line inside whatever box already
 * drew one, and two of them a few pixels apart is the doubled border that
 * showed up on the glass. This is the wash without the edge. */
static void content_surface(const struct ui_rect* f)
{
    wg_glass_fill(f->x, f->y, f->w, f->h, WG_RADIUS,
                  wg_glass_on() ? 0x24FFFFFFu
                                : (0xFF000000u | (wg_body_colour() & 0xFFFFFF)));
}

static void draw_list(struct ui_view* v)
{
    const int sidebar = (v->kind == UI_SIDEBAR);
    if (sidebar)
        wg_sidebar(v->frame.x, v->frame.y, v->frame.w, v->frame.h);
    else
        content_surface(&v->frame);

    const int page = list_rows_visible(v);
    for (int i = 0; i < page; ++i) {
        const int row = v->scroll + i;
        if (row >= v->rows)
            break;
        const int y = v->frame.y + i * v->row_h;
        if (row == v->selected)
            wg_row_select(v->frame.x + 4, y, v->frame.w - 8, v->row_h - 2);
        const char* text = v->row_text != 0 ? v->row_text(v->user, row) : "";
        wg_text_clipped(v->frame.x + 12, y + 4, text != 0 ? text : "",
                        wg_ink_colour(), v->frame.w - 20);
    }
    /* Only when there is more than fits: a bar against a short list is chrome
     * that says nothing. */
    if (v->rows > page)
        wg_scrollbar_v(v->frame.x + v->frame.w - WG_SCROLL_W - 2, v->frame.y,
                       v->frame.h, v->scroll, page, v->rows);
}

void ui_draw(struct ui_view* v)
{
    if (v == 0 || (v->flags & UI_HIDDEN) != 0)
        return;
    const struct ui_rect f = v->frame;
    const int focused = (v == g_focus);

    switch (v->kind) {
    case UI_BOX:
        break;                          /* scenery: its children are the view */
    case UI_LABEL:
        wg_text_clipped(f.x, f.y + 2, v->text, wg_ink_colour(), f.w);
        break;
    case UI_BUTTON:
        wg_button(f.x, f.y, f.w, f.h, v->text, v == g_pressed);
        if (focused)
            wg_fill(f.x + 4, f.y + f.h - 3, f.w - 8, 1, WG_ACCENT);
        break;
    case UI_FIELD: {
        wg_field(f.x, f.y, f.w, f.h, v->text, focused);
        if (focused) {
            /* The caret, where the next character will go. Drawn rather than
             * blinked: a blink needs a timer, and a timer to show where you
             * are typing is a lot of machinery for very little. */
            const int cx = f.x + 8 + caret_x(v->text, v->caret);
            if (cx < f.x + f.w - 4)
                wg_fill(cx, f.y + 5, 1, f.h - 10, wg_ink_colour());
        }
        break;
    }
    case UI_CHECK:
    case UI_RADIO: {
        const int size = 14;
        const int cy = f.y + (f.h - size) / 2;
        if (v->kind == UI_CHECK) wg_check(f.x, cy, size, v->on);
        else                     wg_radio(f.x, cy, size, v->on);
        wg_text_clipped(f.x + size + 8, f.y + (f.h - WG_GLYPH_H) / 2, v->text,
                        wg_ink_colour(), f.w - size - 10);
        if (focused)
            wg_fill(f.x, f.y + f.h - 2, f.w, 1, WG_ACCENT);
        break;
    }
    case UI_SEGMENTED: {
        /* wg_pill_group wants an array; the rows come from a callback, so they
         * are gathered here. Eight is what fits on a line of any window this
         * system draws. */
        const char* labels[8];
        int n = v->rows > 8 ? 8 : v->rows;
        for (int i = 0; i < n; ++i)
            labels[i] = v->row_text != 0 ? v->row_text(v->user, i) : "";
        if (n > 0)
            wg_pill_group(f.x, f.y, f.w / n, f.h, n, labels, v->on);
        break;
    }
    case UI_LIST:
    case UI_SIDEBAR:
        draw_list(v);
        break;
    case UI_SLIDER:
        wg_slider_draw(f.x, f.y, f.w, v->value, v->max);
        break;
    case UI_PROGRESS:
        /* No bevel: a drawn border is what this design does without, and on
         * the glass it came out as a hard grey outline around a pale bar. */
        wg_glass_fill(f.x, f.y, f.w, f.h, f.h / 2,
                      glass_tint(0x2EFFFFFFu, WG_PAPER));
        if (v->max > 0) {
            int fill = (f.w - 2) * v->value / v->max;
            if (fill < 0) fill = 0;
            if (fill > f.w - 2) fill = f.w - 2;
            wg_fill(f.x + 1, f.y + 1, fill, f.h - 2, WG_ACCENT);
        }
        break;
    case UI_SPACER:
        break;
    case UI_CUSTOM:
        /* Clipped to its frame. A view that draws its own content is the one
         * kind that has no idea where the rest of the interface is, and the
         * ones that scroll by the pixel draw a part-row past their own top
         * edge. Painting the chrome again afterwards was the old answer, and
         * it stops working the moment the chrome is a sibling in this tree
         * rather than something drawn last by hand. */
        if (v->draw != 0) {
            wg_clip(f.x, f.y, f.w, f.h);
            v->draw(v, v->user);
            wg_clip_none();
        }
        break;

    case UI_SECURE: {
        /* Bullets, one per character. The length shows, which is what a person
         * needs to see that a keystroke arrived; the characters do not. */
        char dots[UI_TEXT_MAX];
        int n = (int)strlen(v->text);
        if (n > (int)sizeof(dots) - 1) n = (int)sizeof(dots) - 1;
        for (int i = 0; i < n; ++i)
            dots[i] = '*';
        dots[n] = '\0';
        wg_field(f.x, f.y, f.w, f.h, dots, focused);
        if (focused) {
            const int cx = f.x + 8 + caret_x(dots, v->caret);
            if (cx < f.x + f.w - 4)
                wg_fill(cx, f.y + 5, 1, f.h - 10, wg_ink_colour());
        }
        break;
    }

    case UI_SEARCH: {
        const int empty = v->text[0] == '\0';
        wg_field(f.x, f.y, f.w, f.h, empty ? v->col_title[0] : v->text,
                 focused);
        /* A magnifier: a ring and a handle, small enough to read as a hint
         * rather than as a control. */
        const int gx = f.x + 8, gy = f.y + f.h / 2;
        wg_fill(gx + 1, gy - 4, 5, 1, WG_DIM);
        wg_fill(gx + 1, gy + 2, 5, 1, WG_DIM);
        wg_fill(gx, gy - 3, 1, 5, WG_DIM);
        wg_fill(gx + 6, gy - 3, 1, 5, WG_DIM);
        wg_fill(gx + 7, gy + 3, 3, 1, WG_DIM);
        if (!empty) {
            /* And a cross to empty it, only when there is something to empty:
             * a control that does nothing is one to wonder about. */
            const int cx2 = f.x + f.w - 16, cy2 = f.y + f.h / 2;
            for (int i = -3; i <= 3; ++i) {
                wg_fill(cx2 + i, cy2 + i, 1, 1, WG_DIM);
                wg_fill(cx2 + i, cy2 - i, 1, 1, WG_DIM);
            }
        }
        if (focused && !empty) {
            const int cx3 = f.x + 22 + caret_x(v->text, v->caret);
            if (cx3 < f.x + f.w - 20)
                wg_fill(cx3, f.y + 5, 1, f.h - 10, wg_ink_colour());
        }
        break;
    }

    case UI_COMBO: {
        wg_button(f.x, f.y, f.w, f.h, "", v == g_pressed);
        wg_text_clipped(f.x + 12, f.y + (f.h - WG_GLYPH_H) / 2, v->text,
                        wg_ink_colour(), f.w - 40);
        /* A line and an arrow at the end, so the two halves read as two
         * things: the button, and the choices behind it. */
        wg_fill(f.x + f.w - 24, f.y + 4, 1, f.h - 8, WG_DIM);
        for (int i = 0; i < 4; ++i)
            wg_fill(f.x + f.w - 16 + i, f.y + f.h / 2 - 2 + i, 8 - 2 * i, 1,
                    wg_ink_colour());
        break;
    }

    case UI_COLOUR: {
        /* The well: the colour itself, in a frame, so a colour close to the
         * window's own is still visibly a swatch and not a gap. */
        wg_fill(f.x, f.y, f.w, f.h, 0xFF000000u | (uint32_t)v->value);
        wg_glass_outline(f.x, f.y, f.w, f.h, 4, 1,
                         wg_glass_on() ? 0x8CFFFFFFu : 0x40000000u);
        if (focused)
            wg_fill(f.x, f.y + f.h + 1, f.w, 1, WG_ACCENT);
        break;
    }

    case UI_LEVEL: {
        if (v->on > 0) {
            /* Discrete: blocks, of which some are lit. A rating out of five,
             * a signal out of four - things that are counted, not measured. */
            const int gap = 3;
            const int each = (f.w - gap * (v->on - 1)) / v->on;
            const int lit = v->max > 0 ? (v->value * v->on + v->max - 1) / v->max
                                       : 0;
            for (int i = 0; i < v->on; ++i)
                wg_fill(f.x + i * (each + gap), f.y, each, f.h,
                        i < lit ? WG_ACCENT
                                : (wg_glass_on() ? 0x40FFFFFFu : WG_SHADOW));
        } else {
            wg_glass_fill(f.x, f.y, f.w, f.h, f.h / 2,
                          wg_glass_on() ? 0x2EFFFFFFu : WG_PAPER);
            int fill = v->max > 0 ? (f.w - 2) * v->value / v->max : 0;
            if (fill < 0) fill = 0;
            if (fill > f.w - 2) fill = f.w - 2;
            /* Red when nearly empty rather than nearly full: this is a level,
             * and the alarming end of a level is the bottom. */
            wg_fill(f.x + 1, f.y + 1, fill, f.h - 2,
                    (v->max > 0 && v->value * 5 < v->max) ? 0xD2413Au
                                                          : WG_ACCENT);
        }
        break;
    }

    case UI_SPINNER: {
        /* Twelve spokes around a centre, fading behind the lit one. Motion is
         * the message: a bar would claim to know how far along it is. */
        const int cx4 = f.x + f.w / 2, cy4 = f.y + f.h / 2;
        const int r = (f.w < f.h ? f.w : f.h) / 2 - 2;
        static const int kDx[12] = { 0, 1, 2, 2, 2, 1, 0, -1, -2, -2, -2, -1 };
        static const int kDy[12] = { -2, -2, -1, 0, 1, 2, 2, 2, 1, 0, -1, -2 };
        for (int i = 0; i < 12; ++i) {
            const int age = (i - v->value + 12) % 12;
            const unsigned a = age < 6 ? (unsigned)(0xF0 - age * 0x28) : 0x18;
            const int px2 = cx4 + kDx[i] * r / 2;
            const int py2 = cy4 + kDy[i] * r / 2;
            wg_glass_fill(px2 - 1, py2 - 1, 3, 3, 1,
                          (a << 24) | (wg_ink_colour() & 0xFFFFFF));
        }
        break;
    }

    case UI_POPOVER:
        /* A panel with a shadow, over whatever it was raised beside. Its
         * children are drawn by the walk below, as any box's are. */
        wg_glass_fill(f.x, f.y, f.w, f.h, 10,
                      wg_glass_on() ? 0x99FFFFFFu : wg_base_colour());
        wg_glass_outline(f.x, f.y, f.w, f.h, 10, 1,
                         wg_glass_on() ? 0x66FFFFFFu : 0x33000000u);
        break;

    case UI_BROWSER: {
        const int each = v->cols > 0 ? f.w / v->cols : f.w;
        for (int c = 0; c < v->cols; ++c) {
            const int x = f.x + c * each;
            content_surface(&(struct ui_rect){ x, f.y, each - 2, f.h });
            const int n = v->col_count != 0
                        ? v->col_count(v->user, c, v->col_sel) : 0;
            const int page = f.h / v->row_h;
            for (int i = 0; i < n && i < page; ++i) {
                const int y = f.y + i * v->row_h;
                if (i == v->col_sel[c])
                    wg_row_select(x + 2, y, each - 6, v->row_h);
                const char* t = v->col_text != 0
                    ? v->col_text(v->user, c, i, v->col_sel) : "";
                wg_text_clipped(x + 8, y + 2, t != 0 ? t : "",
                                wg_ink_colour(), each - 20);
            }
        }
        break;
    }

    case UI_CALENDAR: {
        static const char* const kDays[7] = { "S","M","T","W","T","F","S" };
        const int cw = f.w / 7;
        for (int d = 0; d < 7; ++d)
            wg_text(f.x + d * cw + (cw - WG_GLYPH_W) / 2, f.y + 2, kDays[d],
                    WG_DIM);
        const int top = f.y + WG_GLYPH_H + 22;
        const int ch = (f.h - WG_GLYPH_H - 22) / 6;
        const int first = ui_first_weekday(v->year, v->month);
        const int count = ui_days_in(v->year, v->month);
        for (int day = 1; day <= count; ++day) {
            const int slot = first + day - 1;
            const int col = slot % 7, row = slot / 7;
            if (row >= 6)
                break;
            const int x = f.x + col * cw, y = top + row * ch;
            if (day == v->day)
                wg_row_select(x + 1, y, cw - 2, ch - 1);
            char num[4];
            snprintf(num, sizeof(num), "%d", day);
            const int tw = (int)strlen(num) * WG_GLYPH_W;
            wg_text(x + (cw - tw) / 2, y + (ch - WG_GLYPH_H) / 2, num,
                    wg_ink_colour());
        }
        break;
    }

    case UI_SEPARATOR:
        /* A hairline down the middle of the room it was given, so the space
         * around it comes from the rule rather than from its neighbours. */
        wg_fill(f.x, f.y + f.h / 2, f.w, 1, WG_DIM);
        break;

    case UI_GROUP:
        wg_text(f.x, f.y, v->text, WG_DIM);
        wg_container(f.x, f.y + WG_GLYPH_H + 2, f.w, f.h - WG_GLYPH_H - 2, 8);
        break;

    case UI_TOGGLE: {
        /* A switch: the knob is at the end it is at, which says the state
         * without a word and without a tick to read. */
        const int tw = 38, th = 18;
        const int ty = f.y + (f.h - th) / 2;
        /* The off state is the one that went grey on the glass. */
        if (v->on)
            wg_fill(f.x, ty, tw, th, WG_ACCENT);
        else
            wg_glass_fill(f.x, ty, tw, th, th / 2,
                          glass_tint(0x33FFFFFFu, WG_SHADOW));
        wg_fill(v->on ? f.x + tw - th + 2 : f.x + 2, ty + 2, th - 4, th - 4,
                WG_PAPER);
        wg_text_clipped(f.x + tw + 10, f.y + (f.h - WG_GLYPH_H) / 2, v->text,
                        wg_ink_colour(), f.w - tw - 12);
        if (focused)
            wg_fill(f.x, f.y + f.h - 2, f.w, 1, WG_ACCENT);
        break;
    }

    case UI_STEPPER: {
        char num[16];
        snprintf(num, sizeof(num), "%d", v->value);
        const int third = f.w / 3;
        wg_button(f.x, f.y, third, f.h, "-", 0);
        wg_button(f.x + f.w - third, f.y, third, f.h, "+", 0);
        const int tw = (int)strlen(num) * WG_GLYPH_W;
        wg_text(f.x + f.w / 2 - tw / 2, f.y + (f.h - WG_GLYPH_H) / 2, num,
                wg_ink_colour());
        break;
    }

    case UI_POPUP: {
        const char* now = (v->row_text != 0 && v->selected >= 0)
                        ? v->row_text(v->user, v->selected) : "";
        wg_button(f.x, f.y, f.w, f.h, "", v->open);
        wg_text_clipped(f.x + 10, f.y + (f.h - WG_GLYPH_H) / 2,
                        now != 0 ? now : "", wg_ink_colour(), f.w - 30);
        /* The arrow, so it reads as something that opens rather than as a
         * button that happens to have a word on it. */
        for (int i = 0; i < 4; ++i)
            wg_fill(f.x + f.w - 16 + i, f.y + f.h / 2 - 2 + i, 8 - 2 * i, 1,
                    wg_ink_colour());
        break;
    }

    case UI_TABS: {
        const int each = v->rows > 0 ? f.w / v->rows : f.w;
        for (int i = 0; i < v->rows; ++i) {
            const int x = f.x + i * each;
            if (i == v->on)
                wg_row_select(x, f.y, each - 2, f.h);
            const char* t = v->row_text != 0 ? v->row_text(v->user, i) : "";
            wg_text_clipped(x + 10, f.y + (f.h - WG_GLYPH_H) / 2,
                            t != 0 ? t : "", wg_ink_colour(), each - 20);
        }
        /* A line under the strip, broken where the chosen tab is: that gap is
         * what joins the tab to what it is showing. */
        wg_fill(f.x, f.y + f.h - 1, f.w, 1, WG_DIM);
        if (v->on >= 0 && v->on < v->rows)
            wg_fill(f.x + v->on * each, f.y + f.h - 1, each - 2, 1,
                    wg_base_colour());
        break;
    }

    case UI_MENUBAR: {
        int at = f.x;
        for (int i = 0; i < v->rows; ++i) {
            const char* t = v->row_text != 0 ? v->row_text(v->user, i) : "";
            const int w = (int)strlen(t != 0 ? t : "") * WG_GLYPH_W + 20;
            if (i == v->open)
                wg_row_select(at, f.y, w, f.h);
            wg_text(at + 10, f.y + (f.h - WG_GLYPH_H) / 2, t != 0 ? t : "",
                    wg_ink_colour());
            at += w;
        }
        break;
    }

    case UI_TABLE: {
        content_surface(&f);
        /* The headings, then a rule, then the rows - and the columns are laid
         * out from the declared widths in both, so a cell cannot land under
         * the wrong heading. */
        int cx = f.x + 10;
        for (int c = 0; c < v->cols; ++c) {
            wg_text_clipped(cx, f.y + 3, v->col_title[c], WG_DIM,
                            v->col_w[c] - 6);
            cx += v->col_w[c];
        }
        wg_fill(f.x + 4, f.y + v->row_h, f.w - 8, 1, WG_DIM);
        const int page = (f.h - v->row_h) / v->row_h;
        for (int i = 0; i < page; ++i) {
            const int row = v->scroll + i;
            if (row >= v->rows)
                break;
            const int y = f.y + v->row_h + i * v->row_h;
            if (row == v->selected)
                wg_row_select(f.x + 4, y, f.w - 8, v->row_h);
            cx = f.x + 10;
            for (int c = 0; c < v->cols; ++c) {
                const char* text = v->cell != 0 ? v->cell(v->user, row, c) : "";
                wg_text_clipped(cx, y + 2, text != 0 ? text : "",
                                wg_ink_colour(), v->col_w[c] - 6);
                cx += v->col_w[c];
            }
        }
        break;
    }

    case UI_TREE: {
        content_surface(&f);
        const int page = f.h / v->row_h;
        for (int i = 0; i < page; ++i) {
            const int row = v->scroll + i;
            if (row >= v->rows)
                break;
            const int y = f.y + i * v->row_h;
            const int depth = v->depth_of != 0 ? v->depth_of(v->user, row) : 0;
            const int branch = v->branch_of != 0 ? v->branch_of(v->user, row) : 0;
            if (row == v->selected)
                wg_row_select(f.x + 4, y, f.w - 8, v->row_h);
            const int tx = f.x + 8 + depth * 16;
            if (branch != 0) {
                /* A twisty: a minus when open, a plus when shut. */
                wg_fill(tx, y + 4, 9, 9, WG_PAPER);
                wg_outline(tx, y + 4, 9, 9, 0);
                wg_fill(tx + 2, y + 8, 5, 1, WG_INK);
                if (branch == 1)
                    wg_fill(tx + 4, y + 6, 1, 5, WG_INK);
            }
            const char* t = v->row_text != 0 ? v->row_text(v->user, row) : "";
            wg_text_clipped(tx + 14, y + 2, t != 0 ? t : "",
                            branch != 0 ? WG_ACCENT : wg_ink_colour(),
                            f.w - (tx - f.x) - 20);
        }
        break;
    }

    case UI_ICONGRID: {
        content_surface(&f);
        const int cols = f.w / (v->cell_w > 0 ? v->cell_w : 1);
        for (int i = 0; i < v->rows && cols > 0; ++i) {
            const int col = i % cols, row = i / cols - v->scroll;
            if (row < 0)
                continue;
            const int x = f.x + col * v->cell_w;
            const int y = f.y + row * v->cell_h;
            if (y + v->cell_h > f.y + f.h)
                break;
            if (i == v->selected)
                wg_row_select(x + 2, y + 2, v->cell_w - 6, v->cell_h - 6);
            const uint32_t* px = v->icon_of != 0 ? v->icon_of(v->user, i) : 0;
            if (px != 0)
                wg_icon(x + (v->cell_w - ICON_SIZE_DEFAULT) / 2, y + 6, px,
                        ICON_SIZE_DEFAULT, ICON_SIZE_DEFAULT);
            const char* t = v->row_text != 0 ? v->row_text(v->user, i) : "";
            wg_text_clipped(x + 6, y + 42, t != 0 ? t : "", wg_ink_colour(),
                            v->cell_w - 12);
        }
        break;
    }

    case UI_TEXT: {
        content_surface(&f);
        if (v->buffer == 0)
            break;
        const int page = (f.h - 8) / v->row_h;
        int at = 0, line = 0;
        /* Skipped rather than measured: finding the first visible line means
         * counting newlines, and counting them is the same walk either way. */
        while (v->buffer[at] != '\0' && line < v->scroll) {
            if (v->buffer[at] == '\n') ++line;
            ++at;
        }
        for (int i = 0; i < page && v->buffer[at] != '\0'; ++i) {
            char text[160];
            unsigned n = 0;
            while (v->buffer[at + n] != '\0' && v->buffer[at + n] != '\n' &&
                   n + 1 < sizeof(text)) {
                text[n] = v->buffer[at + n];
                ++n;
            }
            text[n] = '\0';
            wg_text_clipped(f.x + 8, f.y + 4 + i * v->row_h, text,
                            wg_ink_colour(), f.w - 16);
            /* The caret, when it is on this line. */
            if (focused && v->caret >= at && v->caret <= at + (int)n) {
                const int cx2 = f.x + 8 + caret_x(text, v->caret - at);
                wg_fill(cx2, f.y + 4 + i * v->row_h, 1, v->row_h,
                        wg_ink_colour());
            }
            at += n + (v->buffer[at + n] == '\n' ? 1 : 0);
        }
        break;
    }

    case UI_SCROLL:
        /* The child is drawn by the walk below; this draws the bar beside it. */
        if (v->child != 0 && v->child->frame.h > f.h)
            wg_scrollbar_v(f.x + f.w - WG_SCROLL_W, f.y, f.h, v->scroll,
                           f.h, v->child->frame.h);
        break;

    case UI_SPLIT: {
        /* Clamped to the split, not just clipped by the window.
         *
         * The divider is drawn from `divider`, which the drag writes straight
         * from the pointer - so dragging past the edge drew the grip outside
         * the split, over whatever was beside it. Layout clamps the same value
         * afterwards, which meant the picture and the position disagreed for
         * exactly as long as the mouse was down. */
        int at = v->divider;
        const int span = (v->layout == UI_STACK_H) ? f.w : f.h;
        if (at < 0) at = 0;
        if (at > span - DIVIDER_W) at = span - DIVIDER_W;
        if (at < 0) break;
        /* A pair of hairlines rather than a filled bar: a solid block of the
         * selection colour between two panes reads as a selected thing. */
        if (v->layout == UI_STACK_H) {
            wg_fill(f.x + at + 2, f.y, 1, f.h, WG_DIM);
            wg_fill(f.x + at + 3, f.y, 1, f.h,
                    glass_tint(0x33FFFFFFu, WG_LIGHT));
        } else {
            wg_fill(f.x, f.y + at + 2, f.w, 1, WG_DIM);
            wg_fill(f.x, f.y + at + 3, f.w, 1,
                    glass_tint(0x33FFFFFFu, WG_LIGHT));
        }
        break;
    }

    case UI_IMAGE:
        if (v->user != 0)
            wg_icon_scaled(f.x, f.y, (const uint32_t*)v->user,
                           ICON_SIZE_DEFAULT, ICON_SIZE_DEFAULT, f.w, f.h);
        break;

    default:
        break;
    }

    for (struct ui_view* c = v->child; c != 0; c = c->next)
        if (c->kind != UI_POPOVER)
            ui_draw(c);

    /* The open drop-down is not drawn here.
     *
     * It was, on the way out of the view that owns it - which is above that
     * view's own children and above nothing else. The tree is walked in the
     * order things sit, so every sibling after the menu was painted straight
     * over it, and a menu near the top of a window disappeared behind the
     * whole window. It goes last instead, after the entire tree, which is the
     * only place that is above all of it. */
    if (v->parent == 0)
        draw_overlay();
}
