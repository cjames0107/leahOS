/* The text engine. See textedit.h for what it is and why the buffer is not
 * its own.
 *
 * Everything that changes the text goes through replace(): one function that
 * takes a range out, puts something in, and writes down what it did. Undo is
 * therefore not a feature bolted on afterwards but the only way this file can
 * modify anything, which is what stops an operation being added later that
 * quietly cannot be undone.
 */

#include <clipboard.h>
#include <stdlib.h>
#include <string.h>
#include <textedit.h>
#include <window.h>

/* --- the history -----------------------------------------------------------
 *
 * A ring of edits, each holding what was taken out and what was put in, so it
 * can be run either way. Sixty-four is a compromise nobody will notice: an
 * undo that reaches sixty-five steps back is a person who wants the file they
 * started with, and that is what closing without saving is for.
 */
#define TE_UNDO_MAX 64

struct te_edit {
    int   at;
    char* removed;      /* what was there; 0 for an insertion   */
    char* inserted;     /* what replaced it; 0 for a deletion   */
};

struct te_history {
    struct te_edit e[TE_UNDO_MAX];
    int n;              /* how many are recorded                */
    int at;             /* how many have been done: undo moves it down */
    /* Typing is one edit, not one edit per letter. A run of insertions at the
     * caret is merged into the last record until something interrupts it -
     * a move, a delete, a paste - because undoing a sentence a character at a
     * time is not undo, it is rewinding. */
    int open;
};

static char* dup_range(const char* s, int n)
{
    if (n <= 0)
        return 0;
    char* out = (char*)malloc((unsigned long)n + 1);
    if (out == 0)
        return 0;
    memcpy(out, s, (unsigned long)n);
    out[n] = '\0';
    return out;
}

static void drop_from(struct te_history* h, int at)
{
    /* Anything that had been undone is unreachable once something new is
     * typed: there is one past, and this is now it. */
    for (int i = at; i < h->n; ++i) {
        free(h->e[i].removed);
        free(h->e[i].inserted);
        h->e[i].removed = h->e[i].inserted = 0;
    }
    h->n = at;
}

static struct te_history* history_of(struct textedit* t)
{
    if (t->history == 0) {
        t->history = (struct te_history*)malloc(sizeof(struct te_history));
        if (t->history != 0)
            memset(t->history, 0, sizeof(struct te_history));
    }
    return t->history;
}

void te_forget(struct textedit* t)
{
    if (t == 0 || t->history == 0)
        return;
    drop_from(t->history, 0);
    free(t->history);
    t->history = 0;
}

/* --- the one mutation ------------------------------------------------------ */

static int length_of(const struct textedit* t)
{
    return (int)strlen(t->text);
}

static void clamp(struct textedit* t)
{
    const int n = length_of(t);
    if (t->caret < 0) t->caret = 0;
    if (t->caret > n) t->caret = n;
    if (t->anchor < 0) t->anchor = 0;
    if (t->anchor > n) t->anchor = n;
}

/* Take [from, to) out and put `with` in its place. `join` asks for this to be
 * merged into the previous record when it can be. */
static int replace(struct textedit* t, int from, int to,
                   const char* with, int with_n, int join)
{
    const int n = length_of(t);
    if (from < 0) from = 0;
    if (to > n) to = n;
    if (to < from) { const int s = from; from = to; to = s; }
    if (with_n < 0) with_n = 0;
    const int removed_n = to - from;
    if (removed_n == 0 && with_n == 0)
        return 0;
    /* The NUL has to fit as well as the text. */
    if (n - removed_n + with_n + 1 > t->cap)
        return 0;

    struct te_history* h = history_of(t);
    char* removed = dup_range(&t->text[from], removed_n);
    char* inserted = dup_range(with, with_n);

    memmove(&t->text[from + with_n], &t->text[to],
            (unsigned long)(n - to) + 1);
    if (with_n > 0)
        memcpy(&t->text[from], with, (unsigned long)with_n);

    t->caret = from + with_n;
    t->anchor = t->caret;

    if (h == 0) {
        free(removed);
        free(inserted);
        return 1;               /* the edit stands; only its record is lost */
    }

    /* Merged into the record before it when this is more of the same typing:
     * an insertion, with nothing removed, landing exactly where the last one
     * finished. */
    if (join && h->open && h->at == h->n && h->n > 0 && removed_n == 0) {
        struct te_edit* last = &h->e[h->n - 1];
        if (last->removed == 0 && last->inserted != 0) {
            const int had = (int)strlen(last->inserted);
            if (last->at + had == from) {
                char* grown = (char*)malloc((unsigned long)had + with_n + 1);
                if (grown != 0) {
                    memcpy(grown, last->inserted, (unsigned long)had);
                    memcpy(&grown[had], with, (unsigned long)with_n);
                    grown[had + with_n] = '\0';
                    free(last->inserted);
                    last->inserted = grown;
                    free(removed);
                    free(inserted);
                    return 1;
                }
            }
        }
    }

    drop_from(h, h->at);
    if (h->n == TE_UNDO_MAX) {
        /* The oldest goes, and everything shuffles down. A ring would save the
         * copying; sixty-four pointers moved on the rare occasion the history
         * is full is not worth the arithmetic of one. */
        free(h->e[0].removed);
        free(h->e[0].inserted);
        memmove(&h->e[0], &h->e[1], sizeof(h->e[0]) * (TE_UNDO_MAX - 1));
        --h->n;
    }
    h->e[h->n].at = from;
    h->e[h->n].removed = removed;
    h->e[h->n].inserted = inserted;
    ++h->n;
    h->at = h->n;
    h->open = join;
    return 1;
}

