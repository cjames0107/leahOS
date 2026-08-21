#ifndef _TEXTEDIT_H
#define _TEXTEDIT_H

/* Editing text: a caret, a selection, the clipboard, and undo.
 *
 * Every field in this system used to carry its own three lines of "insert at
 * the caret, back up on backspace", which is why none of them could select,
 * none could copy, and nothing anywhere could be undone. The behaviour a
 * person expects from a text field is not three lines - it is the whole of
 * this file - and it is the same behaviour in a search box, a password field,
 * a document and a spreadsheet cell, so it is written once.
 *
 * The buffer belongs to the caller. This does not allocate the text, grow it,
 * or decide where it lives: a field's text is inside the view, a document's is
 * a megabyte the application malloc'd, and an engine that insisted on owning
 * it would be unusable for one of the two.
 *
 * What it does own is the undo history, because that is the one piece of state
 * a caller has no reason to know the shape of.
 */

#define TE_MULTILINE 1u     /* Return inserts a newline instead of accepting */
#define TE_SECURE    2u     /* no copying out: a password is not for reading */

struct te_history;

struct textedit {
    char* text;             /* the caller's buffer, always NUL-terminated */
    int   cap;              /* its size, the NUL included                 */
    int   caret;            /* where the next character goes              */
    int   anchor;           /* the other end of the selection             */
    unsigned flags;
    struct te_history* history;     /* owned here; te_forget frees it     */
};

/* One keystroke. Returns 1 if the text or the caret moved, which is what a
 * caller needs to know to redraw. `mods` is WIN_MOD_SHIFT / WIN_MOD_CTRL.
 *
 * What it understands:
 *
 *   printable          insert, replacing the selection
 *   backspace          the selection, or the character before
 *   ctrl+backspace     the word before
 *   left / right       move; with ctrl by a word; with shift, select
 *   up / down          the line above or below, when multiline;
 *                      with ctrl, the start or end of the whole text
 *   ctrl+A             select everything
 *   ctrl+C / X / V     copy, cut, paste
 *   ctrl+Z / ctrl+Y    undo, redo
 *   return             a newline when multiline; otherwise not handled, so
 *                      the caller can treat it as "the field was accepted"
 */
int te_key(struct textedit* t, unsigned key, unsigned mods);

/* Is anything selected, and what? Returns 1 when from != to. */
int te_selection(const struct textedit* t, int* from, int* to);

void te_select_all(struct textedit* t);

/* Replace everything, as a recorded edit - so setting a field's text from the
 * application is undoable like anything else. */
void te_set(struct textedit* t, const char* s);

/* Insert at the caret, replacing the selection. `n` bytes, not a string. */
int te_insert(struct textedit* t, const char* s, int n);

/* Where a click lands, given the width of the text before each character.
 * Kept here so that a caret is placed the same way everywhere: the caller says
 * how to measure and this walks. */
typedef int (*te_measure)(void* user, const char* text, int n);
int te_index_at(const struct textedit* t, int line_start, int x,
                te_measure measure, void* user);

/* Let the undo history go. The text is the caller's and is left alone. */
void te_forget(struct textedit* t);

#endif /* _TEXTEDIT_H */
