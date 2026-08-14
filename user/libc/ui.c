/* The component layer: layout, routing, drawing. See <ui.h> for why.
 *
 * The three passes never disagree about geometry because only one of them
 * decides it. ui_layout writes every frame; ui_event and ui_draw read them.
 * That is the whole reason a control can no longer be drawn in one place and
 * hit in another, which is the bug this exists to make unwritable.
 */

#include <ui.h>
#include <stdio.h>
#include <string.h>

static struct ui_view g_pool[UI_MAX_VIEWS];
static int g_used;
static struct ui_view* g_focus;
static struct ui_view* g_pressed;

void ui_reset(void)
{
    g_used = 0;
    g_focus = 0;
    g_pressed = 0;
}

static struct ui_view* alloc_view(struct ui_view* parent, int kind)
{
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
    if (v->kind != UI_BOX)
        return;

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

    int visible = 0, fixed = 0, weight = 0;
    for (struct ui_view* c = v->child; c != 0; c = c->next) {
        if ((c->flags & UI_HIDDEN) != 0)
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
        if ((c->flags & UI_HIDDEN) != 0)
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
        } else if (v->kind == UI_FIELD) {
            /* The caret lands where it was clicked, as near as the glyph width
             * allows. */
            int at = (e->x - v->frame.x - 8) / WG_GLYPH_W;
            const int n = (int)strlen(v->text);
            v->caret = at < 0 ? 0 : (at > n ? n : at);
        }
        return 1;
    }

    if (e->type == WIN_EVENT_MOUSE_UP) {
        struct ui_view* v = hit(root, e->x, e->y);
        /* A button fires on release over the same button it was pressed on,
         * which is what lets a press be taken back by sliding off it. */
        if (v != 0 && v == g_pressed && v->kind == UI_BUTTON && v->action != 0)
            v->action(v, v->user);
        g_pressed = 0;
        return 1;
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
        if (v->kind == UI_FIELD)
            return field_key(v, e->key);
        if (v->kind == UI_LIST || v->kind == UI_SIDEBAR) {
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

static void draw_list(struct ui_view* v)
{
    const int sidebar = (v->kind == UI_SIDEBAR);
    if (sidebar)
        wg_sidebar(v->frame.x, v->frame.y, v->frame.w, v->frame.h);
    else
        wg_container(v->frame.x, v->frame.y, v->frame.w, v->frame.h, 6);

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
            const int cx = f.x + 8 + v->caret * WG_GLYPH_W;
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
        wg_fill(f.x, f.y, f.w, f.h, WG_PAPER);
        wg_bevel(f.x, f.y, f.w, f.h, 0);
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
        if (v->draw != 0)
            v->draw(v, v->user);
        break;
    default:
        break;
    }

    for (struct ui_view* c = v->child; c != 0; c = c->next)
        ui_draw(c);
}