/* --- selection -------------------------------------------------------------- */

int te_selection(const struct textedit* t, int* from, int* to)
{
    const int a = t->caret < t->anchor ? t->caret : t->anchor;
    const int b = t->caret < t->anchor ? t->anchor : t->caret;
    if (from != 0) *from = a;
    if (to != 0)   *to = b;
    return b > a;
}

void te_select_all(struct textedit* t)
{
    t->anchor = 0;
    t->caret = length_of(t);
}

static int delete_selection(struct textedit* t)
{
    int from, to;
    if (!te_selection(t, &from, &to))
        return 0;
    return replace(t, from, to, "", 0, 0);
}

void te_set(struct textedit* t, const char* s)
{
    const int n = s != 0 ? (int)strlen(s) : 0;
    replace(t, 0, length_of(t), s != 0 ? s : "", n, 0);
}

int te_insert(struct textedit* t, const char* s, int n)
{
    /* One edit, not two.
     *
     * Taking the selection out and then putting the text in is two records,
     * and one undo then puts back neither the selection nor half of what
     * replaced it - it removes the insertion and leaves the hole. Replacing a
     * range *is* the operation; it is what replace() takes. */
    int from, to;
    if (te_selection(t, &from, &to))
        return replace(t, from, to, s, n, 0);
    return replace(t, t->caret, t->caret, s, n, 0);
}

/* --- moving around ---------------------------------------------------------- */

static int is_word(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static int word_left(const struct textedit* t, int at)
{
    while (at > 0 && !is_word(t->text[at - 1])) --at;
    while (at > 0 && is_word(t->text[at - 1])) --at;
    return at;
}

static int word_right(const struct textedit* t, int at)
{
    const int n = length_of(t);
    while (at < n && !is_word(t->text[at])) ++at;
    while (at < n && is_word(t->text[at])) ++at;
    return at;
}

static int line_start(const struct textedit* t, int at)
{
    while (at > 0 && t->text[at - 1] != '\n') --at;
    return at;
}

static int line_end(const struct textedit* t, int at)
{
    const int n = length_of(t);
    while (at < n && t->text[at] != '\n') ++at;
    return at;
}

/* The same column on the line above or below, in characters. A proportional
 * font makes "the same column" a lie either way; this is the answer that does
 * not need the caller's font. */
static int line_step(const struct textedit* t, int at, int down)
{
    const int start = line_start(t, at);
    const int col = at - start;
    if (down) {
        const int end = line_end(t, at);
        if (end >= length_of(t))
            return at;
        const int next = end + 1;
        const int next_end = line_end(t, next);
        return next + col < next_end ? next + col : next_end;
    }
    if (start == 0)
        return at;
    const int prev = line_start(t, start - 1);
    return prev + col < start - 1 ? prev + col : start - 1;
}

/* --- undo ------------------------------------------------------------------- */

static int step_history(struct textedit* t, int back)
{
    struct te_history* h = t->history;
    if (h == 0)
        return 0;
    h->open = 0;
    if (back) {
        if (h->at == 0)
            return 0;
        const struct te_edit* e = &h->e[--h->at];
        const int put_back = e->removed != 0 ? (int)strlen(e->removed) : 0;
        const int take_out = e->inserted != 0 ? (int)strlen(e->inserted) : 0;
        const int n = length_of(t);
        if (e->at + take_out > n)
            return 0;                   /* the text moved under us */
        memmove(&t->text[e->at + put_back], &t->text[e->at + take_out],
                (unsigned long)(n - e->at - take_out) + 1);
        if (put_back > 0)
            memcpy(&t->text[e->at], e->removed, (unsigned long)put_back);
        t->caret = t->anchor = e->at + put_back;
        return 1;
    }
    if (h->at >= h->n)
        return 0;
    const struct te_edit* e = &h->e[h->at++];
    const int take_out = e->removed != 0 ? (int)strlen(e->removed) : 0;
    const int put_in = e->inserted != 0 ? (int)strlen(e->inserted) : 0;
    const int n = length_of(t);
    if (e->at + take_out > n)
        return 0;
    memmove(&t->text[e->at + put_in], &t->text[e->at + take_out],
            (unsigned long)(n - e->at - take_out) + 1);
    if (put_in > 0)
        memcpy(&t->text[e->at], e->inserted, (unsigned long)put_in);
    t->caret = t->anchor = e->at + put_in;
    return 1;
}

/* --- the clipboard ---------------------------------------------------------- */

static int copy_out(struct textedit* t, int cut)
{
    int from, to;
    if (!te_selection(t, &from, &to))
        return 0;
    /* A password is not for reading. Cutting is refused for the same reason
     * copying is: the point of the field is that what is in it does not leave
     * by any route the person did not intend. */
    if ((t->flags & TE_SECURE) != 0)
        return 0;
    clip_put(&t->text[from], (unsigned)(to - from));
    if (!cut)
        return 0;               /* nothing changed, so nothing to redraw */
    return replace(t, from, to, "", 0, 0);
}

static int paste_in(struct textedit* t)
{
    char buf[CLIP_MAX];
    const int n = clip_get(buf, sizeof(buf));
    if (n <= 0)
        return 0;
    int end = 0;
    while (end < n && buf[end] != '\0') {
        /* A newline pasted into a single-line field would make a field that
         * cannot show what it holds. */
        if (buf[end] == '\n' && (t->flags & TE_MULTILINE) == 0)
            break;
        ++end;
    }
    int from, to;
    if (te_selection(t, &from, &to))
        return replace(t, from, to, buf, end, 0);
    return replace(t, t->caret, t->caret, buf, end, 0);
}

/* --- keys -------------------------------------------------------------------- */

int te_key(struct textedit* t, unsigned key, unsigned mods)
{
    if (t == 0 || t->text == 0 || t->cap <= 0)
        return 0;
    clamp(t);

    const int shift = (mods & WIN_MOD_SHIFT) != 0;
    const int ctrl  = (mods & WIN_MOD_CTRL) != 0;
    const int n = length_of(t);

    /* Moving: the anchor follows the caret unless shift is held, which is the
     * whole of what "extend the selection" means. */
    int to = -1;
    if (key == WIN_KEY_LEFT)
        to = ctrl ? word_left(t, t->caret) : (t->caret > 0 ? t->caret - 1 : 0);
    else if (key == WIN_KEY_RIGHT)
        to = ctrl ? word_right(t, t->caret) : (t->caret < n ? t->caret + 1 : n);
    else if (key == WIN_KEY_UP)
        to = ctrl ? 0 : ((t->flags & TE_MULTILINE) ? line_step(t, t->caret, 0)
                                                   : line_start(t, t->caret));
    else if (key == WIN_KEY_DOWN)
        to = ctrl ? n : ((t->flags & TE_MULTILINE) ? line_step(t, t->caret, 1)
                                                   : line_end(t, t->caret));
    if (to >= 0) {
        /* An unshifted arrow with something selected collapses to the near
         * end of it rather than moving from the caret, which is what every
         * text field does and what stops an arrow key losing your place. */
        int from, until;
        if (!shift && te_selection(t, &from, &until))
            to = (key == WIN_KEY_LEFT || key == WIN_KEY_UP) ? from : until;
        const int moved = (t->caret != to) || (!shift && t->anchor != t->caret);
        t->caret = to;
        if (!shift)
            t->anchor = to;
        if (t->history != 0)
            t->history->open = 0;
        return moved;
    }

    if (ctrl) {
        switch (key) {
        case 1:  te_select_all(t); return 1;            /* ctrl+A */
        case 3:  return copy_out(t, 0);                 /* ctrl+C */
        case 24: return copy_out(t, 1);                 /* ctrl+X */
        case 22: return paste_in(t);                    /* ctrl+V */
        case 26: return step_history(t, 1);             /* ctrl+Z */
        case 25: return step_history(t, 0);             /* ctrl+Y */
        default: break;
        }
    }

    if (key == '\b') {
        if (delete_selection(t))
            return 1;
        if (t->caret <= 0)
            return 0;
        const int from = ctrl ? word_left(t, t->caret) : t->caret - 1;
        return replace(t, from, t->caret, "", 0, 0);
    }
    if (key == 0x7F) {                                  /* forward delete */
        if (delete_selection(t))
            return 1;
        if (t->caret >= n)
            return 0;
        const int until = ctrl ? word_right(t, t->caret) : t->caret + 1;
        return replace(t, t->caret, until, "", 0, 0);
    }
    if (key == '\n' || key == '\r') {
        if ((t->flags & TE_MULTILINE) == 0)
            return 0;           /* the caller decides what Return means */
        return te_insert(t, "\n", 1);
    }
    if (key == '\t' && (t->flags & TE_MULTILINE) != 0)
        return te_insert(t, "\t", 1);
    if (key >= ' ' && key < 127) {
        const char c = (char)key;
        int from, to;
        if (te_selection(t, &from, &to)) {
            /* Not joined to what came before: this edit began by removing
             * something, so it is the start of a new one. */
            return replace(t, from, to, &c, 1, 0);
        }
        /* Joined to the last insertion, so a word undoes as a word. */
        return replace(t, t->caret, t->caret, &c, 1, 1);
    }
    return 0;
}

/* --- where a click lands ----------------------------------------------------- */

int te_index_at(const struct textedit* t, int start, int x,
                te_measure measure, void* user)
{
    if (measure == 0)
        return start;
    const int end = line_end(t, start);
    int at = start;
    /* Walked rather than divided: the glyphs are not one width, so the column
     * is where the text stops being narrower than the point. */
    while (at < end &&
           measure(user, &t->text[start], at - start + 1) <= x)
        ++at;
    return at;
}
